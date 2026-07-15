#include "mail/smtp/session.hpp"

#include <string>
#include <string_view>
#include <utility>

// SMTP 会话状态机的实现。分派逻辑严格对照 RFC 5321 的命令时序，并把 SMTP smuggling
// 相关的分帧安全（裸 CR/LF 终止、超长行 500、DATA 阶段终止行不可被超长行误吞）交由
// 下层 LineReader 的语义保证与本层的显式长度自查共同兜住。

namespace mail::smtp {

namespace {

// 命令行剥除 CRLF 后允许的最大字节数。kMaxCommandLineOctets 含尾部 CRLF，故此处减 2。
constexpr std::size_t kMaxStrippedCommandOctets = kMaxCommandLineOctets - 2;

Reply parseErrorReply(ParseError error) {
    switch (error) {
        case ParseError::Syntax:
            return Reply::single(501, "Syntax error in parameters or arguments");
        case ParseError::PathTooLong:
            return Reply::single(501, "Path too long");
        case ParseError::BadParam:
            return Reply::single(555, "Unrecognized or unimplemented parameter");
        case ParseError::None:
            break;
    }
    // None 不应走到这里；给出保守的语法错误应答以保持函数总有返回。
    return Reply::single(501, "Syntax error in parameters or arguments");
}

}  // namespace

Session::Session(net::LineReader& reader, net::ByteSink& writer, MessageSink& sink,
                 RecipientVerifier& verifier, SessionConfig config)
    : reader_(reader),
      writer_(writer),
      sink_(sink),
      verifier_(verifier),
      config_(std::move(config)) {}

void Session::resetTransaction() { envelope_ = Envelope{}; }

IoStatus Session::sendReply(const Reply& reply) {
    return writer_.writeAll(serialize(reply));
}

Reply Session::onHeloEhlo(const Command& command) {
    // HELO/EHLO 在任意状态都合法：重置任何进行中的事务并回到 Greeted。
    resetTransaction();
    state_ = State::Greeted;

    if (command.verb == Verb::Ehlo) {
        Reply reply;
        reply.code = 250;
        reply.lines.push_back(config_.serverDomain);
        reply.lines.push_back("PIPELINING");
        reply.lines.push_back("SIZE " + std::to_string(config_.maxMessageOctets));
        reply.lines.push_back("8BITMIME");
        return reply;
    }
    return Reply::single(250, config_.serverDomain);
}

Reply Session::onMail(const Command& command) {
    if (state_ != State::Greeted) {
        return Reply::single(503, "Bad sequence of commands");
    }
    if (command.declaredSize > config_.maxMessageOctets) {
        return Reply::single(552, "Message size exceeds fixed maximum message size");
    }
    // 开启新事务：记录发信人与宣告参数。sender 为空串即 <> 退信路径。
    envelope_ = Envelope{};
    envelope_.sender = command.path;
    envelope_.declaredSize = command.declaredSize;
    envelope_.eightBitMime = (command.body == BodyType::EightBitMime);
    state_ = State::MailGiven;
    return Reply::single(250, "OK");
}

Reply Session::onRcpt(const Command& command) {
    if (state_ != State::MailGiven && state_ != State::RcptGiven) {
        return Reply::single(503, "Bad sequence of commands");
    }
    if (envelope_.recipients.size() >= config_.maxRecipients) {
        return Reply::single(452, "Too many recipients");
    }
    switch (verifier_.verify(command.path)) {
        case RcptDecision::Accept:
            envelope_.recipients.push_back(command.path);
            state_ = State::RcptGiven;
            return Reply::single(250, "OK");
        case RcptDecision::RejectPermanent:
            return Reply::single(550, "Mailbox unavailable");
        case RcptDecision::RejectTemporary:
            return Reply::single(450, "Mailbox unavailable, try again later");
    }
    // 所有枚举分支均已返回；此处不可达。
    return Reply::single(550, "Mailbox unavailable");
}

IoStatus Session::collectData(Reply& outReply) {
    std::string data;
    bool poisoned = false;   // 累积超限或遇超长行后置真，停止累积但继续排空到终止行
    Reply poisonReply;       // 毒化发生时记下的 552，供终止行处直接回给客户端
    std::string line;

    while (true) {
        IoStatus status = reader_.readLine(line);
        if (status == IoStatus::Timeout) {
            return IoStatus::Timeout;  // 由 run() 回 421
        }
        if (status == IoStatus::Closed || status == IoStatus::Error) {
            return status;  // 由 run() 直接结束，不回码
        }
        if (status == IoStatus::LineTooLong) {
            // 超长的数据行：毒化并继续。LineReader 会自动排空这一行直到其 CRLF，之后
            // 独立成行的终止 "." 仍会作为正常 Ok 行到达，不被误吞。
            if (!poisoned) {
                poisoned = true;
                poisonReply = Reply::single(552, "Message exceeds maximum size");
            }
            continue;
        }
        if (status != IoStatus::Ok) {
            return status;  // WouldBlock/Interrupted 等：防御性上抛
        }

        // 独立成行的 "." 是数据终止符（此判断先于 dot-stuffing 去点）。
        if (line == ".") {
            if (poisoned) {
                outReply = poisonReply;
            } else {
                switch (sink_.deliver(envelope_, data)) {
                    case SinkStatus::Ok:
                        outReply = Reply::single(250, "OK");
                        break;
                    case SinkStatus::TransientFail:
                        outReply = Reply::single(451,
                            "Requested action aborted: local error in processing");
                        break;
                    case SinkStatus::PermanentFail:
                        outReply = Reply::single(554, "Transaction failed");
                        break;
                }
            }
            resetTransaction();
            return IoStatus::Ok;
        }

        if (poisoned) {
            continue;  // 已超限，仅排空到终止行，不再累积
        }

        // 去 dot-stuffing：行首多余的 '.' 去掉一个。
        std::string_view content = line;
        if (!content.empty() && content.front() == '.') {
            content.remove_prefix(1);
        }

        // 累积（含补回的 CRLF）若将超过上限则毒化，停止累积。
        if (data.size() + content.size() + 2 > config_.maxMessageOctets) {
            poisoned = true;
            poisonReply = Reply::single(552, "Message exceeds maximum size");
            continue;
        }
        data.append(content.data(), content.size());
        data.append("\r\n");
    }
}

IoStatus Session::run() {
    // 问候：单行 220，必须含服务器域名。
    IoStatus status = sendReply(
        Reply::single(220, config_.serverDomain + " Service ready"));
    if (status != IoStatus::Ok) {
        return status;
    }

    std::string line;
    while (true) {
        IoStatus readStatus = reader_.readLine(line);
        if (readStatus == IoStatus::Timeout) {
            sendReply(Reply::single(421,
                config_.serverDomain + " Timeout, closing connection"));
            return IoStatus::Timeout;
        }
        if (readStatus == IoStatus::Closed) {
            return IoStatus::Closed;
        }
        if (readStatus == IoStatus::Error) {
            // 裸 CR/LF 分帧违规（终止性）或底层错误：回 500 后结束，防 SMTP smuggling。
            sendReply(Reply::single(500, "Syntax error, command unrecognized"));
            return IoStatus::Error;
        }
        if (readStatus == IoStatus::LineTooLong) {
            // LineReader 已进入丢弃模式自动排空这一行；本层只需回 500 后继续循环。
            status = sendReply(Reply::single(500, "Line too long"));
            if (status != IoStatus::Ok) {
                return status;
            }
            continue;
        }
        if (readStatus != IoStatus::Ok) {
            return readStatus;  // WouldBlock/Interrupted 等：防御性结束
        }

        // 命令行长度自查：LineReader 以 kMaxDataWireLineOctets 分帧，511..999 字节的
        // 命令行会以 Ok 到达，必须在此拒绝超过命令行上限者。
        if (line.size() > kMaxStrippedCommandOctets) {
            status = sendReply(Reply::single(500, "Line too long"));
            if (status != IoStatus::Ok) {
                return status;
            }
            continue;
        }

        Command command = parseCommand(line);

        // ParseError 先于一切（含 503 状态检查）处理。
        if (command.error != ParseError::None) {
            status = sendReply(parseErrorReply(command.error));
            if (status != IoStatus::Ok) {
                return status;
            }
            continue;
        }

        switch (command.verb) {
            case Verb::Helo:
            case Verb::Ehlo: {
                status = sendReply(onHeloEhlo(command));
                if (status != IoStatus::Ok) {
                    return status;
                }
                break;
            }
            case Verb::Mail: {
                status = sendReply(onMail(command));
                if (status != IoStatus::Ok) {
                    return status;
                }
                break;
            }
            case Verb::Rcpt: {
                status = sendReply(onRcpt(command));
                if (status != IoStatus::Ok) {
                    return status;
                }
                break;
            }
            case Verb::Data: {
                if (state_ != State::RcptGiven) {
                    status = sendReply(Reply::single(503, "Bad sequence of commands"));
                    if (status != IoStatus::Ok) {
                        return status;
                    }
                    break;
                }
                status = sendReply(Reply::single(354,
                    "Start mail input; end with <CRLF>.<CRLF>"));
                if (status != IoStatus::Ok) {
                    return status;
                }
                Reply dataReply;
                IoStatus collectStatus = collectData(dataReply);
                if (collectStatus == IoStatus::Timeout) {
                    sendReply(Reply::single(421,
                        config_.serverDomain + " Timeout, closing connection"));
                    return IoStatus::Timeout;
                }
                if (collectStatus != IoStatus::Ok) {
                    return collectStatus;  // Closed/Error 等：不回码直接结束
                }
                // collectData 已复位事务；回到 Greeted 等待下一事务。
                state_ = State::Greeted;
                status = sendReply(dataReply);
                if (status != IoStatus::Ok) {
                    return status;
                }
                break;
            }
            case Verb::Rset: {
                resetTransaction();
                if (state_ != State::Start) {
                    state_ = State::Greeted;
                }
                status = sendReply(Reply::single(250, "OK"));
                if (status != IoStatus::Ok) {
                    return status;
                }
                break;
            }
            case Verb::Noop: {
                status = sendReply(Reply::single(250, "OK"));
                if (status != IoStatus::Ok) {
                    return status;
                }
                break;
            }
            case Verb::Vrfy: {
                status = sendReply(Reply::single(252,
                    "Cannot VRFY user, but will accept message and attempt delivery"));
                if (status != IoStatus::Ok) {
                    return status;
                }
                break;
            }
            case Verb::Quit: {
                return sendReply(Reply::single(221,
                    config_.serverDomain + " Service closing transmission channel"));
            }
            case Verb::Unknown: {
                status = sendReply(Reply::single(500, "Command unrecognized"));
                if (status != IoStatus::Ok) {
                    return status;
                }
                break;
            }
        }
    }
}

}  // namespace mail::smtp
