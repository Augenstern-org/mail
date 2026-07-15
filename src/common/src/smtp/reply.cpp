#include "mail/smtp/reply.hpp"

#include <cstddef>
#include <string>

// SMTP 响应序列化的实现。

namespace mail::smtp {

Reply Reply::single(int code, std::string text) {
    Reply reply;
    reply.code = code;
    reply.lines.push_back(std::move(text));
    return reply;
}

std::string serialize(const Reply& reply) {
    std::string codeStr = std::to_string(reply.code);
    std::string out;
    for (std::size_t i = 0; i < reply.lines.size(); ++i) {
        // 非末行用连字符分隔，末行用空格分隔，以标记多行响应的结束。
        char sep = (i + 1 < reply.lines.size()) ? '-' : ' ';
        out += codeStr;
        out += sep;
        out += reply.lines[i];
        out += "\r\n";
    }
    return out;
}

}  // namespace mail::smtp
