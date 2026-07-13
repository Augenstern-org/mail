#pragma once

#include <cstddef>

// 协议与安全上限，以字节（octet）计。来自 RFC 的数值标注确切章节；项目自定的默认
// 值会明确说明。

namespace mail {

// SMTP 命令行的最大总长度，含尾部 CRLF。
// RFC 5321 §4.5.3.1.4（"Command Line"）：512 字节。
inline constexpr std::size_t kMaxCommandLineOctets = 512;

// SMTP 文本（消息内容）行的最大总长度，含尾部 CRLF。
// RFC 5321 §4.5.3.1.6（"Text Line"）：1000 字节。
inline constexpr std::size_t kMaxTextLineOctets = 1000;

// 服务器必须无需中间续行即接受的 IMAP 非同步 literal 的最大尺寸。
// RFC 7888（"IMAP4 Non-synchronizing Literals"）/ RFC 9051 §4.3：4096 字节。
inline constexpr std::size_t kMaxNonSyncLiteralOctets = 4096;

// LineReader 中单行累积长度的默认上限。IMAP 没有 RFC 级别的行长限制，故此值是项目
// 层面的内存安全默认值，而非协议常量；已知更紧协议限值的调用方（如 SMTP 的
// kMaxCommandLineOctets）应显式传入。
inline constexpr std::size_t kDefaultMaxLineOctets = 8192;

}  // namespace mail
