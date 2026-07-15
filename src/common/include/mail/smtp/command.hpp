#pragma once

#include <cstddef>
#include <string>
#include <string_view>

// SMTP 命令行的纯函数解析层。不接触套接字、不维护会话状态、不做协议时序校验，只把
// 单条已剥除 CRLF 的命令行文本翻译为一个 tagged 的 Command 结构。文法子集覆盖
// RFC 5321 的 HELO/EHLO/MAIL/RCPT/DATA/RSET/NOOP/VRFY/QUIT。

namespace mail::smtp {

// 命令动词。Unknown 表示动词无法识别（仍视为语法可继续，由上层决定回码）。
enum class Verb { Helo, Ehlo, Mail, Rcpt, Data, Rset, Noop, Vrfy, Quit, Unknown };

// 解析错误分类，直接映射到 SMTP 回码。None 表示解析成功。
enum class ParseError {
    None,
    Syntax,       // 一般语法错误 → 501
    PathTooLong,  // 尖括号内路径超过 kMaxPathOctets → 501
    BadParam      // 不识别的 ESMTP 参数 → 555
};

// MAIL 命令的 BODY= 参数取值。
enum class BodyType { Unspecified, SevenBit, EightBitMime };

// 解析结果。error != None 时，仅 verb 与 error 字段有效，其余字段内容未定义。
struct Command {
    Verb verb = Verb::Unknown;
    ParseError error = ParseError::None;
    std::string domain;            // HELO/EHLO 参数域名
    std::string path;             // 邮箱路径；空串表示 <>（空反向路径）
    std::size_t declaredSize = 0;  // SIZE= 参数值，未指定为 0
    BodyType body = BodyType::Unspecified;
};

// 解析一条已剥除尾部 CRLF 的 SMTP 命令行。纯函数：动词大小写不敏感，不校验会话状
// 态，不校验命令行总长（长度限制由 LineReader 在读取层完成）。
Command parseCommand(std::string_view line);

}  // namespace mail::smtp
