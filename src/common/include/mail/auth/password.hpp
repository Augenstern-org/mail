#pragma once

#include <string>
#include <string_view>

// libsodium Argon2id 口令哈希的薄封装。<sodium.h> 只出现在 src/auth/password.cpp，
// 绝不出现在任何公开头文件里（保持 mailcommon 的公开接口不泄漏第三方类型）。
//
// 前置条件：调用本文件其余任何函数之前，进程必须已成功调用 initCrypto()。

namespace mail::auth {

// 一次性初始化底层加密库（内部即 sodium_init）。线程安全、幂等。返回 false 表示
// 库不可用，调用方应直接终止，不得在未初始化状态下继续。
bool initCrypto();

// 用 Argon2id（INTERACTIVE 档：opslimit=2、memlimit=64 MiB）哈希口令，产出自描述
// 的 crypt 串（"$argon2id$" 开头，ASCII，内含算法/版本/参数/盐/摘要，长度 < 128）。
// 成功返回 true 并整体替换 outEncoded。password 按原始字节处理，可含嵌入 NUL 与
// 非 ASCII；不做 SASLprep / Unicode 规范化。
bool hashPassword(std::string_view password, std::string& outEncoded);

// 按 encoded 中自描述的参数校验 password。encoded 无法解析（用户档损坏）与口令不
// 匹配都返回 false —— 调用方不得据此区分二者，以免泄漏用户档状态。
bool verifyPassword(std::string_view encoded, std::string_view password);

// 一个固定的、语法合法的 Argon2id crypt 串，用于"用户不存在"时的假校验，使存在与
// 不存在两条路径的耗时一致（缓解用户枚举：真实用户约 100 ms，缺失用户若直接返回
// 则是立即返回，差异可观测）。
//
// 实现上是首次调用时用 hashPassword 现算一次并存入函数局部 static（见
// password.cpp）。相对写死字面量的好处是：档位常量一旦调整，假串的参数自动跟着
// 变，不会悄悄退化成"假校验比真校验便宜"。返回空串表示底层库不可用（hashPassword
// 失败），调用方可据此判定加密设施故障。
std::string_view dummyEncoded();

// 就地抹除敏感缓冲（内部即 sodium_memzero；普通 memset 可能被优化掉）。
// noexcept，s 可为空。抹除后 s.size() 不变、内容全零。
void scrub(std::string& s) noexcept;

}  // namespace mail::auth
