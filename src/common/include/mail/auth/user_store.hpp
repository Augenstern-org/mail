#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <semaphore>
#include <string>
#include <string_view>

#include "mail/result.hpp"

// 用户档的只读内存视图。档案格式为每行 "username:argon2id-crypt-string"。
// crypt 串是自描述的（含算法/版本/参数/盐），故不需要 {SCHEME} 前缀，也不需要
// 额外的参数列；其内容含 '$' 与 ',' 但绝不含 ':'，故按首个 ':' 切分是安全的。
//
// 生命周期：构造后内容不可变，查询是 const 且线程安全，可被多个连接线程共享。
// 服务端以 shared_ptr<const UserStore> 按值传给每个工作线程，使其在 main 返回后
// 仍然存活。

namespace mail::auth {

// 单次认证的裁决。沿用 RcptDecision / SinkStatus 确立的"小裁决枚举"先例；
// 不是错误枚举，也不与 IoStatus 重叠（本类型不描述 I/O）。
enum class AuthResult {
    Ok,          // 用户存在且口令匹配 → 235
    Rejected,    // 用户不存在或口令不匹配（二者对客户端不可区分）→ 535
    Unavailable  // 校验暂时无法进行（加密库故障）→ 454
};

class UserStore {
public:
    UserStore(const UserStore&) = delete;
    UserStore& operator=(const UserStore&) = delete;

    // 工厂：读取并全量解析 path 指向的用户档，成功后返回堆上的只读实例。
    //
    // 解析规则：
    //   - 逐行处理，行尾的 CR 一并剥除（容忍 CRLF 档案）。
    //   - 空行、以及首个非空白字符为 '#' 的行整行跳过（注释）。
    //   - 其余行按**首个** ':' 切分为用户名与 crypt 串；crypt 串内含 '$' 与 ','
    //     但绝不含 ':'，故首个冒号切分不会切坏它。
    //   - 用户名按 ASCII 大小写不敏感处理，折成小写后作为键。
    //   - 重复用户名以最后一次出现为准。
    //
    // 任一行格式非法（无冒号、用户名为空、crypt 串为空）都会让整个 open 失败，
    // 返回 IoStatus::Error 且 errno 为 0——用户档是配置，宁可启动即失败，也不要静
    // 默丢掉一个账号后带着残缺的档案上线。打开 / 读取失败同样返回 IoStatus::Error，
    // 但带上真实的 errno（如 ENOENT），据此可区分"档案有问题"与"档案读不到"。
    //
    // 为什么是 Result<std::unique_ptr<UserStore>> 而不是 Result<UserStore>（有意
    // 偏离 maildir.hpp:31 的 Result<MaildirStore> 先例）：Result<T> 是就地从一次移
    // 动构造出 T 的（见 result.hpp:29、:103），而本类持有 std::counting_semaphore
    // ——它既不可拷贝也不可移动，放不进 Result<T>。放在堆上同时正好是服务端需要的
    // 形状：拿到后可直接转成 shared_ptr<const UserStore> 分发给各工作线程。
    static Result<std::unique_ptr<UserStore>> open(const std::string& path);

    // 校验一对用户名 / 口令。
    //
    // 用户名按 ASCII 大小写不敏感匹配；口令按**原始字节**比对，不做 SASLprep 或任
    // 何 Unicode 规范化——RFC 4616 把 SASLprep 定为 RECOMMENDED 而非 MUST，Dovecot
    // 与 Postfix 默认也不做规范化，此处与之保持一致。
    //
    // 用户不存在时**不会**提前返回，而是拿 dummyEncoded() 做一次等价的 Argon2 校验
    // 后再返回 Rejected。否则"存在"要花约 100 ms 而"不存在"立即返回，构成可观测的
    // 用户枚举计时旁路。
    //
    // 整个 Argon2 工作处于 gate_ 的保护之下（RAII 获取 / 释放，覆盖所有退出路径）。
    // 理由：INTERACTIVE 档每次并发哈希占 64 MiB，AUTH 又是未认证即可触达的代码路
    // 径，而服务端是每连接一线程、本身不限并发——32 个并发尝试就是 2 GiB。
    //
    // 仅当加密设施确实故障时返回 Unavailable；口令错误一律是 Rejected。
    AuthResult authenticate(std::string_view username,
                            std::string_view password) const;

    // 用户名是否存在（ASCII 大小写不敏感）。不做任何口令运算，也不占用 gate_，故可
    // 在 RCPT 阶段的收件人校验里逐收件人调用。
    bool hasUser(std::string_view username) const;

    std::size_t size() const noexcept { return users_.size(); }

    // 同时进行的 Argon2 校验数上限。项目自定的资源上限，非协议常量。
    static constexpr std::ptrdiff_t kMaxConcurrentVerifications = 4;

private:
    explicit UserStore(std::map<std::string, std::string, std::less<>> users);

    std::map<std::string, std::string, std::less<>> users_;  // 小写用户名 → crypt 串

    // 并发闸门。mutable：authenticate 逻辑上是 const（不改 users_），但要动闸门。
    mutable std::counting_semaphore<kMaxConcurrentVerifications> gate_{
        kMaxConcurrentVerifications};
};

}  // namespace mail::auth
