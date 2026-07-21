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

// 邮件路径（含尖括号的 forward/reverse-path）的最大总长度。
// RFC 5321 §4.5.3.1.3（"Path"）：256 字节。
inline constexpr std::size_t kMaxPathOctets = 256;

// 单封邮件必须支持的最大收件人数。
// RFC 5321 §4.5.3.1.10（"Recipients Buffer"）：100 个。
inline constexpr std::size_t kMaxRecipients = 100;

// 单封邮件消息内容的最大总长度。RFC 未强制统一上限，故此为项目自定的资源上限。
inline constexpr std::size_t kMaxMessageOctets = 10 * 1024 * 1024;

// DATA 阶段线路上单行的最大总长度。kMaxTextLineOctets（1000，已含 CRLF）之上再留
// 1 字节，用于点填充（dot-stuffing）的行首 '.' 余量。项目自定。
inline constexpr std::size_t kMaxDataWireLineOctets = kMaxTextLineOctets + 1;

// AUTH（RFC 4954）交换中**续行**的最大总长度，含尾部 CRLF。AUTH 命令行本身仍受
// kMaxCommandLineOctets（512）约束；RFC 4954 §4 明确把续行排除在命令行限制之外
//（"This requirement is independent of any line length limitations"），并指出 12288
// 字节足矣。本项目取更紧的 2048：RFC 4616 要求 authzid/authcid/passwd 各支持到 255
// 字节，最坏情形 767 字节原文 → base64 后 1024 → 含 CRLF 1026；2048 留余量。项目自定。
inline constexpr std::size_t kMaxAuthLineOctets = 2048;

// LineReader 为 SMTP 连接分帧时使用的上限，含尾部 CRLF。取本层各上下文上限的最大值
// —— LineReader 只负责"能把最长的合法行装下"，各上下文更紧的上限一律由 Session 自
// 查：命令行由 run() 的命令长度自查，DATA 行由 collectData，AUTH 续行由 runAuth。
inline constexpr std::size_t kMaxWireLineOctets =
    kMaxAuthLineOctets > kMaxDataWireLineOctets ? kMaxAuthLineOctets
                                                : kMaxDataWireLineOctets;

// 服务器等待客户端命令的最大空闲秒数（注意：此常量单位是秒，不是字节）。
// RFC 5321 §4.5.3.2.7（"Command Timeouts"）：5 分钟。
inline constexpr std::size_t kCommandTimeoutSeconds = 300;

}  // namespace mail
