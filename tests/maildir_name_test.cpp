// mail::store::makeMaildirUniqueName / escapeMaildirHost 的纯函数单元测试。无 I/O、无
// 系统调用：全部输入经 MaildirNameParts 注入，逐项断言格式、边界、转义与安全不变式。
//
// 不使用测试框架：一个极小的 CHECK 宏记录失败，任一检查失败时进程返回非零（沿用
// smtp_session_test.cpp 的模式）。

#include <cstdio>
#include <string>

#include "mail/store/maildir_name.hpp"

using mail::store::escapeMaildirHost;
using mail::store::makeMaildirUniqueName;
using mail::store::MaildirNameParts;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            ++g_failures;                                               \
            std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #cond,   \
                         __FILE__, __LINE__);                           \
        }                                                               \
    } while (0)

// 1. 基本格式：各分段拼接精确等于预期字符串。
void testBasicFormat() {
    MaildirNameParts parts;
    parts.epochSeconds = 1700000000;
    parts.microseconds = 123456;
    parts.pid = 4242;
    parts.sequence = 7;
    parts.hostname = "mx1.example.com";
    CHECK(makeMaildirUniqueName(parts) ==
          "1700000000.M123456P4242Q7.mx1.example.com");
}

// 2. usec 边界：0 与 999999 均原样、不补零。
void testMicrosecondBounds() {
    MaildirNameParts parts;
    parts.epochSeconds = 1700000000;
    parts.pid = 4242;
    parts.sequence = 7;
    parts.hostname = "mx1.example.com";

    parts.microseconds = 0;
    CHECK(makeMaildirUniqueName(parts).find(".M0P") != std::string::npos);

    parts.microseconds = 999999;
    CHECK(makeMaildirUniqueName(parts).find(".M999999P") != std::string::npos);
}

// 3. escapeMaildirHost 逐项转义。
void testEscapeHost() {
    CHECK(escapeMaildirHost("a/b") == "a\\057b");
    CHECK(escapeMaildirHost("a/b").size() == 6);
    CHECK(escapeMaildirHost("a:b") == "a\\072b");
    CHECK(escapeMaildirHost("a/b:c") == "a\\057b\\072c");
    CHECK(escapeMaildirHost("plain.host") == "plain.host");
    CHECK(escapeMaildirHost("") == "");
}

// 4. 空 host 兜底为 "localhost"。
void testEmptyHostFallback() {
    MaildirNameParts parts;
    parts.epochSeconds = 1700000000;
    parts.microseconds = 1;
    parts.pid = 4242;
    parts.sequence = 7;
    parts.hostname = "";

    std::string name = makeMaildirUniqueName(parts);
    const std::string suffix = ".localhost";
    CHECK(name.size() >= suffix.size());
    CHECK(name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0);
}

// 5. 安全性：恶意 host 经完整生成后不含 '/' ':'，且不以 '.' 开头。
void testSecurityInvariants() {
    MaildirNameParts parts;
    parts.epochSeconds = 1700000000;
    parts.microseconds = 1;
    parts.pid = 4242;
    parts.sequence = 7;
    parts.hostname = "evil/host:x";

    std::string name = makeMaildirUniqueName(parts);
    CHECK(name.find('/') == std::string::npos);
    CHECK(name.find(':') == std::string::npos);
    CHECK(name[0] != '.');
}

// 6. 唯一性：仅 sequence 或仅 microseconds 不同即产生互异的名字。
void testUniqueness() {
    MaildirNameParts base;
    base.epochSeconds = 1700000000;
    base.microseconds = 123456;
    base.pid = 4242;
    base.sequence = 7;
    base.hostname = "mx1.example.com";

    MaildirNameParts seqDiff = base;
    seqDiff.sequence = 8;
    CHECK(makeMaildirUniqueName(base) != makeMaildirUniqueName(seqDiff));

    MaildirNameParts usecDiff = base;
    usecDiff.microseconds = 123457;
    CHECK(makeMaildirUniqueName(base) != makeMaildirUniqueName(usecDiff));
}

}  // namespace

int main() {
    testBasicFormat();
    testMicrosecondBounds();
    testEscapeHost();
    testEmptyHostFallback();
    testSecurityInvariants();
    testUniqueness();
    return g_failures == 0 ? 0 : 1;
}
