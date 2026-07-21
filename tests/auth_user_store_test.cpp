// mail::auth::UserStore 的落盘测试。在 mkdtemp 建立的独立临时目录内写出各式用户档，
// 演练解析规则（首个冒号切分、注释与空行、大小写不敏感、格式非法即整体失败）、认证
// 裁决（正确 / 口令错 / 用户不存在），以及并发闸门下的多线程校验。结束时清理临时目录。
//
// 不使用测试框架：一个极小的 CHECK 宏记录失败，任一检查失败时进程返回非零（沿用
// maildir_store_test.cpp / smtp_session_test.cpp 的模式）。
//
// 开销约束：每次 Argon2 操作约 100 ms 且分配 64 MiB。两份口令哈希在 main 里只算一次，
// 供全部用例复用；并发用例的线程数封顶 8。

#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <cerrno>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mail/auth/password.hpp"
#include "mail/auth/user_store.hpp"
#include "mail/io_status.hpp"

using mail::IoStatus;
using mail::auth::AuthResult;
using mail::auth::UserStore;

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
std::string g_bobHash;

const std::string kAlicePassword = "alice-secret";
const std::string kBobPassword = "bob-secret";

// mkdtemp 建立一个独立临时目录，返回其绝对路径（失败返回空串）。
std::string makeTempDir() {
    char tmpl[] = "/tmp/auth_user_store_test_XXXXXX";
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

// 1. 两用户档：open 成功、size()==2、大小写不敏感的存在性、三种认证裁决。
//    Argon2 开销：3 次校验（正确、口令错、不存在时对假串的等价校验）+ 1 次假串生成。
void testTwoUsers() {
    std::string dir = makeTempDir();
    CHECK(!dir.empty());
    std::string path = dir + "/users";
    writeFile(path, "alice:" + g_aliceHash + "\nbob:" + g_bobHash + "\n");

    auto r = UserStore::open(path);
    CHECK(r.has_value());
    if (r.has_value()) {
        const UserStore& store = *r.value();
        CHECK(store.size() == 2);

        CHECK(store.hasUser("alice"));
        CHECK(store.hasUser("ALICE"));
        CHECK(store.hasUser("Bob"));
        CHECK(!store.hasUser("mallory"));

        CHECK(store.authenticate("alice", kAlicePassword) == AuthResult::Ok);
        CHECK(store.authenticate("alice", "wrong") == AuthResult::Rejected);
        CHECK(store.authenticate("mallory", kAlicePassword) ==
              AuthResult::Rejected);
    }

    removeRecursive(dir);
}

// 2. 注释行与空行被忽略；用户名以小写为键（档案里写大写也照样能查到）。
//    Argon2 开销：0 次。
void testCommentsAndBlankLines() {
    std::string dir = makeTempDir();
    CHECK(!dir.empty());
    std::string path = dir + "/users";
    writeFile(path,
              "# 这是注释\n"
              "\n"
              "   \t \n"
              "   # 前导空白后的注释\n"
              "ALICE:" + g_aliceHash + "\n"
              "\n"
              "bob:" + g_bobHash + "\n");

    auto r = UserStore::open(path);
    CHECK(r.has_value());
    if (r.has_value()) {
        const UserStore& store = *r.value();
        CHECK(store.size() == 2);
        CHECK(store.hasUser("alice"));
        CHECK(store.hasUser("bob"));
    }

    removeRecursive(dir);
}

// 3. 格式非法的行让整个 open 失败：无冒号 / 用户名为空 / crypt 串为空。
//    失败时 error()==IoStatus::Error 且 errno 为 0（区别于真正的 I/O 失败）。
//    Argon2 开销：0 次。
void testMalformedLinesFailOpen() {
    const std::string cases[] = {
        "alice:" + g_aliceHash + "\nnocolonline\n",  // 无冒号
        "alice:" + g_aliceHash + "\n:" + g_bobHash + "\n",  // 用户名为空
        "alice:" + g_aliceHash + "\nbob:\n",  // crypt 串为空
    };

    for (const std::string& content : cases) {
        std::string dir = makeTempDir();
        CHECK(!dir.empty());
        std::string path = dir + "/users";
        writeFile(path, content);

        auto r = UserStore::open(path);
        CHECK(!r.has_value());
        if (!r.has_value()) {
            CHECK(r.error() == IoStatus::Error);
            CHECK(r.error_errno() == 0);
        }

        removeRecursive(dir);
    }
}

// 4. 路径不存在：open 失败，带真实 errno（ENOENT），据此可与"档案格式非法"区分。
//    Argon2 开销：0 次。
void testMissingFile() {
    std::string dir = makeTempDir();
    CHECK(!dir.empty());

    auto r = UserStore::open(dir + "/does_not_exist");
    CHECK(!r.has_value());
    if (!r.has_value()) {
        CHECK(r.error() == IoStatus::Error);
        CHECK(r.error_errno() != 0);
        CHECK(r.error_errno() == ENOENT);
    }

    removeRecursive(dir);
}

// 5. 冒号切分的安全性：Argon2 crypt 串本身就含 '$' 与 ','，若按最后一个冒号切、或
//    按 '$' / ',' 再切一刀，串就会被截坏而校验失败。认证成功即证明整串原样存下。
//    Argon2 开销：1 次校验。
void testCryptValueWithDollarAndCommaIntact() {
    CHECK(g_aliceHash.find('$') != std::string::npos);
    CHECK(g_aliceHash.find(',') != std::string::npos);

    std::string dir = makeTempDir();
    CHECK(!dir.empty());
    std::string path = dir + "/users";
    writeFile(path, "alice:" + g_aliceHash + "\n");

    auto r = UserStore::open(path);
    CHECK(r.has_value());
    if (r.has_value()) {
        CHECK(r.value()->authenticate("alice", kAlicePassword) ==
              AuthResult::Ok);
    }

    removeRecursive(dir);
}

// 6. 并发冒烟：8 个线程同时 authenticate 同一实例，全部返回 Ok。校验并发闸门在
//    kMaxConcurrentVerifications 之外的线程上正确排队并在每条退出路径释放。
//    线程数封顶 8 以控制墙钟时间（闸门为 4，故约两批）。
void testConcurrentAuthenticate() {
    std::string dir = makeTempDir();
    CHECK(!dir.empty());
    std::string path = dir + "/users";
    writeFile(path, "alice:" + g_aliceHash + "\nbob:" + g_bobHash + "\n");

    auto r = UserStore::open(path);
    CHECK(r.has_value());
    if (r.has_value()) {
        const UserStore& store = *r.value();
        constexpr int kThreads = 8;
        std::vector<AuthResult> results(kThreads, AuthResult::Unavailable);
        std::vector<std::thread> workers;
        workers.reserve(kThreads);
        for (int i = 0; i < kThreads; ++i) {
            workers.emplace_back([&store, &results, i] {
                results[static_cast<std::size_t>(i)] =
                    (i % 2 == 0)
                        ? store.authenticate("alice", kAlicePassword)
                        : store.authenticate("bob", kBobPassword);
            });
        }
        for (std::thread& t : workers) {
            t.join();
        }
        for (AuthResult got : results) {
            CHECK(got == AuthResult::Ok);
        }
    }

    removeRecursive(dir);
}

}  // namespace

int main() {
    if (!mail::auth::initCrypto()) {
        std::fprintf(stderr, "initCrypto failed\n");
        return 1;
    }
    if (!mail::auth::hashPassword(kAlicePassword, g_aliceHash) ||
        !mail::auth::hashPassword(kBobPassword, g_bobHash)) {
        std::fprintf(stderr, "hashPassword failed\n");
        return 1;
    }

    testTwoUsers();
    testCommentsAndBlankLines();
    testMalformedLinesFailOpen();
    testMissingFile();
    testCryptValueWithDollarAndCommaIntact();
    testConcurrentAuthenticate();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("all auth_user_store tests passed");
    return 0;
}
