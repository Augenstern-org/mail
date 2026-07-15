#pragma once

#include <cstddef>
#include <string>
#include <vector>

// 一次邮件事务的信封（envelope）。在 MAIL/RCPT 阶段逐步累积，DATA 成功完成时随消息
// 原文一并交给 MessageSink。与消息正文分离：信封只描述“从谁、投给谁、多大”，不含头
// 部与正文。sender 为空串表示 <>（空反向路径），即退信（bounce）路径。

namespace mail::smtp {

struct Envelope {
    std::string sender;                     // 反向路径 mailbox；空串 = <> 退信路径
    std::vector<std::string> recipients;    // 已被接受的正向路径 mailbox 列表
    std::size_t declaredSize = 0;           // MAIL SIZE= 宣告值，未指定为 0
    bool eightBitMime = false;              // MAIL BODY=8BITMIME 时为真
};

}  // namespace mail::smtp
