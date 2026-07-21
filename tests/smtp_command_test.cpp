// mail::smtp 纯函数解析层的单元测试：parseCommand 的文法子集与 Reply 序列化。
//
// 不使用测试框架：一个极小的 CHECK 宏记录失败，任一检查失败时进程返回非零（沿用
// line_reader_test.cpp 的模式）。

#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

#include "mail/smtp/command.hpp"
#include "mail/smtp/reply.hpp"

using mail::smtp::BodyType;
using mail::smtp::Command;
using mail::smtp::parseCommand;
using mail::smtp::ParseError;
using mail::smtp::Reply;
using mail::smtp::serialize;
using mail::smtp::Verb;

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

// MAIL FROM:<> 必须被接受，path 为空串表示空反向路径。
void testEmptyReversePath() {
    Command cmd = parseCommand("MAIL FROM:<>");
    CHECK(cmd.verb == Verb::Mail);
    CHECK(cmd.error == ParseError::None);
    CHECK(cmd.path.empty());
}

// source route 必须被解析并剥离，只留最终 mailbox。
void testSourceRouteStripped() {
    Command cmd = parseCommand("RCPT TO:<@a,@b:user@d>");
    CHECK(cmd.verb == Verb::Rcpt);
    CHECK(cmd.error == ParseError::None);
    CHECK(cmd.path == "user@d");
}

// 普通 forward-path 原样保留。
void testPlainPath() {
    Command cmd = parseCommand("RCPT TO:<user@example.com>");
    CHECK(cmd.error == ParseError::None);
    CHECK(cmd.path == "user@example.com");
}

// Quoted-string local-part 连同引号原样透传（含内部的 '>'）。
void testQuotedStringPassthrough() {
    Command cmd = parseCommand("MAIL FROM:<\"weird>name\"@example.com>");
    CHECK(cmd.verb == Verb::Mail);
    CHECK(cmd.error == ParseError::None);
    CHECK(cmd.path == "\"weird>name\"@example.com");
}

// SIZE= 与 BODY= 参数解析（参数名/值大小写不敏感）。
void testSizeAndBodyParams() {
    Command cmd = parseCommand("MAIL FROM:<a@b> size=1024 body=8BITMIME");
    CHECK(cmd.error == ParseError::None);
    CHECK(cmd.path == "a@b");
    CHECK(cmd.declaredSize == 1024);
    CHECK(cmd.body == BodyType::EightBitMime);

    Command seven = parseCommand("MAIL FROM:<a@b> BODY=7bit");
    CHECK(seven.error == ParseError::None);
    CHECK(seven.body == BodyType::SevenBit);
}

// 未知 ESMTP 参数 → BadParam(555)。
void testUnknownParamBadParam() {
    Command cmd = parseCommand("MAIL FROM:<a@b> FROBNICATE=1");
    CHECK(cmd.verb == Verb::Mail);
    CHECK(cmd.error == ParseError::BadParam);

    // 非法 BODY 取值同样 → BadParam。
    Command badBody = parseCommand("MAIL FROM:<a@b> BODY=9bit");
    CHECK(badBody.error == ParseError::BadParam);

    // 非数字 SIZE → BadParam。
    Command badSize = parseCommand("MAIL FROM:<a@b> SIZE=abc");
    CHECK(badSize.error == ParseError::BadParam);
}

// 尖括号内路径总长 > 256 → PathTooLong(501)；恰好 256 放行。
void testPathTooLong() {
    // "<" + 255 字节 mailbox + ">" == 257 octets，超限。
    std::string longMailbox(255, 'x');
    Command tooLong = parseCommand("MAIL FROM:<" + longMailbox + ">");
    CHECK(tooLong.verb == Verb::Mail);
    CHECK(tooLong.error == ParseError::PathTooLong);

    // "<" + 254 字节 + ">" == 256 octets，恰好合法。
    std::string maxMailbox(254, 'x');
    Command atLimit = parseCommand("MAIL FROM:<" + maxMailbox + ">");
    CHECK(atLimit.error == ParseError::None);
    CHECK(atLimit.path == maxMailbox);
}

// 缺尖括号 / 关键字拼写错 → Syntax(501)。
void testSyntaxErrors() {
    Command noAngle = parseCommand("MAIL FROM:user@b");
    CHECK(noAngle.verb == Verb::Mail);
    CHECK(noAngle.error == ParseError::Syntax);

    Command wrongKeyword = parseCommand("MAIL FRM:<a@b>");
    CHECK(wrongKeyword.error == ParseError::Syntax);

    Command rcptWrong = parseCommand("RCPT FROM:<a@b>");
    CHECK(rcptWrong.error == ParseError::Syntax);
}

// 动词大小写不敏感。
void testVerbCaseInsensitive() {
    CHECK(parseCommand("quit").verb == Verb::Quit);
    CHECK(parseCommand("QUIT").verb == Verb::Quit);
    CHECK(parseCommand("Data").verb == Verb::Data);
    CHECK(parseCommand("rSeT").verb == Verb::Rset);
    CHECK(parseCommand("NoOp").verb == Verb::Noop);
    CHECK(parseCommand("vrfy").verb == Verb::Vrfy);
    CHECK(parseCommand("bogus").verb == Verb::Unknown);
}

