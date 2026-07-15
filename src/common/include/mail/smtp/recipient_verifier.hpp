#pragma once

#include <string_view>

// 收件人校验的抽象。Session 在 RCPT 阶段对每个正向路径 mailbox 询问一次，据此决定收
// 录还是拒绝。把策略（本地存在性、白名单、限流……）与协议时序解耦。

namespace mail::smtp {

// verify 的裁决，直接映射到 RCPT 的回码。
enum class RcptDecision {
    Accept,          // 250：接受该收件人
    RejectPermanent, // 550：永久拒绝（如收件人不存在）
    RejectTemporary  // 450：临时拒绝（如暂不可用），客户端可稍后重试
};

// 收件人校验接口。
class RecipientVerifier {
public:
    virtual ~RecipientVerifier() = default;

    // 校验单个收件人 mailbox（已剥除尖括号与 source route）。
    virtual RcptDecision verify(std::string_view mailbox) = 0;
};

// 接受一切收件人的平凡实现，供开发与测试使用。
class AcceptAllVerifier final : public RecipientVerifier {
public:
    RcptDecision verify(std::string_view /*mailbox*/) override {
        return RcptDecision::Accept;
    }
};

}  // namespace mail::smtp
