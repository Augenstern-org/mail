#include "mail/smtp/command.hpp"

#include "mail/limits.hpp"

// SMTP 命令行解析的实现。所有解析都在传入的 string_view 上以偏移游标推进，不复制中
// 间子串（仅在需要生成 domain/path 字段时构造 std::string）。

namespace mail::smtp {

namespace {

// 将单个 ASCII 字符转为大写，避免依赖当前 locale。
char asciiUpper(char c) {
    if (c >= 'a' && c <= 'z') {
        return static_cast<char>(c - 'a' + 'A');
    }
    return c;
}

// ASCII 大小写不敏感比较：text 是否与 literal 逐字相等。
bool iequals(std::string_view text, std::string_view literal) {
    if (text.size() != literal.size()) {
        return false;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (asciiUpper(text[i]) != asciiUpper(literal[i])) {
            return false;
        }
    }
    return true;
}

bool isSpace(char c) { return c == ' ' || c == '\t'; }

// 从 pos 起跳过连续空白，返回首个非空白位置。
std::size_t skipSpaces(std::string_view s, std::size_t pos) {
    while (pos < s.size() && isSpace(s[pos])) {
        ++pos;
    }
    return pos;
}

// 从 pos 起读取一个由空白终止的 token，pos 会被推进到 token 之后。
std::string_view nextToken(std::string_view s, std::size_t& pos) {
    std::size_t start = pos;
    while (pos < s.size() && !isSpace(s[pos])) {
        ++pos;
    }
    return s.substr(start, pos - start);
}

// 在 s 中自 pos（应指向 '<'）起定位与之匹配的 '>'，扫描时正确跳过 Quoted-string
// 内部的 '>'（quoted 内以反斜杠转义下一个字符）。找到返回 '>' 的下标，否则返回
// npos。
std::size_t findPathEnd(std::string_view s, std::size_t pos) {
    bool inQuote = false;
    for (std::size_t i = pos + 1; i < s.size(); ++i) {
        char c = s[i];
        if (inQuote) {
            if (c == '\\' && i + 1 < s.size()) {
                ++i;  // 跳过被转义的字符
            } else if (c == '"') {
                inQuote = false;
            }
        } else if (c == '"') {
            inQuote = true;
        } else if (c == '>') {
            return i;
        }
    }
    return std::string_view::npos;
}

// 解析 MAIL/RCPT 尖括号内的路径内容（不含尖括号），剥离 source route 后写入
// out.path。inner 可能为空（对应 <>）。
void extractMailbox(std::string_view inner, Command& out) {
    // source route 形如 "@a,@b:user@d"：以 '@' 开头，':' 之后才是真正的 mailbox。
    if (!inner.empty() && inner.front() == '@') {
        std::size_t colon = inner.find(':');
        if (colon != std::string_view::npos) {
            inner = inner.substr(colon + 1);
        }
    }
    out.path.assign(inner);
}

// 解析单个 ESMTP 参数（形如 NAME=VALUE）。识别 SIZE= 与 BODY=，其余置 BadParam。
void applyParam(std::string_view param, Command& out) {
    std::size_t eq = param.find('=');
    if (eq == std::string_view::npos) {
        out.error = ParseError::BadParam;
        return;
    }
    std::string_view name = param.substr(0, eq);
    std::string_view value = param.substr(eq + 1);

    if (iequals(name, "SIZE")) {
        if (value.empty()) {
            out.error = ParseError::BadParam;
            return;
        }
        std::size_t size = 0;
        for (char c : value) {
            if (c < '0' || c > '9') {
                out.error = ParseError::BadParam;
                return;
            }
            size = size * 10 + static_cast<std::size_t>(c - '0');
        }
        out.declaredSize = size;
        return;
    }

    if (iequals(name, "BODY")) {
        if (iequals(value, "7BIT")) {
            out.body = BodyType::SevenBit;
        } else if (iequals(value, "8BITMIME")) {
            out.body = BodyType::EightBitMime;
        } else {
            out.error = ParseError::BadParam;
        }
        return;
    }

    out.error = ParseError::BadParam;
}

// 解析 '>' 之后剩余的 ESMTP 参数序列（以空白分隔）。任一参数出错即停在该错误上。
void parseParams(std::string_view s, std::size_t pos, Command& out) {
    while (true) {
        pos = skipSpaces(s, pos);
        if (pos >= s.size()) {
            return;
        }
        std::string_view param = nextToken(s, pos);
        applyParam(param, out);
        if (out.error != ParseError::None) {
            return;
        }
    }
}

// 解析 MAIL/RCPT 的公共主体：keyword 是期望的 "FROM:" 或 "TO:"。pos 指向动词之后。
void parseAddressCommand(std::string_view s, std::size_t pos,
                         std::string_view keyword, Command& out) {
    pos = skipSpaces(s, pos);
    // 关键字与冒号必须完整拼写（大小写不敏感）。
    if (s.size() - pos < keyword.size() ||
        !iequals(s.substr(pos, keyword.size()), keyword)) {
        out.error = ParseError::Syntax;
        return;
    }
    pos += keyword.size();
    pos = skipSpaces(s, pos);

    if (pos >= s.size() || s[pos] != '<') {
        out.error = ParseError::Syntax;
        return;
    }
    std::size_t end = findPathEnd(s, pos);
    if (end == std::string_view::npos) {
        out.error = ParseError::Syntax;
        return;
    }

    // 路径总长以尖括号（含）之间的字节数计，对应 RFC 5321 §4.5.3.1.3 的 256 上限。
    std::size_t pathOctets = end - pos + 1;
    if (pathOctets > kMaxPathOctets) {
        out.error = ParseError::PathTooLong;
        return;
    }

    std::string_view inner = s.substr(pos + 1, end - pos - 1);
    extractMailbox(inner, out);

    parseParams(s, end + 1, out);
}

// 解析 HELO/EHLO 参数进 domain。域名宽松：非空、无空白、无尖括号。
void parseGreeting(std::string_view s, std::size_t pos, Command& out) {
    pos = skipSpaces(s, pos);
    std::string_view arg = nextToken(s, pos);
    if (arg.empty()) {
        out.error = ParseError::Syntax;
        return;
    }
    for (char c : arg) {
        if (c == '<' || c == '>') {
            out.error = ParseError::Syntax;
            return;
        }
    }
    out.domain.assign(arg);
}

}  // namespace

Command parseCommand(std::string_view line) {
    Command out;

    std::size_t pos = skipSpaces(line, 0);
    std::string_view verbTok = nextToken(line, pos);

    if (iequals(verbTok, "HELO")) {
        out.verb = Verb::Helo;
        parseGreeting(line, pos, out);
    } else if (iequals(verbTok, "EHLO")) {
        out.verb = Verb::Ehlo;
        parseGreeting(line, pos, out);
    } else if (iequals(verbTok, "MAIL")) {
        out.verb = Verb::Mail;
        parseAddressCommand(line, pos, "FROM:", out);
    } else if (iequals(verbTok, "RCPT")) {
        out.verb = Verb::Rcpt;
        parseAddressCommand(line, pos, "TO:", out);
    } else if (iequals(verbTok, "DATA")) {
        out.verb = Verb::Data;
    } else if (iequals(verbTok, "RSET")) {
        out.verb = Verb::Rset;
    } else if (iequals(verbTok, "NOOP")) {
        out.verb = Verb::Noop;
    } else if (iequals(verbTok, "VRFY")) {
        out.verb = Verb::Vrfy;
    } else if (iequals(verbTok, "QUIT")) {
        out.verb = Verb::Quit;
    } else {
        out.verb = Verb::Unknown;
    }

    return out;
}

}  // namespace mail::smtp
