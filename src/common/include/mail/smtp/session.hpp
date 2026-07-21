#pragma once

#include <cstddef>
#include <string>
#include <string_view>

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

// 前置声明：AUTH 相关类型只在实现里按引用/指针使用，故协议头不必拉入 auth 头。
namespace mail::auth {
class Sasl;
class UserStore;
}  // namespace mail::auth

namespace mail::smtp {

// 会话级配置。默认值取自 limits.hpp 的项目上限。
struct SessionConfig {
    std::string serverDomain = "localhost";       // 出现在问候与 EHLO 首行的服务器域名
    std::size_t maxMessageOctets = kMaxMessageOctets;  // 单封消息原文上限
    std::size_t maxRecipients = kMaxRecipients;        // 单封邮件收件人数上限

    // RFC 4954：明文口令机制默认禁用。为 false（或 userStore 为空）时，AUTH 既不在
    // EHLO 中宣告，也不被接受（回 504）。RFC 4954 要求服务器"必须实现一种不允许任何
    // 明文口令机制"的配置；本项目 TLS 尚未落地（M6），故默认关是这条 MUST 的安全读法。
    bool allowPlaintextAuth = false;
    // 是否要求先认证才能 MAIL FROM。为 true 且未认证时 onMail 回 530 5.7.0。
    bool requireAuthForMail = false;
    // 非拥有指针。为 nullptr 时等同 allowPlaintextAuth=false。调用方须保证其存活至
    // Session 析构（服务端经 shared_ptr<const UserStore> 按值传入工作线程保证之）。
    const auth::UserStore* userStore = nullptr;
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

    // 已发送首个 334 之前的全部准入检查在 run() 的 case Verb::Auth 内联完成（对照
    // case Verb::Data 的 503 前置检查）。runAuth 驱动 334/响应 的多轮交换直到裁决。
    // 产出的应答写入 outReply（仅当返回 Ok 时有效）。非 Ok 表示 I/O 中断，由 run()
    // 决定回码与去留 —— 与 collectData 完全同构。
    // initialResponse 是命令行上的 base64 原文；hasInitialResponse 区分"缺省"与 "="。
    IoStatus runAuth(auth::Sasl& mech, bool hasInitialResponse,
                     std::string_view initialResponse, Reply& outReply);

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

    // 已认证身份。与 State 正交，故独立成员而非第 5 个枚举值：RSET 与重复 EHLO 都只
    // 复位事务与 state_，绝不触碰这两个字段。RFC 4954 对二者是否清除认证保持沉默；
    // Postfix / Dovecot / Exim 的一致做法是认证跨越 RSET 与第二次 EHLO 存活，RSET
    // 只清事务。本实现照此。
    bool authenticated_ = false;
    std::string authenticatedUser_;
};

}  // namespace mail::smtp
