#include "mail/auth/password.hpp"

#include <cstring>
#include <string>
#include <string_view>

#include <sodium.h>

// Argon2id 口令哈希实现。全项目仅本文件 #include <sodium.h>，故 CMake 侧的
// libsodium 链接可以是 PRIVATE，mailcommon 的公开头文件不泄漏第三方类型。
//
// 档位固定为 INTERACTIVE（opslimit=2、memlimit=64 MiB）。不用 MODERATE /
// SENSITIVE：那两档每次操作分别要 256 MiB / 1 GiB，对"每连接一次"的认证路径过重。
//
// 口令一律按 (指针, 长度) 传递，绝不经 strlen / c_str() 的 C 串语义——RFC 4616 的
// 口令是任意字节串，可含嵌入 NUL 与非 ASCII UTF-8。

namespace mail::auth {

namespace {

// 空 string_view 的 data() 可能是 nullptr，而把 (nullptr, 0) 交给 libsodium 属未定义
// 行为。统一在此兜一个合法的非空指针。
const char* bytesOrEmpty(std::string_view s) noexcept {
    return s.empty() ? "" : s.data();
}

}  // namespace

bool initCrypto() {
    // sodium_init：0 表示本次初始化成功，1 表示此前已初始化，-1 表示失败。
    return ::sodium_init() >= 0;
}

bool hashPassword(std::string_view password, std::string& outEncoded) {
    char encoded[crypto_pwhash_STRBYTES];
    if (::crypto_pwhash_str(encoded, bytesOrEmpty(password), password.size(),
                            crypto_pwhash_OPSLIMIT_INTERACTIVE,
                            crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0) {
        return false;  // 内存分配失败等；此时 outEncoded 保持不变
    }
    // crypto_pwhash_str 产出的是 NUL 结尾的 ASCII 串，长度必然 < STRBYTES。
    outEncoded.assign(encoded);
    return true;
}

bool verifyPassword(std::string_view encoded, std::string_view password) {
    // crypto_pwhash_str_verify 要求 str 是 NUL 结尾、且容量为 STRBYTES 的缓冲，而
    // encoded 是 string_view，未必带终止符。先拷进定长缓冲并补 NUL；长度 >=
    // STRBYTES 的输入本就不可能是合法 crypt 串，直接判为不可解析。
    if (encoded.size() >= crypto_pwhash_STRBYTES) {
        return false;
    }
    char buf[crypto_pwhash_STRBYTES];
    if (!encoded.empty()) {
        std::memcpy(buf, encoded.data(), encoded.size());
    }
    buf[encoded.size()] = '\0';

    // 不可解析（用户档损坏）与口令不匹配在此合流为 false，调用方无从区分。
    return ::crypto_pwhash_str_verify(buf, bytesOrEmpty(password),
                                      password.size()) == 0;
}

std::string_view dummyEncoded() {
    // 函数局部 static：首次调用现算一次，之后零成本复用。C++11 起 static 局部变量的
    // 初始化是线程安全的，故多个连接线程并发首次调用也安全。
    //
    // 选择"现算"而非写死字面量：假校验的耗时必须和真校验一致，而 crypt 串是自描述
    // 的——耗时由串里的参数决定。现算保证假串的档位永远等于 hashPassword 当下用的
    // 档位，档位常量改动时不会退化成廉价的假校验。代价是首次调用多一次哈希。
    //
    // 哈希的原文是一个固定的一次性字符串，本身不构成任何有效凭据：假校验只关心耗时，
    // 任何客户端口令与之匹配的概率可忽略。
    static const std::string kDummy = [] {
        std::string encoded;
        if (!hashPassword("mail::auth dummy password", encoded)) {
            return std::string();  // 底层库不可用；调用方据空串判定加密设施故障
        }
        return encoded;
    }();
    return kDummy;
}

void scrub(std::string& s) noexcept {
    if (s.empty()) {
        return;
    }
    // 普通 memset 可能被编译器当作死存储消去；sodium_memzero 保证不被优化掉。
    ::sodium_memzero(s.data(), s.size());
}

}  // namespace mail::auth
