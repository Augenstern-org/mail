#pragma once

#include <cstddef>
#include <string>

#include "mail/io_status.hpp"
#include "mail/limits.hpp"
#include "mail/net/byte_sink.hpp"
#include "mail/net/line_reader.hpp"
#include "mail/smtp/command.hpp"
#include "mail/smtp/envelope.hpp"
#include "mail/smtp/message_sink.hpp"
#include "mail/smtp/recipient_verifier.hpp"
#include "mail/smtp/reply.hpp"

// 单个 SMTP 连接的会话状态机。把命令解析（command.hpp）、响应序列化（reply.hpp）、
// 分帧读取（LineReader）与投递/校验抽象（MessageSink/RecipientVerifier）编织成一次
// 完整的 SMTP 事务序列。Session 只持引用，不拥有任何 I/O 或下游对象；一个 Session
// 对应一个连接的完整生命周期。

namespace mail::smtp {

// 会话级配置。默认值取自 limits.hpp 的项目上限。
struct SessionConfig {
    std::string serverDomain = "localhost";       // 出现在问候与 EHLO 首行的服务器域名
    std::size_t maxMessageOctets = kMaxMessageOctets;  // 单封消息原文上限
    std::size_t maxRecipients = kMaxRecipients;        // 单封邮件收件人数上限
};

// SMTP 会话状态机。禁拷贝：持有引用成员，语义上绑定单个连接。
class Session {
public:
    Session(net::LineReader& reader, net::ByteSink& writer, MessageSink& sink,
            RecipientVerifier& verifier, SessionConfig config);

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // 驱动整个会话直到结束（QUIT / 对端关闭 / 致命 I/O 错误 / 超时）。返回会话结束
    // 时最后一次有意义 I/O 的状态，供上层记录断开原因。
    IoStatus run();

private:
    // 事务阶段。DATA 不占用独立状态位，其内部收集在 collectData 的子循环中完成。
    enum class State { Start, Greeted, MailGiven, RcptGiven };

    // 各命令的处理：返回应答，并按需推进 state_ / envelope_。
    Reply onHeloEhlo(const Command& command);
    Reply onMail(const Command& command);
    Reply onRcpt(const Command& command);

    // 已发送 354 之后收集 DATA 内容。产出的应答写入 outReply（仅当返回 Ok 时有效，
    // 且此时事务已复位）。非 Ok 表示 I/O 中断，由 run() 决定回码与去留。
    IoStatus collectData(Reply& outReply);

    // 复位当前事务累积的信封（不改变 state_）。
    void resetTransaction();

    // 序列化并写出一条应答；返回底层写状态（非 Ok 表示应终止会话）。
    IoStatus sendReply(const Reply& reply);

    net::LineReader& reader_;
    net::ByteSink& writer_;
    MessageSink& sink_;
    RecipientVerifier& verifier_;
    SessionConfig config_;
    State state_ = State::Start;
    Envelope envelope_;
};

}  // namespace mail::smtp
