#pragma once

#include <ctime>
#include <string>
#include <string_view>

// Maildir 投递文件的唯一名生成——纯函数层，零系统调用、零 I/O。
//
// 唯一名的全部外部输入（时钟、微秒、pid、投递序号、主机名）经 MaildirNameParts
// 显式注入，便于单元测试确定性地覆盖各分段与边界；实取时钟/pid/主机名属上层职责，
// 不在本层发生。
//
// 名字格式沿用现代 Maildir 惯例（依据 DJB maildir(5) 与 Dovecot Maildir 文档）：
//   "<epoch>.M<usec>P<pid>Q<seq>.<escaped-host>"
// 其中主机名按 Maildir 约定转义，保证整名不含 ':' '/'，且不以 '.' 开头。

namespace mail::store {

// Maildir unique-name 的全部输入，显式注入以便单测（不在函数内取时钟/pid/主机名）。
struct MaildirNameParts {
    std::time_t epochSeconds = 0;    // 秒级墙钟
    long        microseconds = 0;    // [0, 999999]
    long        pid = 0;
    unsigned long sequence = 0;      // per-process 投递计数（Q 段）
    std::string_view hostname;       // 原始主机名，函数内部做转义
};

// 生成 "<epoch>.M<usec>P<pid>Q<seq>.<escaped-host>"。
// 保证：不含 ':' '/'，不以 '.' 开头（hostname 为空时用 "localhost" 兜底）。
std::string makeMaildirUniqueName(const MaildirNameParts& parts);

// Maildir 主机名转义：'/' -> "\057"（字面反斜杠+八进制），':' -> "\072"。
std::string escapeMaildirHost(std::string_view host);

}  // namespace mail::store
