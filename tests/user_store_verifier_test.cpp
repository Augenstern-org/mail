// mail::app::UserStoreVerifier 的单元测试。在 mkdtemp 建立的临时用户档内载入一个
// 只含 alice 的 UserStore，演练 RCPT 阶段的收件人裁决：存在即 Accept、缺失即
// RejectPermanent、local part 不合格（首点 / ".." / 白名单外字符 / 空）即
// RejectPermanent，并断言任何输入都绝不产生 RejectTemporary，以及"Accept ⇒ 投递侧
// sanitizedLocalPart 必非空"的一致性不变量（RCPT-250 蕴含可投递）。结束时清理临时档。
//
// 不使用测试框架：一个极小的 CHECK 宏记录失败，任一检查失败时进程返回非零（沿用
// auth_user_store_test.cpp / maildir_sink_test.cpp 的模式）。

#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>

#include <cerrno>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mail/auth/password.hpp"
#include "mail/auth/user_store.hpp"
#include "mail/smtp/recipient_verifier.hpp"

#include "maildir_sink.hpp"
#include "user_store_verifier.hpp"

using mail::app::sanitizedLocalPart;
using mail::app::UserStoreVerifier;
using mail::auth::UserStore;
using mail::smtp::RcptDecision;

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

// mkdtemp 建立一个独立临时目录，返回其绝对路径（失败返回空串）。
std::string makeTempDir() {
    char tmpl[] = "/tmp/user_store_verifier_test_XXXXXX";
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

// 一个方便断言的辅助：verify 后同时校验"Accept ⇒ sanitizedLocalPart 非空"（C4 不变
// 量，RCPT-250 蕴含投递侧能取到合法 local part），且裁决绝不是 RejectTemporary。
void expectDecision(UserStoreVerifier& v, std::string_view mailbox,
                    RcptDecision want) {
    RcptDecision got = v.verify(mailbox);
    CHECK(got == want);
    // 任何输入都不得触发临时拒绝：用户档在启动时一次性载入内存，查询不会瞬时失败。
    CHECK(got != RcptDecision::RejectTemporary);
    // C4：凡回 Accept 的收件人，投递侧的同一个 sanitizedLocalPart 必得到非空 local
    // part，故不会出现"RCPT 250、投递 451"的错配。
    if (got == RcptDecision::Accept) {
        CHECK(!sanitizedLocalPart(mailbox).empty());
    }
}

}  // namespace

int main() {
    if (!mail::auth::initCrypto()) {
        std::fprintf(stderr, "initCrypto failed\n");
        return 1;
    }

    std::string aliceHash;
    if (!mail::auth::hashPassword("alice-secret", aliceHash)) {
        std::fprintf(stderr, "hashPassword failed\n");
        return 1;
    }

    std::string dir = makeTempDir();
    CHECK(!dir.empty());
    std::string path = dir + "/users";
    writeFile(path, "alice:" + aliceHash + "\n");

    auto opened = UserStore::open(path);
    CHECK(opened.has_value());
    if (opened.has_value()) {
        auto store = std::shared_ptr<const UserStore>(std::move(opened).value());
        UserStoreVerifier verifier(store);

        // 存在的用户：带域名与裸 local part 都 Accept。
        expectDecision(verifier, "alice@example.com", RcptDecision::Accept);
        expectDecision(verifier, "alice", RcptDecision::Accept);

        // 存在但大小写不同：UserStore 大小写不敏感，仍 Accept。
        expectDecision(verifier, "ALICE@example.com", RcptDecision::Accept);

        // 用户档中无此用户：550。
        expectDecision(verifier, "mallory@example.com",
                       RcptDecision::RejectPermanent);

        // local part 含 ".."：550（sanitize 阶段即拒，绝不落到存在性查询）。
        expectDecision(verifier, "../etc/passwd@x",
                       RcptDecision::RejectPermanent);

        // local part 以 '.' 开头：550。
        expectDecision(verifier, ".alice@x", RcptDecision::RejectPermanent);

        // local part 含白名单外字符（空格 / 斜杠）：550。
        expectDecision(verifier, "al ice@x", RcptDecision::RejectPermanent);
        expectDecision(verifier, "al/ice@x", RcptDecision::RejectPermanent);

        // 空 mailbox：550。
        expectDecision(verifier, "", RcptDecision::RejectPermanent);

        // '@' 开头（空 local part）：550。
        expectDecision(verifier, "@example.com",
                       RcptDecision::RejectPermanent);
    }

    removeRecursive(dir);

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("all user_store_verifier tests passed");
    return 0;
}
