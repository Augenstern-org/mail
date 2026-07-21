// mail::auth::Sasl（PLAIN / LOGIN）的单元测试。机制对象只见**解码后的原始字节**，
// 故本文件完全不涉及 base64、回码与线路格式——那些是 Session 的职责。测试在 mkdtemp
// 建立的临时目录内放一份真实用户档，用真实的 UserStore 驱动机制。
//
// 不使用测试框架：一个极小的 CHECK 宏记录失败，任一检查失败时进程返回非零（沿用
// auth_user_store_test.cpp / maildir_store_test.cpp 的模式）。
//
// 开销约束：每次 Argon2 操作约 100 ms 且分配 64 MiB。两份口令哈希在 main 里只算一次；
// 真正触达 store.authenticate 的用例控制在 8 个以内，文法非法的用例在到达 Argon2 之
// 前就短路返回，不计开销。

#include <cstdio>
#include <fstream>
#include <memory>
#include <string>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mail/auth/password.hpp"
#include "mail/auth/sasl.hpp"
#include "mail/auth/user_store.hpp"

using mail::auth::AuthResult;
using mail::auth::Sasl;
using mail::auth::SaslStep;
using mail::auth::UserStore;
using mail::auth::makeSasl;

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

// 两份口令的哈希在 main 里算一次，全局复用（每次 Argon2 约 100 ms）。
std::string g_aliceHash;
std::string g_nulHash;

const std::string kAlicePassword = "hunter2";

// 含**嵌入 NUL** 的口令，长度 7 字节。PLAIN 载荷按前两个 NUL 切分后，第二个 NUL
// 之后的一切原样都是口令——若实现误按 C 字符串语义处理，这里会被截成 "hun"。
const std::string kNulPassword("hun\0ter", 7);

// 全局唯一的用户档与其 store：alice / hunter2 与 nulluser / "hun\0ter"。
std::string g_tempDir;
std::unique_ptr<UserStore> g_store;

// mkdtemp 建立一个独立临时目录，返回其绝对路径（失败返回空串）。
std::string makeTempDir() {
    char tmpl[] = "/tmp/auth_sasl_test_XXXXXX";
    char* p = ::mkdtemp(tmpl);
    return p ? std::string(p) : std::string();
}

// 把 content 原样写入 path。
void writeFile(const std::string& path, const std::string& content) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << content;
}

// 递归删除 path（文件或目录）。尽力而为，忽略中途错误。
void removeRecursive(const std::string& path) {
    struct stat st{};
    if (::lstat(path.c_str(), &st) != 0) {
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        DIR* d = ::opendir(path.c_str());
        if (d) {
            while (dirent* e = ::readdir(d)) {
                std::string name = e->d_name;
                if (name == "." || name == "..") {
                    continue;
                }
                removeRecursive(path + "/" + name);
            }
            ::closedir(d);
        }
        ::rmdir(path.c_str());
    } else {
        ::unlink(path.c_str());
    }
}

// 组装一条 PLAIN 载荷：authzid NUL authcid NUL passwd。三段都按长度拼接，故任一段
// 都可以含嵌入 NUL。
std::string plainPayload(const std::string& authzid, const std::string& authcid,
                         const std::string& passwd) {
    std::string out;
    out.append(authzid);
    out.push_back('\0');
    out.append(authcid);
    out.push_back('\0');
    out.append(passwd);
    return out;
}

// 1. 工厂：机制名 ASCII 大小写不敏感；不支持的名字与空名字返回 nullptr。
//    Argon2 开销：0 次。
void testFactory() {
    CHECK(makeSasl("PLAIN", *g_store) != nullptr);
    CHECK(makeSasl("plain", *g_store) != nullptr);
    CHECK(makeSasl("LOGIN", *g_store) != nullptr);
    CHECK(makeSasl("Login", *g_store) != nullptr);
    CHECK(makeSasl("CRAM-MD5", *g_store) == nullptr);
    CHECK(makeSasl("", *g_store) == nullptr);

    CHECK(mail::auth::supportedMechanisms() == "PLAIN LOGIN");
}

// 2. PLAIN 带初始响应的常规路径：authzid 为空，一步走完。
//    Argon2 开销：1 次校验。
void testPlainInitialResponse() {
    auto sasl = makeSasl("PLAIN", *g_store);
    CHECK(sasl != nullptr);
    if (!sasl) {
        return;
    }
    std::string challenge = "sentinel";  // 应被实现清空
    const std::string payload = plainPayload("", "alice", kAlicePassword);
    CHECK(payload.size() == 14);  // NUL + "alice" + NUL + "hunter2"

    CHECK(sasl->step(payload, challenge) == SaslStep::Done);
    CHECK(sasl->result() == AuthResult::Ok);
    CHECK(sasl->authenticatedUser() == "alice");
}