// HELO/EHLO 抓参数进 domain；地址字面量放行。
void testGreetingDomain() {
    Command helo = parseCommand("HELO client.example.org");
    CHECK(helo.verb == Verb::Helo);
    CHECK(helo.error == ParseError::None);
    CHECK(helo.domain == "client.example.org");

    Command ehlo = parseCommand("ehlo [1.2.3.4]");
    CHECK(ehlo.verb == Verb::Ehlo);
    CHECK(ehlo.error == ParseError::None);
    CHECK(ehlo.domain == "[1.2.3.4]");

    Command noArg = parseCommand("HELO");
    CHECK(noArg.error == ParseError::Syntax);
}

// AUTH 机制名解析：大小写不敏感并折为大写；无初始响应时 hasInitialResponse 为假。
void testAuthMechanism() {
    Command plain = parseCommand("AUTH PLAIN");
    CHECK(plain.verb == Verb::Auth);
    CHECK(plain.error == ParseError::None);
    CHECK(plain.mechanism == "PLAIN");
    CHECK(!plain.hasInitialResponse);

    Command lower = parseCommand("auth plain");
    CHECK(lower.verb == Verb::Auth);
    CHECK(lower.error == ParseError::None);
    CHECK(lower.mechanism == "PLAIN");

    Command login = parseCommand("AUTH LOGIN");
    CHECK(login.error == ParseError::None);
    CHECK(login.mechanism == "LOGIN");
}

// 初始响应原样保留（不解码）；"=" 表示"存在且为空"，与完全不带初始响应有别。
void testAuthInitialResponse() {
    Command empty = parseCommand("AUTH PLAIN =");
    CHECK(empty.error == ParseError::None);
    CHECK(empty.mechanism == "PLAIN");
    CHECK(empty.hasInitialResponse);
    CHECK(empty.initialResponse == "=");

    Command withIr = parseCommand("AUTH PLAIN AGFsaWNlAHB3");
    CHECK(withIr.error == ParseError::None);
    CHECK(withIr.hasInitialResponse);
    CHECK(withIr.initialResponse == "AGFsaWNlAHB3");
}

// AUTH 的语法错误：缺机制名、初始响应多于一个 token。
void testAuthSyntaxErrors() {
    Command noMech = parseCommand("AUTH");
    CHECK(noMech.verb == Verb::Auth);
    CHECK(noMech.error == ParseError::Syntax);

    Command extraToken = parseCommand("AUTH PLAIN a b");
    CHECK(extraToken.verb == Verb::Auth);
    CHECK(extraToken.error == ParseError::Syntax);
}

// 机制名字符集与长度（RFC 4422 §3.1）：1-20 字节，仅 ALPHA/DIGIT/'-'/'_'。
void testAuthMechanismCharset() {
    std::string tooLong(21, 'A');
    Command over = parseCommand("AUTH " + tooLong);
    CHECK(over.error == ParseError::Syntax);

    std::string atLimit(20, 'A');
    Command exact = parseCommand("AUTH " + atLimit);
    CHECK(exact.error == ParseError::None);
    CHECK(exact.mechanism == atLimit);

    // '.' 不在允许字符集内。
    Command badChar = parseCommand("AUTH PLA.IN");
    CHECK(badChar.error == ParseError::Syntax);

    // '-' 与 '_' 合法。
    Command hyphen = parseCommand("AUTH CRAM-MD5");
    CHECK(hyphen.error == ParseError::None);
    CHECK(hyphen.mechanism == "CRAM-MD5");
}

// RFC 4954 §5：MAIL FROM 上的 AUTH= 参数必须被接受（解析后丢弃），但白名单不得放宽
// 到其他未知参数。
void testMailFromAuthParam() {
    Command emptyAuth = parseCommand("MAIL FROM:<a@b> AUTH=<>");
    CHECK(emptyAuth.verb == Verb::Mail);
    CHECK(emptyAuth.error == ParseError::None);
    CHECK(emptyAuth.path == "a@b");

    Command withAddr = parseCommand("MAIL FROM:<a@b> AUTH=alice@example.com");
    CHECK(withAddr.error == ParseError::None);

    // 其余未知参数仍然 → BadParam(555)。
    Command bogus = parseCommand("MAIL FROM:<a@b> BOGUS=1");
    CHECK(bogus.error == ParseError::BadParam);
}

// Reply 多行序列化：前 n-1 行连字符形态，末行空格形态。
void testReplyMultilineSerialize() {
    Reply reply;
    reply.code = 250;
    reply.lines = {"first", "second", "last"};
    CHECK(serialize(reply) == "250-first\r\n250-second\r\n250 last\r\n");
}

// Reply::single 与单行序列化：单行必须用空格分隔符。
void testReplySingleSerialize() {
    Reply reply = Reply::single(220, "service ready");
    CHECK(reply.code == 220);
    CHECK(reply.lines.size() == 1);
    CHECK(serialize(reply) == "220 service ready\r\n");
}

}  // namespace

int main() {
    testEmptyReversePath();
    testSourceRouteStripped();
    testPlainPath();
    testQuotedStringPassthrough();
    testSizeAndBodyParams();
    testUnknownParamBadParam();
    testPathTooLong();
    testSyntaxErrors();
    testVerbCaseInsensitive();
    testGreetingDomain();
    testAuthMechanism();
    testAuthInitialResponse();
    testAuthSyntaxErrors();
    testAuthMechanismCharset();
    testMailFromAuthParam();
    testReplyMultilineSerialize();
    testReplySingleSerialize();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("all smtp_command tests passed");
    return 0;
}
