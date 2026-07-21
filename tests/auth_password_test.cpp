// mail::auth 口令哈希层的测试。覆盖 initCrypto 的幂等初始化、Argon2id crypt 串的
// 形状、正确/错误口令的校验、盐的随机性、含嵌入 NUL 与非 ASCII 字节的口令往返、
// 不可解析输入的容错、假串的可用性，以及 scrub 的抹除语义。
//
// 不使用测试框架：一个极小的 CHECK 宏记录失败，任一检查失败时进程返回非零（沿用
// maildir_store_test.cpp / smtp_session_test.cpp 的模式）。
//
// 开销约束：每次 Argon2 操作约 100 ms 且分配 64 MiB，故本文件刻意把哈希/校验总次数
// 控制在 10 次以内（实测 9 次：3 次 hashPassword，5 次真校验，1 次假串生成）。

#include <cstdio>
#include <string>
#include <string_view>

#include "mail/auth/password.hpp"

using mail::auth::dummyEncoded;
using mail::auth::hashPassword;
using mail::auth::initCrypto;
using mail::auth::scrub;
using mail::auth::verifyPassword;

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

// 1. initCrypto 成功，且幂等——重复调用仍返回 true。
void testInitCrypto() {
    CHECK(initCrypto());
    CHECK(initCrypto());
}

// 2. crypt 串的形状 + 正确/错误口令的校验 + 盐的随机性。
//    Argon2 开销：2 次 hashPassword + 2 次 verifyPassword = 4 次。
void testHashShapeAndVerify() {
    const std::string password = "correct horse battery staple";

    std::string first;
    CHECK(hashPassword(password, first));
    CHECK(first.rfind("$argon2id$", 0) == 0);
    CHECK(first.size() < 128);

    CHECK(verifyPassword(first, password));
    CHECK(!verifyPassword(first, "correct horse battery stapl"));

    // 同一口令的两次哈希必须不同：盐是每次随机取的。
    std::string second;
    CHECK(hashPassword(password, second));
    CHECK(first != second);
}

// 3. 口令是任意字节串，不是 C 串：含嵌入 NUL 与非 ASCII UTF-8 的口令必须完整往返，
//    且按 NUL 截断后的前缀必须被拒。这是本文件最关键的一条（RFC 4616 的字节语义）。
//    Argon2 开销：1 次 hashPassword + 2 次 verifyPassword = 3 次。
void testEmbeddedNulAndUtf8() {
    std::string password = "pass";
    password.push_back('\0');
    password += "\xc3\xa9\xe4\xb8\xad";  // U+00E9 é、U+4E2D 中
    CHECK(password.size() == 10);

    std::string encoded;
    CHECK(hashPassword(password, encoded));
    CHECK(verifyPassword(encoded, password));

    // strlen 语义下会被看成 "pass"——若实现误用 c_str()，这条会通过，从而漏掉 NUL
    // 之后的全部字节。必须为 false。
    CHECK(!verifyPassword(encoded, std::string_view(password.data(), 4)));
}

// 4. 不可解析的 encoded 一律返回 false 且不崩溃；不区分"档损坏"与"口令错"。
//    Argon2 开销：0 次（这些输入在解析阶段即被拒）。
void testUnparseableEncoded() {
    CHECK(!verifyPassword("garbage", "x"));
    CHECK(!verifyPassword("", "x"));
    CHECK(!verifyPassword("$argon2id$truncated", "x"));
    // 长度 >= crypto_pwhash_STRBYTES(128) 的输入本就不可能是合法 crypt 串。
    CHECK(!verifyPassword(std::string(200, 'a'), "x"));
}

// 5. 假串必须是语法合法的 Argon2id crypt 串（否则假校验会提前失败，耗时对不上），
//    且拒绝任意口令。Argon2 开销：1 次假串生成 + 1 次 verifyPassword = 2 次。
void testDummyEncoded() {
    const std::string_view dummy = dummyEncoded();
    CHECK(!dummy.empty());
    CHECK(dummy.rfind("$argon2id$", 0) == 0);
    CHECK(dummy.size() < 128);

    // 同一进程内多次调用返回同一份（函数局部 static），不重复付哈希成本。
    CHECK(dummyEncoded().data() == dummy.data());

    CHECK(!verifyPassword(dummy, "whatever the client sent"));
}

// 6. scrub：长度不变、内容全零；空串不崩。
void testScrub() {
    std::string secret = "hunter2";
    const std::size_t original = secret.size();
    scrub(secret);
    CHECK(secret.size() == original);
    bool allZero = true;
    for (char c : secret) {
        if (c != '\0') {
            allZero = false;
        }
    }
    CHECK(allZero);

    std::string empty;
    scrub(empty);
    CHECK(empty.empty());
}

}  // namespace

int main() {
    testInitCrypto();
    testHashShapeAndVerify();
    testEmbeddedNulAndUtf8();
    testUnparseableEncoded();
    testDummyEncoded();
    testScrub();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("all auth_password tests passed");
    return 0;
}
