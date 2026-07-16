#pragma once

#include <string>
#include <string_view>

#include "mail/result.hpp"

// 单个 Maildir 邮箱的写侧存储（投递）。仅移动。
//
// Maildir 由 tmp/ new/ cur/ 三个子目录构成（依据 DJB maildir(5) 与 Dovecot
// Maildir 文档）。投递遵循经典的崩溃安全序列：先在 tmp/ 内以唯一名建文件并写全、
// fsync，再 rename 到 new/，最后 fsync new/ 目录项使目录项本身落盘。任一步失败都会
// unlink 自己的 tmp 文件后返回，保证不留半截垃圾。
//
// 读侧（cur/ 扫描、flag 解析）属后续里程碑，本类不涉及。
//
// 唯一名的实取输入（时钟/pid/主机名）全部收在 maildir.cpp；本头文件零系统调用。

namespace mail::store {

// 单个 Maildir 邮箱（tmp/new/cur 三目录）。仅移动。写侧（投递）实现；读侧属 M5。
class MaildirStore {
public:
    MaildirStore(MaildirStore&&) noexcept = default;
    MaildirStore& operator=(MaildirStore&&) noexcept = default;
    MaildirStore(const MaildirStore&) = delete;
    MaildirStore& operator=(const MaildirStore&) = delete;

    // 工厂：确保 root 及 root/{tmp,new,cur} 存在（0700，EEXIST 容忍），取并转义主机
    // 名（gethostname 失败退 "localhost"）。失败返回 IoStatus::Error + errno。
    static Result<MaildirStore> open(std::string root);

    // 投递一份拷贝：uniq 名 → open(tmp/, O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC, 0600)
    //（EEXIST 换名重试，上限 16 次）→ write 全量循环 → fsync(file) → close 检查
    // → rename(tmp→new) → fsync(new/ 目录 fd)。全部通过才 success，返回 new/ 内
    // 最终文件名（basename）。任何失败 unlink 自己的 tmp 文件后返回
    // IoStatus::Error + errno（保留首个失败点 errno）。data 按线上原样落盘（CRLF）。
    Result<std::string> deliverMessage(std::string_view data);

    const std::string& root() const noexcept { return root_; }

private:
    MaildirStore(std::string root, std::string escapedHost);

    std::string root_;
    std::string escapedHost_;  // open() 时转义一次，逐投递复用
};

}  // namespace mail::store
