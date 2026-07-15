#pragma once

#include <string>
#include <vector>

// SMTP 响应的纯函数构造与序列化层。Reply 以结构化形式持有回码与文本行，serialize
// 负责按 RFC 5321 §4.2.1 的多行响应格式落到线路字节。

namespace mail::smtp {

// 一个 SMTP 响应：单一回码配一或多行文本。
struct Reply {
    int code = 0;
    std::vector<std::string> lines;

    // 构造单行响应的便捷工厂。
    static Reply single(int code, std::string text);
};

// 将 Reply 序列化为线路字节（RFC 5321 §4.2.1）：前 n-1 行以 "code-text\r\n"
// 形式（连字符分隔符表示后续仍有行），末行以 "code text\r\n"（空格分隔符表示结束）。
std::string serialize(const Reply& reply);

}  // namespace mail::smtp
