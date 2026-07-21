#include "mail/smtp/session.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "mail/auth/password.hpp"
#include "mail/auth/sasl.hpp"
#include "mail/auth/user_store.hpp"
#include "mail/base64.hpp"

// SMTP 会话状态机的实现。分派逻辑严格对照 RFC 5321 的命令时序，并把 SMTP smuggling
// 相关的分帧安全（裸 CR/LF 终止、超长行 500、DATA 阶段终止行不可被超长行误吞）交由
// 下层 LineReader 的语义保证与本层的显式长度自查共同兜住。

namespace mail::smtp {

namespace {

// 命令行剥除 CRLF 后允许的最大字节数。kMaxCommandLineOctets 含尾部 CRLF，故此处减 2。
constexpr std::size_t kMaxStrippedCommandOctets = kMaxCommandLineOctets - 2;

// AUTH 续行剥除 CRLF 后允许的最大字节数。kMaxAuthLineOctets 含尾部 CRLF，故减 2。
constexpr std::size_t kMaxStrippedAuthOctets = kMaxAuthLineOctets - 2;

// 作用域退出时抹除一个字符串缓冲。AUTH 交换里 decoded 会承载明文口令，而 runAuth
// 有近十条提前返回路径（501/500/超时/对端关闭），逐条手写 scrub 必然漏。用析构函数
// 兜住全部退出路径，包括将来新增的分支。
class ScrubGuard {
public:
    explicit ScrubGuard(std::string& target) : target_(target) {}

    ScrubGuard(const ScrubGuard&) = delete;
    ScrubGuard& operator=(const ScrubGuard&) = delete;

    ~ScrubGuard() { auth::scrub(target_); }

private:
    std::string& target_;
};

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
        // 仅在启用时宣告。只发 RFC 4954 的标准形式；历史上的 "AUTH=PLAIN LOGIN" 变通不在
        // RFC 4954 中，2026 年已无必要。认证成功后继续宣告（RFC 4954 不要求撤回，重复
        // AUTH 由 503 兜住）。
        if (config_.allowPlaintextAuth && config_.userStore != nullptr) {
            reply.lines.push_back("AUTH " + std::string(auth::supportedMechanisms()));
        }
        return reply;
    }
    return Reply::single(250, config_.serverDomain);
}

