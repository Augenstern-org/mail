#pragma once

#include <string_view>

#include "mail/smtp/envelope.hpp"

// 消息投递的抽象出口。Session 在 DATA 成功收尾时调用 deliver 把信封与完整消息原文交
// 给下游，由具体实现决定落地方式（本地邮箱、队列、转发……）。

namespace mail::smtp {

// deliver 的结果，直接映射到 SMTP 对“.”终止行的回码。
enum class SinkStatus {
    Ok,            // 250：已接受并负责投递
    TransientFail, // 451：临时失败，客户端可稍后重试
    PermanentFail  // 554：永久失败，事务被拒
};

// 消息投递接口。适配器放在应用层实现；storage 层绝不 include 本头，以免协议语义泄漏
// 进存储层。实现须线程安全，或由调用方按连接单独实例化。
class MessageSink {
public:
    virtual ~MessageSink() = default;

    // 投递一封邮件。data 是已去除 dot-stuffing 的完整消息原文，各行以 CRLF 分隔，
    // 不含终止行“.”；空消息时 data 为空串。
    virtual SinkStatus deliver(const Envelope& envelope, std::string_view data) = 0;
};

}  // namespace mail::smtp