// 3. PLAIN 带 authzid：与 authcid 相同则照常认证（本服务端只是不支持"代理身份"，
//    并非不接受 authzid 字段）。
//    Argon2 开销：1 次校验。
void testPlainWithMatchingAuthzid() {
    auto sasl = makeSasl("PLAIN", *g_store);
    CHECK(sasl != nullptr);
    if (!sasl) {
        return;
    }
    std::string challenge;
    CHECK(sasl->step(plainPayload("alice", "alice", kAlicePassword),
                     challenge) == SaslStep::Done);
    CHECK(sasl->result() == AuthResult::Ok);
    CHECK(sasl->authenticatedUser() == "alice");
}

// 4. PLAIN 不带初始响应：begin() 必须产出**空**挑战（线路上即 "334 "），随后
//    step() 走完。
//    Argon2 开销：1 次校验。
void testPlainNoInitialResponse() {
    auto sasl = makeSasl("PLAIN", *g_store);
    CHECK(sasl != nullptr);
    if (!sasl) {
        return;
    }
    std::string challenge = "sentinel";
    CHECK(sasl->begin(challenge) == SaslStep::Challenge);
    CHECK(challenge.empty());

    CHECK(sasl->step(plainPayload("", "alice", kAlicePassword), challenge) ==
          SaslStep::Done);
    CHECK(sasl->result() == AuthResult::Ok);
    CHECK(sasl->authenticatedUser() == "alice");
}

// 5. **本文件最重要的用例**：口令自身含 NUL。载荷只按**前两个** NUL 切分，第二个
//    NUL 之后的 7 个字节 "hun\0ter" 原样都是口令。若实现按 C 字符串语义处理（strlen /
//    strtok / c_str()），口令会被截成 "hun" 而校验失败——故 Ok 即证明未被截断。
//    Argon2 开销：1 次校验。
void testPlainPasswordWithEmbeddedNul() {
    auto sasl = makeSasl("PLAIN", *g_store);
    CHECK(sasl != nullptr);
    if (!sasl) {
        return;
    }
    const std::string payload = plainPayload("", "nulluser", kNulPassword);
    CHECK(payload.size() == 1 + 8 + 1 + 7);
    CHECK(kNulPassword.size() == 7);

    std::string challenge;
    CHECK(sasl->step(payload, challenge) == SaslStep::Done);
    CHECK(sasl->result() == AuthResult::Ok);
    CHECK(sasl->authenticatedUser() == "nulluser");
}

// 6. PLAIN 文法非法：无 NUL、只有一个 NUL、空载荷、authcid 为空。一律 Done +
//    Rejected（线路上都是 535，不单列 Malformed），且在到达 Argon2 之前短路。
//    Argon2 开销：0 次。
void testPlainMalformed() {
    const std::string cases[] = {
        std::string(""),                    // 空载荷
        std::string("alice hunter2"),       // 完全没有 NUL
        std::string("\0alice", 6),          // 只有一个 NUL
        std::string("\0\0hunter2", 9),      // authcid 为空
    };

    for (const std::string& payload : cases) {
        auto sasl = makeSasl("PLAIN", *g_store);
        CHECK(sasl != nullptr);
        if (!sasl) {
            continue;
        }
        std::string challenge;
        CHECK(sasl->step(payload, challenge) == SaslStep::Done);
        CHECK(sasl->result() == AuthResult::Rejected);
    }
}

// 7. PLAIN authzid 非空且不等于 authcid：本服务端不支持"代理身份"，直接拒绝，
//    且不做任何口令运算。
//    Argon2 开销：0 次。
void testPlainRejectsForeignAuthzid() {
    auto sasl = makeSasl("PLAIN", *g_store);
    CHECK(sasl != nullptr);
    if (!sasl) {
        return;
    }
    std::string challenge;
    CHECK(sasl->step(plainPayload("bob", "alice", kAlicePassword), challenge) ==
          SaslStep::Done);
    CHECK(sasl->result() == AuthResult::Rejected);
}

// 8. PLAIN 口令错误：Rejected（与用户不存在对客户端不可区分）。
//    Argon2 开销：1 次校验。
void testPlainWrongPassword() {
    auto sasl = makeSasl("PLAIN", *g_store);
    CHECK(sasl != nullptr);
    if (!sasl) {
        return;
    }
    std::string challenge;
    CHECK(sasl->step(plainPayload("", "alice", "wrong"), challenge) ==
          SaslStep::Done);
    CHECK(sasl->result() == AuthResult::Rejected);
}

// 9. RFC 4616 要求服务端至少接受各 255 octet 的 authcid 与 passwd。解析器不得对其
//    截断或崩溃：这里没有这样的用户，故裁决是 Rejected，但必须是"走完解析后认证不
//    通过"，而不是文法层面就出事。
//    Argon2 开销：1 次校验（对假串的等价校验）。
void testPlainLongFields() {
    auto sasl = makeSasl("PLAIN", *g_store);
    CHECK(sasl != nullptr);
    if (!sasl) {
        return;
    }
    const std::string longUser(255, 'u');
    const std::string longPass(255, 'p');
    const std::string payload = plainPayload("", longUser, longPass);
    CHECK(payload.size() == 1 + 255 + 1 + 255);

    std::string challenge;
    CHECK(sasl->step(payload, challenge) == SaslStep::Done);
    CHECK(sasl->result() == AuthResult::Rejected);
}

