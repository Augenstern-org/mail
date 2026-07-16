#pragma once

#include <cstdio>
#include <string>
#include <string_view>

#include "mail/smtp/message_sink.hpp"
#include "mail/store/maildir.hpp"

// MessageSink → MaildirStore 的应用层适配器（header-only）。
//
// 依赖方向：适配器位于应用层（src/server），反向依赖 storage 层的 MaildirStore；
// storage 层绝不 include MessageSink，以免协议语义泄漏进存储层（见
// message_sink.hpp 头注）。故本文件落在 src/server、namespace mail::app，而非
// src/common。

namespace mail::app {

// 前置声明：deliver 的 inline 定义在类体内引用本函数，其定义在类之后。
std::string sanitizedLocalPart(std::string_view mailbox);

// MessageSink → MaildirStore 适配器（应用层，见 message_sink.hpp 头注约束）。
// 每收件人一份独立拷贝，投入 <root>/<sanitized-local-part>/ 的 new/；邮箱按需
// MaildirStore::open。local-part 白名单 [A-Za-z0-9._+-]、非空、不以 '.' 开头、无
// ".."，不合格或任一份落盘失败 → TransientFail（451）。已成功副本不回收（注释说明
// 重试重复语义）。per-connection 实例化，不共享。
class MaildirSink final : public mail::smtp::MessageSink {
public:
    explicit MaildirSink(std::string rootDir) : rootDir_(std::move(rootDir)) {}

    // 逐收件人投递一份独立拷贝。
    //
    // 流程：对每个收件人先 sanitizedLocalPart 提取并校验 local part；空串（不合格）
    // 即整体失败。否则 MaildirStore::open(rootDir_ + "/" + local)（按需建邮箱）再
    // deliverMessage(data)。任一步失败即整体 TransientFail（451）。
    //
    // 绝不返回 PermanentFail：存储侧的一切失败（含非法收件人 local part）都映射为
    // 451——收件人的最终合法性本应在 RCPT 阶段由 verifier 把关，到此已是暂时性的落地
    // 问题，客户端可稍后重试。
    //
    // 重试重复语义：部分失败时，已成功落入 new/ 的拷贝不回收；客户端收到 451 后重试
    // 会对先前成功的收件人再投一份，产生重复投递。这是刻意取舍——回收已落盘的副本本身
    // 也可能失败，且跨收件人的原子性并非本层职责。
    mail::smtp::SinkStatus deliver(const mail::smtp::Envelope& envelope,
                                   std::string_view data) override {
        for (const std::string& recipient : envelope.recipients) {
            std::string local = sanitizedLocalPart(recipient);
            if (local.empty()) {
                std::fprintf(stderr,
                             "maildir sink: reject recipient with invalid "
                             "local-part: %s\n",
                             recipient.c_str());
                return mail::smtp::SinkStatus::TransientFail;
            }

            auto store = mail::store::MaildirStore::open(rootDir_ + "/" + local);
            if (!store.has_value()) {
                std::fprintf(stderr,
                             "maildir sink: open mailbox failed for %s "
                             "(errno=%d)\n",
                             local.c_str(), store.error_errno());
                return mail::smtp::SinkStatus::TransientFail;
            }

            auto delivered = store.value().deliverMessage(data);
            if (!delivered.has_value()) {
                std::fprintf(stderr,
                             "maildir sink: deliver failed for %s (errno=%d)\n",
                             local.c_str(), delivered.error_errno());
                return mail::smtp::SinkStatus::TransientFail;
            }
        }
        return mail::smtp::SinkStatus::Ok;
    }

private:
    std::string rootDir_;
};

// 从 mailbox 提取并校验 local part（'@' 前部分，无 '@' 则整串）。校验：非空、全部
// 字符 ∈ [A-Za-z0-9._+-]、不以 '.' 开头、不含 ".." 子串。任一不满足返回空串。
// 纯函数，便于单测。
inline std::string sanitizedLocalPart(std::string_view mailbox) {
    std::string_view local = mailbox;
    if (auto at = mailbox.find('@'); at != std::string_view::npos) {
        local = mailbox.substr(0, at);
    }

    if (local.empty()) {
        return std::string();
    }
    if (local.front() == '.') {
        return std::string();
    }

    char prev = '\0';
    for (char c : local) {
        bool allowed = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                       (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                       c == '+' || c == '-';
        if (!allowed) {
            return std::string();
        }
        if (prev == '.' && c == '.') {
            return std::string();
        }
        prev = c;
    }

    return std::string(local);
}

}  // namespace mail::app
