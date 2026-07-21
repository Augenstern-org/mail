#include "mail/auth/sasl.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

#include "mail/auth/password.hpp"
#include "mail/auth/user_store.hpp"

// SASL 机制实现。两个机制共用一个基类，基类持有裁决、凭据副本与"已开始 / 已结束"
// 两个状态位；机制子类只负责自己的文法。
//
// 凭据卫生：用户名与口令都以 std::string 成员留存（而非指向调用方缓冲的
// string_view），一是跨轮次的生命周期安全，二是这样才有一块自己的缓冲可抹除——
// 口令在用完的那一刻即 scrub，两者在析构时再 scrub 一次兜底。
//
// 全部比较按**原始字节**进行，不做 SASLprep 或任何 Unicode 规范化：RFC 4616 把
// SASLprep 定为 RECOMMENDED 而非 MUST，Dovecot 与 Postfix 默认也不规范化，此处与
// user_store.cpp 的口令比对保持同一约定。

namespace mail::auth {

namespace {

constexpr std::string_view kSupportedMechanisms = "PLAIN LOGIN";

// LOGIN 的两个挑战串。**刻意用带冒号的形式**，而非 draft-murchison-sasl-login-00
// 里的 "User Name" / "Password"：Dovecot / Postfix / Exim 一律发冒号形式，因为有
// 广泛部署的客户端硬性依赖它，draft 自身也承认了这点。base64 后分别为
// "VXNlcm5hbWU6" / "UGFzc3dvcmQ6"，故这两个串必须逐字节精确。
constexpr std::string_view kUsernameChallenge = "Username:";
constexpr std::string_view kPasswordChallenge = "Password:";

// ASCII 大小写不敏感的相等比较。用于机制名与 authzid/authcid 的比对，二者都限定
// ASCII 语义，故不碰 locale，也不做 Unicode 大小写映射。
bool equalsIgnoreAsciiCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') {
            ca = static_cast<char>(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = static_cast<char>(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

// 两个机制的共同底座：借用的 store、凭据副本、裁决与状态位。
class MechanismBase : public Sasl {
public:
    explicit MechanismBase(const UserStore& store) : store_(store) {}

    ~MechanismBase() override {
        scrub(username_);
        scrub(password_);
    }

    AuthResult result() const override { return result_; }

    std::string_view authenticatedUser() const override { return username_; }

protected:
    // 收束到终态：记下裁决、立刻抹掉口令副本、清空未用上的挑战缓冲。
    // 交换尚未走完就被误驱动时（begin 调两次、Done 后再 step），传入现有的
    // result_ 即可原样保持既有裁决，不会把一次成功的认证改写掉。
    SaslStep finish(AuthResult decision, std::string& outChallenge) {
        result_ = decision;
        done_ = true;
        started_ = true;
        scrub(password_);
        password_.clear();
        outChallenge.clear();
        return SaslStep::Done;
    }

    const UserStore& store_;
    std::string username_;
    std::string password_;

    // 交换未正常走完就被取值时的保守默认：拒绝。
    AuthResult result_ = AuthResult::Rejected;

    bool started_ = false;  // begin() 或首次 step() 已调用过
    bool done_ = false;     // 已产出终局裁决
};

// PLAIN（RFC 4616）。载荷文法：message = [authzid] NUL authcid NUL passwd，
// 两个 NUL 分隔符**恒定存在**，即便 authzid 为空（常态）也是如此。
class PlainSasl final : public MechanismBase {
public:
    using MechanismBase::MechanismBase;

    SaslStep begin(std::string& outChallenge) override {
        if (started_ || done_) {
            return finish(result_, outChallenge);
        }
        started_ = true;
        outChallenge.clear();  // PLAIN 的挑战为空（线路上是 "334 "）
        return SaslStep::Challenge;
    }

    SaslStep step(std::string_view response, std::string& outChallenge) override {
        if (done_) {
            return finish(result_, outChallenge);
        }
        started_ = true;

        // 只按**前两个** NUL 切分：第二个 NUL 之后的一切原样就是口令，它自身可以
        // 再含 NUL 字节与任意 UTF-8。载荷不是 C 字符串，故全程用 string_view 的
        // 长度语义，绝不经手 strlen / strtok / c_str()——那会在首个 NUL 处静默截断。
        const std::size_t firstNul = response.find('\0');
        if (firstNul == std::string_view::npos) {
            return finish(AuthResult::Rejected, outChallenge);
        }
        const std::size_t secondNul = response.find('\0', firstNul + 1);
        if (secondNul == std::string_view::npos) {
            return finish(AuthResult::Rejected, outChallenge);
        }

        const std::string_view authzid = response.substr(0, firstNul);
        const std::string_view authcid =
            response.substr(firstNul + 1, secondNul - firstNul - 1);
        const std::string_view passwd = response.substr(secondNul + 1);

        if (authcid.empty()) {
            return finish(AuthResult::Rejected, outChallenge);
        }

        // 本服务端不支持"代理身份"（acting-as）：authzid 非空且不等于 authcid 时
        // 直接拒绝。相等或为空则照常认证。文法错误与凭据不符在线路上都是 535，
        // 故二者不作区分。
        if (!authzid.empty() && !equalsIgnoreAsciiCase(authzid, authcid)) {
            return finish(AuthResult::Rejected, outChallenge);
        }

        // RFC 4616 要求服务端至少接受各 255 octet 的 authzid/authcid/passwd；此处
        // 不设任何更紧的长度上限（行长上限由 Session 一侧把关）。
        username_.assign(authcid);
        password_.assign(passwd);
        return finish(store_.authenticate(username_, password_), outChallenge);
    }
};

// LOGIN（draft-murchison-sasl-login-00，IANA 状态为 OBSOLETE，但客户端仍在广泛
// 使用）。两轮：先要用户名，再要口令。
class LoginSasl final : public MechanismBase {
public:
    using MechanismBase::MechanismBase;

    SaslStep begin(std::string& outChallenge) override {
        if (started_ || done_) {
            return finish(result_, outChallenge);
        }
        started_ = true;
        outChallenge.assign(kUsernameChallenge);
        return SaslStep::Challenge;
    }

    SaslStep step(std::string_view response, std::string& outChallenge) override {
        if (done_) {
            return finish(result_, outChallenge);
        }
        started_ = true;

        // 首次 step 收到的就是用户名——无论前面是否调过 begin()。客户端在命令行上
        // 给了初始响应时（"AUTH LOGIN <b64-user>"），Session 会跳过 begin() 直接
        // 走到这里，这两条路径在本机制看来是同一个状态。
        if (!haveUsername_) {
            if (response.empty()) {
                return finish(AuthResult::Rejected, outChallenge);
            }
            username_.assign(response);
            haveUsername_ = true;
            outChallenge.assign(kPasswordChallenge);
            return SaslStep::Challenge;
        }

        password_.assign(response);
        return finish(store_.authenticate(username_, password_), outChallenge);
    }

private:
    bool haveUsername_ = false;
};

}  // namespace

std::unique_ptr<Sasl> makeSasl(std::string_view mechanism,
                               const UserStore& store) {
    if (equalsIgnoreAsciiCase(mechanism, "PLAIN")) {
        return std::make_unique<PlainSasl>(store);
    }
    if (equalsIgnoreAsciiCase(mechanism, "LOGIN")) {
        return std::make_unique<LoginSasl>(store);
    }
    return nullptr;
}

std::string_view supportedMechanisms() { return kSupportedMechanisms; }

}  // namespace mail::auth
