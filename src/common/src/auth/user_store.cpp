#include "mail/auth/user_store.hpp"

#include <cerrno>
#include <cstddef>
#include <map>
#include <memory>
#include <semaphore>
#include <string>
#include <string_view>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

#include "mail/auth/password.hpp"
#include "mail/file_descriptor.hpp"
#include "mail/io_status.hpp"
#include "mail/result.hpp"

// UserStore 实现。档案一次性读入内存后逐行解析，解析完即不可变；此后所有查询都是
// const 且线程安全的。

namespace mail::auth {

namespace {

// ASCII 小写折叠。用户名限定 ASCII 语义，故不碰 locale，也不做 Unicode 大小写映射。
std::string toLowerAscii(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

// 一行是否应被跳过：空行，或首个非空白字符为 '#' 的注释行。
bool isBlankOrComment(std::string_view line) {
    for (char c : line) {
        if (c == ' ' || c == '\t') {
            continue;
        }
        return c == '#';
    }
    return true;  // 全是空白（含完全为空）
}

// 一次性读入整个文件。成功返回 0 并把内容放进 out；失败返回捕获的 errno。
int readWholeFile(const std::string& path, std::string& out) {
    FileDescriptor fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
    if (!fd) {
        return errno;
    }
    out.clear();
    char buf[4096];
    for (;;) {
        ssize_t n = ::read(fd.get(), buf, sizeof(buf));
        if (n > 0) {
            out.append(buf, static_cast<std::size_t>(n));
            continue;
        }
        if (n == 0) {
            return 0;  // EOF
        }
        if (errno == EINTR) {
            continue;  // 被信号打断，重试
        }
        return errno;
    }
}

// 并发闸门的 RAII 持有者：构造即 acquire，析构即 release，覆盖 authenticate 的每条
// 退出路径（含提前 return 与异常展开），避免手写 release() 漏掉某个分支。
class GateGuard {
public:
    explicit GateGuard(
        std::counting_semaphore<UserStore::kMaxConcurrentVerifications>& gate)
        : gate_(gate) {
        gate_.acquire();
    }

    GateGuard(const GateGuard&) = delete;
    GateGuard& operator=(const GateGuard&) = delete;

    ~GateGuard() { gate_.release(); }

private:
    std::counting_semaphore<UserStore::kMaxConcurrentVerifications>& gate_;
};

}  // namespace

UserStore::UserStore(std::map<std::string, std::string, std::less<>> users)
    : users_(std::move(users)) {}

Result<std::unique_ptr<UserStore>> UserStore::open(const std::string& path) {
    using ResultType = Result<std::unique_ptr<UserStore>>;

    std::string content;
    if (int err = readWholeFile(path, content); err != 0) {
        return ResultType::failure(IoStatus::Error, err);
    }

    std::map<std::string, std::string, std::less<>> users;
    std::string_view rest(content);
    while (!rest.empty()) {
        std::size_t nl = rest.find('\n');
        std::string_view line = (nl == std::string_view::npos)
                                    ? rest
                                    : rest.substr(0, nl);
        rest = (nl == std::string_view::npos) ? std::string_view()
                                              : rest.substr(nl + 1);

        // 容忍 CRLF 档案：剥掉行尾的 CR。
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (isBlankOrComment(line)) {
            continue;
        }

        // 按首个 ':' 切分。crypt 串含 '$' 与 ',' 但绝不含 ':'，故不会被切坏。
        std::size_t colon = line.find(':');
        if (colon == std::string_view::npos) {
            return ResultType::failure(IoStatus::Error, 0);
        }
        std::string_view username = line.substr(0, colon);
        std::string_view encoded = line.substr(colon + 1);
        if (username.empty() || encoded.empty()) {
            return ResultType::failure(IoStatus::Error, 0);
        }

        users[toLowerAscii(username)] = std::string(encoded);
    }

    // 构造函数是私有的，make_unique 够不到，故显式 new 后交给 unique_ptr。
    return ResultType::success(
        std::unique_ptr<UserStore>(new UserStore(std::move(users))));
}

AuthResult UserStore::authenticate(std::string_view username,
                                   std::string_view password) const {
    // 假串为空意味着 hashPassword 失败，即加密库确实不可用——此时既没法校验真用户，
    // 也没法做等价的假校验，只能如实报 Unavailable（对客户端是 454，可重试）。
    const std::string_view dummy = dummyEncoded();
    if (dummy.empty()) {
        return AuthResult::Unavailable;
    }

    const std::string key = toLowerAscii(username);
    const auto it = users_.find(key);
    const bool known = (it != users_.end());

    // 用户不存在时也走一次等价的 Argon2 校验（对假串），使两条路径耗时一致。
    const std::string_view encoded = known ? std::string_view(it->second) : dummy;

    GateGuard guard(gate_);
    const bool matched = verifyPassword(encoded, password);
    return (known && matched) ? AuthResult::Ok : AuthResult::Rejected;
}

bool UserStore::hasUser(std::string_view username) const {
    return users_.find(toLowerAscii(username)) != users_.end();
}

}  // namespace mail::auth
