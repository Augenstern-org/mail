// mail::base64Encode / base64Decode 的纯函数单元测试。无 I/O、无系统调用。
//
// 解码器的严格性是承载安全职责的（其输出直接喂给 SMTP AUTH 的凭据解析），故负例
// 覆盖为重点：非 4 的倍数、字母表外字符（含 CR/LF 与空白）、URL 变体、错位或多余的
// '='、非零填充位。
//
// 不使用测试框架：一个极小的 CHECK 宏记录失败，任一检查失败时进程返回非零（沿用
// maildir_name_test.cpp 的模式）。

#include <cstdio>
#include <string>

#include "mail/base64.hpp"

using mail::base64Decode;
using mail::base64Encode;

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

// 编码与解码在同一组向量上互为逆运算，故成对断言。
void checkVector(const std::string& plain, const std::string& encoded) {
    CHECK(base64Encode(plain) == encoded);

    std::string decoded;
    CHECK(base64Decode(encoded, decoded));
    CHECK(decoded == plain);
}

// 解码必须失败；out 在失败后内容未定义，故只断言返回值。
void checkReject(std::string_view text) {
    std::string out;
    CHECK(!base64Decode(text, out));
}

// 1. RFC 4648 §10 的官方测试向量，双向。
void testRfcVectors() {
    checkVector("", "");
    checkVector("f", "Zg==");
    checkVector("fo", "Zm8=");
    checkVector("foo", "Zm9v");
    checkVector("foob", "Zm9vYg==");
    checkVector("fooba", "Zm9vYmE=");
    checkVector("foobar", "Zm9vYmFy");
}

// 2. 全字节域往返：0x00–0xFF 一次编码再解码，逐字节相等。
void testFullByteRoundTrip() {
    std::string plain;
    plain.reserve(256);
    for (int i = 0; i < 256; ++i) {
        plain += static_cast<char>(i);
    }

    std::string decoded;
    CHECK(base64Decode(base64Encode(plain), decoded));
    CHECK(decoded.size() == 256);
    CHECK(decoded == plain);
}

// 3. 嵌入 NUL：全程按长度处理，绝不在 NUL 处截断。
void testEmbeddedNul() {
    const std::string plain("a\0b", 3);

    std::string decoded;
    CHECK(base64Decode(base64Encode(plain), decoded));
    CHECK(decoded.size() == 3);
    CHECK(decoded[0] == 'a');
    CHECK(decoded[1] == '\0');
    CHECK(decoded[2] == 'b');
}

// 4. 空串解码成功且产出空串；解码前 out 的旧内容被整体替换。
void testEmptyDecode() {
    std::string out = "stale";
    CHECK(base64Decode("", out));
    CHECK(out.empty());
}

// 5. 长度不是 4 的倍数——一律拒绝。
void testRejectBadLength() {
    checkReject("Zg=");
    checkReject("Zg===");
    checkReject("Z");
    checkReject("Zm9vYmFyZ");
}

// 6. 字母表外字符——空白与 CR/LF 是重点：LineReader 在上游已剥掉 CRLF，任何抵达
// 解码器的 CR/LF 都属注入企图，必须拒绝。
void testRejectNonAlphabet() {
    checkReject("Z g==");
    checkReject("Zg==\r\n");
    checkReject("Z g=");
    checkReject("Zm\r\n");
    checkReject("Zm9v\n");
    checkReject("Zm9\t");
    checkReject("Zm9*");
}

// 7. URL-safe 变体的 '-' 与 '_' 不属于标准字母表。
void testRejectUrlAlphabet() {
    checkReject("Zm9-");
    checkReject("Zm9_");
}

// 8. '=' 只能出现在末四元组的第 3、4 位；错位、过量、或填充后再出现字母表字符均拒绝。
void testRejectBadPadding() {
    checkReject("=Zg=");
    checkReject("====");
    checkReject("Z=g=");
    checkReject("Zg=v");
    checkReject("Zg==Zm9v");
}

// 9. 非零填充位：两个 '=' 时第二个六位组的低 4 位、一个 '=' 时第三个六位组的低 2 位
// 必须为零，否则同一明文会有多种编码形式。
void testRejectNonZeroPaddingBits() {
    checkReject("Zh==");
    checkReject("Zm9=");

    // 对照：同长度下填充位为零的输入必须被接受。
    std::string out;
    CHECK(base64Decode("Zg==", out));
    CHECK(out == "f");
    CHECK(base64Decode("Zm8=", out));
    CHECK(out == "fo");
}

}  // namespace

int main() {
    testRfcVectors();
    testFullByteRoundTrip();
    testEmbeddedNul();
    testEmptyDecode();
    testRejectBadLength();
    testRejectNonAlphabet();
    testRejectUrlAlphabet();
    testRejectBadPadding();
    testRejectNonZeroPaddingBits();
    return g_failures == 0 ? 0 : 1;
}