Reply Session::onMail(const Command& command) {
    if (config_.requireAuthForMail && !authenticated_) {
        return Reply::single(530, "5.7.0 Authentication required");
    }
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

        // DATA 行长自查：LineReader 现以 kMaxWireLineOctets（2048，为 AUTH 续行放宽）
        // 分帧，1000..2046 字节的数据行会以 Ok 到达，必须在此毒化，否则 RFC 5321
        // §4.5.3.1.6 的文本行上限形同虚设。与 run() 中的命令行自查同构。line 已剥除
        // CRLF，故补回 2 字节再与含 CRLF 的上限比较。终止行 "." 只有 1 字节，永不触发
        // 此判断，故置于其前是安全的。
        if (line.size() + 2 > kMaxDataWireLineOctets) {
            if (!poisoned) {
                poisoned = true;
                poisonReply = Reply::single(552, "Message exceeds maximum size");
            }
            continue;
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

IoStatus Session::runAuth(auth::Sasl& mech, bool hasInitialResponse,
                          std::string_view initialResponse, Reply& outReply) {
    std::string decoded;          // 明文载荷，含口令；由 guard 在每条退出路径抹除
    ScrubGuard decodedGuard(decoded);
    std::string challenge;
    auth::SaslStep st;

    // 初始响应的有无是两个不同的机制状态，不可混同："AUTH PLAIN" 是"给我挑战"，
    // "AUTH PLAIN =" 是"这是我的空初始响应"（RFC 4954 用 '=' 表示零长初始响应）。
    if (hasInitialResponse) {
        if (initialResponse != "=" && !base64Decode(initialResponse, decoded)) {
            // RFC 4954 MUST：base64 解码失败是 501，不是 535 —— 后者会把"语法错"
            // 谎报成"凭据错"，客户端据此重试没有意义。
            outReply = Reply::single(501, "5.5.2 Invalid base64 data");
            return IoStatus::Ok;
        }
        st = mech.step(decoded, challenge);
    } else {
        st = mech.begin(challenge);
    }

    std::string line;
    while (st == auth::SaslStep::Challenge) {
        // 空挑战时 base64Encode 返回空串 → serialize 产出 "334 \r\n"（带尾随空格）。
        // 裸 "334" 不合规，客户端会解析失败。
        IoStatus status = sendReply(Reply::single(334, base64Encode(challenge)));
        if (status != IoStatus::Ok) {
            return status;  // 写失败：由 run() 直接结束，不再回码
        }

        status = reader_.readLine(line);
        if (status == IoStatus::Timeout) {
            return IoStatus::Timeout;  // 由 run() 回 421
        }
        if (status == IoStatus::LineTooLong) {
            // LineReader 已自行排空该行，会话可继续。
            outReply = Reply::single(500,
                "5.5.6 Authentication Exchange line is too long");
            return IoStatus::Ok;
        }
        if (status != IoStatus::Ok) {
            return status;  // Closed/Error/WouldBlock 等：由 run() 决定去留
        }

        // 续行长度自查。因当前 kMaxWireLineOctets == kMaxAuthLineOctets，LineReader
        // 的分帧上限与本上限重合，超限行必然已在上面以 LineTooLong 返回，故本分支
        // 暂不可达。**保留**是为了将来分帧上限再被抬高时（如 IMAP 的 literal 需要
        // 更大窗口）不至于静默失守 —— 与 collectData 的数据行自查同构。
        if (line.size() > kMaxStrippedAuthOctets) {
            outReply = Reply::single(500,
                "5.5.6 Authentication Exchange line is too long");
            return IoStatus::Ok;
        }

        // RFC 4954 MUST：客户端可用单独一行 "*" 取消交换。状态回到认证前，会话继续，
        // 不关连接。
        if (line == "*") {
            outReply = Reply::single(501, "5.7.0 Authentication aborted");
            return IoStatus::Ok;
        }

        if (line == "=") {
            decoded.clear();
        } else if (!base64Decode(line, decoded)) {
            outReply = Reply::single(501, "5.5.2 Invalid base64 data");
            return IoStatus::Ok;
        }

        st = mech.step(decoded, challenge);
    }

    switch (mech.result()) {
        case auth::AuthResult::Ok:
            authenticated_ = true;
            authenticatedUser_.assign(mech.authenticatedUser());
            outReply = Reply::single(235, "2.7.0 Authentication successful");
            break;
        case auth::AuthResult::Rejected:
            // 用户不存在与口令错在此处不可区分，故文案统一，不泄漏用户档状态。
            outReply = Reply::single(535, "5.7.8 Authentication credentials invalid");
            break;
        case auth::AuthResult::Unavailable:
            outReply = Reply::single(454, "4.7.0 Temporary authentication failure");
            break;
    }
    return IoStatus::Ok;
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

        // 命令行长度自查：LineReader 以 kMaxWireLineOctets 分帧，511..2046 字节的
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
            case Verb::Auth: {
                // 首个 334 之前的全部准入检查，顺序照 case Verb::Data 的 503 前置检查。
                if (!config_.allowPlaintextAuth || config_.userStore == nullptr) {
                    status = sendReply(Reply::single(504,
                        "5.5.4 Authentication mechanism not supported"));
                    if (status != IoStatus::Ok) {
                        return status;
                    }
                    break;
                }
                if (authenticated_) {
                    status = sendReply(Reply::single(503, "5.5.1 Already authenticated"));
                    if (status != IoStatus::Ok) {
                        return status;
                    }
                    break;
                }
                // 同时覆盖"事务进行中 AUTH"（MailGiven/RcptGiven，RFC 4954 MUST 拒绝）
                // 与"EHLO 之前 AUTH"（Start）。RFC 4954 弃用了 538，故一律 503。
                if (state_ != State::Greeted) {
                    status = sendReply(Reply::single(503, "Bad sequence of commands"));
                    if (status != IoStatus::Ok) {
                        return status;
                    }
                    break;
                }
                std::unique_ptr<auth::Sasl> mech =
                    auth::makeSasl(command.mechanism, *config_.userStore);
                if (!mech) {
                    status = sendReply(Reply::single(504,
                        "5.5.4 Unrecognized authentication type"));
                    if (status != IoStatus::Ok) {
                        return status;
                    }
                    break;
                }

                Reply authReply;
                IoStatus authStatus = runAuth(*mech, command.hasInitialResponse,
                                              command.initialResponse, authReply);
                if (authStatus == IoStatus::Timeout) {
                    sendReply(Reply::single(421,
                        config_.serverDomain + " Timeout, closing connection"));
                    return IoStatus::Timeout;
                }
                if (authStatus != IoStatus::Ok) {
                    return authStatus;  // Closed/Error 等：不回码直接结束
                }
                status = sendReply(authReply);
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
