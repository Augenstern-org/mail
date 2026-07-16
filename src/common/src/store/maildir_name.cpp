#include "mail/store/maildir_name.hpp"

#include <string>

// Maildir 唯一名生成的实现。全程纯字符串拼接，不触碰时钟、pid、主机名或任何 I/O，
// 全部输入均由调用方经 MaildirNameParts 注入（参见头文件）。

namespace mail::store {

std::string escapeMaildirHost(std::string_view host) {
    // Maildir 约定：主机名中的 '/' 与 ':' 会破坏路径与冒号分隔的 info 段，故各自替换为
    // 字面的反斜杠加三位八进制码——'/' -> "\057"，':' -> "\072"；其余字符原样保留。
    std::string escaped;
    escaped.reserve(host.size());
    for (char c : host) {
        if (c == '/') {
            escaped += "\\057";
        } else if (c == ':') {
            escaped += "\\072";
        } else {
            escaped += c;
        }
    }
    return escaped;
}

std::string makeMaildirUniqueName(const MaildirNameParts& parts) {
    // hostname 为空时兜底为 "localhost"，避免生成以 '.' 开头（点段之间空）的名字。
    std::string_view host = parts.hostname.empty() ? std::string_view("localhost")
                                                   : parts.hostname;

    // 数字段一律十进制原样输出、不补零（M0 与 M999999 均合法）。
    std::string name;
    name += std::to_string(parts.epochSeconds);
    name += ".M";
    name += std::to_string(parts.microseconds);
    name += 'P';
    name += std::to_string(parts.pid);
    name += 'Q';
    name += std::to_string(parts.sequence);
    name += '.';
    name += escapeMaildirHost(host);
    return name;
}

}  // namespace mail::store