// 10. LOGIN 不带初始响应：两个挑战串必须逐字节精确（base64 后即客户端硬性依赖的
//     "VXNlcm5hbWU6" / "UGFzc3dvcmQ6"）。
//     Argon2 开销：1 次校验。
void testLoginNoInitialResponse() {
    auto sasl = makeSasl("LOGIN", *g_store);
    CHECK(sasl != nullptr);
    if (!sasl) {
        return;
    }
    std::string challenge;
    CHECK(sasl->begin(challenge) == SaslStep::Challenge);
    CHECK(challenge == "Username:");

    CHECK(sasl->step("alice", challenge) == SaslStep::Challenge);
    CHECK(challenge == "Password:");

    CHECK(sasl->step(kAlicePassword, challenge) == SaslStep::Done);
    CHECK(sasl->result() == AuthResult::Ok);
    CHECK(sasl->authenticatedUser() == "alice");
}

// 11. LOGIN 带初始响应：初始响应**就是用户名**，首次 step 直接进到口令挑战——与
//     走过 begin() 的路径在机制看来是同一个状态。
//     Argon2 开销：1 次校验。
void testLoginWithInitialResponse() {
    auto sasl = makeSasl("LOGIN", *g_store);
    CHECK(sasl != nullptr);
    if (!sasl) {
        return;
    }
    std::string challenge;
    CHECK(sasl->step("alice", challenge) == SaslStep::Challenge);
    CHECK(challenge == "Password:");

    CHECK(sasl->step(kAlicePassword, challenge) == SaslStep::Done);
    CHECK(sasl->result() == AuthResult::Ok);
    CHECK(sasl->authenticatedUser() == "alice");
}

// 12. LOGIN 用户名为空：直接拒绝，不进口令轮。
//     Argon2 开销：0 次。
void testLoginEmptyUsername() {
    auto sasl = makeSasl("LOGIN", *g_store);
    CHECK(sasl != nullptr);
    if (!sasl) {
        return;
    }
    std::string challenge;
    CHECK(sasl->begin(challenge) == SaslStep::Challenge);
    CHECK(sasl->step("", challenge) == SaslStep::Done);
    CHECK(sasl->result() == AuthResult::Rejected);
}

// 13. 误驱动的健壮性：begin() 调两次、以及 Done 之后再 step()，都不得崩溃或改写既
//     有裁决——一律返回 Done 并保持原裁决。
//     Argon2 开销：1 次校验。
void testMisuseIsInert() {
    // begin() 调两次：第二次直接 Done，裁决为保守默认 Rejected。
    {
        auto sasl = makeSasl("PLAIN", *g_store);
        CHECK(sasl != nullptr);
        if (sasl) {
            std::string challenge;
            CHECK(sasl->begin(challenge) == SaslStep::Challenge);
            CHECK(sasl->begin(challenge) == SaslStep::Done);
            CHECK(sasl->result() == AuthResult::Rejected);
        }
    }

    // Done（且成功）之后再 step()：仍是 Done，且不得把 Ok 改写掉。
    {
        auto sasl = makeSasl("PLAIN", *g_store);
        CHECK(sasl != nullptr);
        if (sasl) {
            std::string challenge;
            CHECK(sasl->step(plainPayload("", "alice", kAlicePassword),
                             challenge) == SaslStep::Done);
            CHECK(sasl->result() == AuthResult::Ok);

            CHECK(sasl->step("anything", challenge) == SaslStep::Done);
            CHECK(sasl->result() == AuthResult::Ok);
            CHECK(sasl->authenticatedUser() == "alice");
        }
    }
}

}  // namespace

int main() {
    if (!mail::auth::initCrypto()) {
        std::fprintf(stderr, "initCrypto failed\n");
        return 1;
    }
    if (!mail::auth::hashPassword(kAlicePassword, g_aliceHash) ||
        !mail::auth::hashPassword(kNulPassword, g_nulHash)) {
        std::fprintf(stderr, "hashPassword failed\n");
        return 1;
    }

    g_tempDir = makeTempDir();
    if (g_tempDir.empty()) {
        std::fprintf(stderr, "mkdtemp failed\n");
        return 1;
    }
    const std::string usersPath = g_tempDir + "/users";
    writeFile(usersPath,
              "alice:" + g_aliceHash + "\nnulluser:" + g_nulHash + "\n");

    auto opened = UserStore::open(usersPath);
    if (!opened.has_value()) {
        std::fprintf(stderr, "UserStore::open failed\n");
        removeRecursive(g_tempDir);
        return 1;
    }
    g_store = std::move(opened.value());

    testFactory();
    testPlainInitialResponse();
    testPlainWithMatchingAuthzid();
    testPlainNoInitialResponse();
    testPlainPasswordWithEmbeddedNul();
    testPlainMalformed();
    testPlainRejectsForeignAuthzid();
    testPlainWrongPassword();
    testPlainLongFields();
    testLoginNoInitialResponse();
    testLoginWithInitialResponse();
    testLoginEmptyUsername();
    testMisuseIsInert();

    g_store.reset();
    removeRecursive(g_tempDir);

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("all auth_sasl tests passed");
    return 0;
}
