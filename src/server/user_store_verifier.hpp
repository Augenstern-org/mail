#pragma once
#include <memory>
#include <string>
#include <string_view>

#include "mail/auth/user_store.hpp"
#include "mail/smtp/recipient_verifier.hpp"

#include "maildir_sink.hpp"  // mail::app::sanitizedLocalPart

// RecipientVerifier → auth::UserStore 的应用层适配器（header-only）。
//
// 依赖方向与 maildir_sink.hpp 相同：适配器住在应用层（src/server），反向依赖 auth
// 与 smtp 两层；auth 层绝不 include recipient_verifier.hpp。
//
// 与投递侧的一致性（关键不变量）：verify 先用 mail::app::sanitizedLocalPart 归一并
// 校验 local part —— 与 MaildirSink 投递时调用的是同一个函数。凡本类回 Accept 的收件
// 人，MaildirSink 必定能取到合法 local part，故不会出现"RCPT 250、投递 451"的错配。
//
// 本类是 M4 让 RcptDecision::RejectPermanent（550）首次可达的地方：在此之前
// AcceptAllVerifier 使该分支恒不触发。

namespace mail::app {

class UserStoreVerifier final : public mail::smtp::RecipientVerifier {
public:
    explicit UserStoreVerifier(std::shared_ptr<const mail::auth::UserStore> store)
        : store_(std::move(store)) {}

    // local part 不合格（空、含白名单外字符、以 '.' 开头、含 ".."）→ 550；
    // 合格但用户档中无此用户 → 550；存在 → 250。
    // 绝不返回 RejectTemporary：用户档在启动时一次性载入内存，查询不会瞬时失败。
    mail::smtp::RcptDecision verify(std::string_view mailbox) override {
        std::string local = sanitizedLocalPart(mailbox);
        if (local.empty()) {
            return mail::smtp::RcptDecision::RejectPermanent;
        }
        if (!store_->hasUser(local)) {
            return mail::smtp::RcptDecision::RejectPermanent;
        }
        return mail::smtp::RcptDecision::Accept;
    }

private:
    std::shared_ptr<const mail::auth::UserStore> store_;
};

}  // namespace mail::app
