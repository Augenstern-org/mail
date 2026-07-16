// mail::app::MaildirSink 的落盘集成测试与 sanitizedLocalPart 纯函数测试。
//
// 在 mkdtemp 建立的独立临时根内演练 MaildirSink::deliver 的每收件人一份投递：单/多
// 收件人成功、非法收件人整体 451、部分失败不回收、根不可写、空消息、以及"绝不返回
// PermanentFail"的断言。结束时递归清理临时根。
//
// 不使用测试框架：一个极小的 CHECK 宏记录失败，任一检查失败时进程返回非零（沿用
// smtp_session_test.cpp / maildir_store_test.cpp 的模式）。

#include <cstddef>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mail/smtp/envelope.hpp"
#include "mail/smtp/message_sink.hpp"
#include "maildir_sink.hpp"

using mail::app::MaildirSink;
using mail::app::sanitizedLocalPart;
using mail::smtp::Envelope;
using mail::smtp::SinkStatus;

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

// mkdtemp 建立一个独立临时根，返回其绝对路径（失败返回空串）。
std::string makeTempRoot() {
    char tmpl[] = "/tmp/maildir_sink_test_XXXXXX";
    char* p = ::mkdtemp(tmpl);
    return p ? std::string(p) : std::string();
}

// 列出目录下的条目，跳过 "." 与 ".."。目录不可读或不存在时返回空。
std::vector<std::string> listDir(const std::string& path) {
    std::vector<std::string> out;
    DIR* d = ::opendir(path.c_str());
    if (!d) {
        return out;
    }
    while (dirent* e = ::readdir(d)) {
        std::string name = e->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        out.push_back(name);
    }
    ::closedir(d);
    return out;
}

// path 是否存在（任意类型）。
bool pathExists(const std::string& path) {
    struct stat st{};
    return ::lstat(path.c_str(), &st) == 0;
}

// 读取整个文件为字节串（二进制）。
std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// 递归删除 path（文件或目录）。先把目录权限放开到 0700 以便遍历/删除（测试可能把
// 某个目录设为只读）。尽力而为，忽略中途错误。
void removeRecursive(const std::string& path) {
    struct stat st{};
    if (::lstat(path.c_str(), &st) != 0) {
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        ::chmod(path.c_str(), 0700);
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

// 建立 mailroot 目录（sink 的 rootDir_）并返回其路径；失败返回空串。sink 内部的
// MaildirStore::open 只做单层 mkdir，故根目录须预先存在。
std::string makeMailroot(const std::string& base) {
    std::string root = base + "/mailroot";
    if (::mkdir(root.c_str(), 0700) != 0) {
        return std::string();
    }
    return root;
}

// 1. sanitizedLocalPart 纯函数：白名单、'@' 截断、首点、".."、非法字符。
void testSanitizePureFunction() {
    CHECK(sanitizedLocalPart("alice@example.com") == "alice");
    CHECK(sanitizedLocalPart("bob") == "bob");
    CHECK(sanitizedLocalPart("A.b_c+d-9@x") == "A.b_c+d-9");
    CHECK(sanitizedLocalPart("") == "");
    CHECK(sanitizedLocalPart("@example.com") == "");
    CHECK(sanitizedLocalPart(".alice@x") == "");
    CHECK(sanitizedLocalPart("a..b@x") == "");
    CHECK(sanitizedLocalPart("a/b@x") == "");
    CHECK(sanitizedLocalPart("a:b@x") == "");
    CHECK(sanitizedLocalPart("a b@x") == "");
}

// 2. 单收件人：Ok；<root>/alice/new/ 恰一文件内容==data；<root>/alice/tmp/ 空。
void testSingleRecipient() {
    std::string base = makeTempRoot();
    CHECK(!base.empty());
    std::string root = makeMailroot(base);
    CHECK(!root.empty());

    MaildirSink sink(root);
    Envelope env;
    env.sender = "from@host";
    env.recipients = {"alice@example.com"};
    std::string data = "From: a\r\n\r\nbody\r\n";

    CHECK(sink.deliver(env, data) == SinkStatus::Ok);

    std::vector<std::string> newFiles = listDir(root + "/alice/new");
    CHECK(newFiles.size() == 1);
    if (newFiles.size() == 1) {
        CHECK(slurp(root + "/alice/new/" + newFiles[0]) == data);
    }
    CHECK(listDir(root + "/alice/tmp").empty());

    removeRecursive(base);
}

// 3. 多收件人：3 个 → 各自 new/ 恰一份、内容一致、文件名互异。
void testMultipleRecipients() {
    std::string base = makeTempRoot();
    CHECK(!base.empty());
    std::string root = makeMailroot(base);
    CHECK(!root.empty());

    MaildirSink sink(root);
    Envelope env;
    env.recipients = {"alice@example.com", "bob@example.com", "carol@x"};
    std::string data = "Subject: hi\r\n\r\nhello everyone\r\n";

    CHECK(sink.deliver(env, data) == SinkStatus::Ok);

    std::set<std::string> names;
    for (const char* local : {"alice", "bob", "carol"}) {
        std::vector<std::string> newFiles = listDir(root + "/" + local + "/new");
        CHECK(newFiles.size() == 1);
        if (newFiles.size() == 1) {
            CHECK(slurp(std::string(root) + "/" + local + "/new/" +
                        newFiles[0]) == data);
            names.insert(newFiles[0]);
        }
    }
    // 三份文件名互异（唯一名含单调递增序号）。
    CHECK(names.size() == 3);

    removeRecursive(base);
}

// 4. 非法收件人整体 451：recipients={"..bad@x"} → TransientFail，不产生该收件人目录。
void testInvalidRecipientRejected() {
    std::string base = makeTempRoot();
    CHECK(!base.empty());
    std::string root = makeMailroot(base);
    CHECK(!root.empty());

    MaildirSink sink(root);
    Envelope env;
    env.recipients = {"..bad@x"};

    CHECK(sink.deliver(env, "x\r\n") == SinkStatus::TransientFail);
    // 非法 local part 在 sanitize 阶段即被拒，绝不触碰存储层，故 mailroot 保持空。
    CHECK(listDir(root).empty());

    removeRecursive(base);
}

// 5. 部分失败：recipients={合法, 非法}（合法在前）→ TransientFail；合法收件人拷贝
//    存在（不回收）。
void testPartialFailureKeepsCopies() {
    std::string base = makeTempRoot();
    CHECK(!base.empty());
    std::string root = makeMailroot(base);
    CHECK(!root.empty());

    MaildirSink sink(root);
    Envelope env;
    env.recipients = {"alice@example.com", ".illegal@x"};

    CHECK(sink.deliver(env, "body\r\n") == SinkStatus::TransientFail);
    // 合法收件人（在前）已成功落盘，部分失败不回收。
    CHECK(listDir(root + "/alice/new").size() == 1);
    // 非法收件人未产生目录。
    CHECK(!pathExists(root + "/.illegal"));

    removeRecursive(base);
}

// 6. 根不可写：把 mailroot 设为 0500，使邮箱创建（root 之下的 mkdir）失败 →
//    TransientFail；末尾恢复权限以便清理。
void testRootNotWritable() {
    std::string base = makeTempRoot();
    CHECK(!base.empty());
    std::string root = makeMailroot(base);
    CHECK(!root.empty());

    CHECK(::chmod(root.c_str(), 0500) == 0);

    MaildirSink sink(root);
    Envelope env;
    env.recipients = {"alice@example.com"};

    CHECK(sink.deliver(env, "body\r\n") == SinkStatus::TransientFail);

    CHECK(::chmod(root.c_str(), 0700) == 0);  // 恢复以便校验与清理
    // mkdir 被拒，未建出收件人目录。
    CHECK(listDir(root).empty());

    removeRecursive(base);
}

// 8. 空消息：data="" 投递 Ok，new/ 文件大小 0。
void testEmptyMessage() {
    std::string base = makeTempRoot();
    CHECK(!base.empty());
    std::string root = makeMailroot(base);
    CHECK(!root.empty());

    MaildirSink sink(root);
    Envelope env;
    env.recipients = {"alice@example.com"};

    CHECK(sink.deliver(env, "") == SinkStatus::Ok);

    std::vector<std::string> newFiles = listDir(root + "/alice/new");
    CHECK(newFiles.size() == 1);
    if (newFiles.size() == 1) {
        CHECK(slurp(root + "/alice/new/" + newFiles[0]).empty());
    }

    removeRecursive(base);
}

// 7. 绝无 PermanentFail：所有失败用例的返回码都恰为 TransientFail，绝不是
//    PermanentFail。这里把各失败触发条件再集中断言一遍。
void testNeverPermanentFail() {
    std::string base = makeTempRoot();
    CHECK(!base.empty());
    std::string root = makeMailroot(base);
    CHECK(!root.empty());

    MaildirSink sink(root);

    // 非法 local part。
    Envelope invalid;
    invalid.recipients = {"a..b@x"};
    SinkStatus s1 = sink.deliver(invalid, "x\r\n");
    CHECK(s1 == SinkStatus::TransientFail);
    CHECK(s1 != SinkStatus::PermanentFail);

    // 空 local part（'@' 开头）。
    Envelope emptyLocal;
    emptyLocal.recipients = {"@x"};
    SinkStatus s2 = sink.deliver(emptyLocal, "x\r\n");
    CHECK(s2 == SinkStatus::TransientFail);
    CHECK(s2 != SinkStatus::PermanentFail);

    // 存储层失败（根不可写）。
    CHECK(::chmod(root.c_str(), 0500) == 0);
    Envelope valid;
    valid.recipients = {"alice@x"};
    SinkStatus s3 = sink.deliver(valid, "x\r\n");
    CHECK(s3 == SinkStatus::TransientFail);
    CHECK(s3 != SinkStatus::PermanentFail);
    CHECK(::chmod(root.c_str(), 0700) == 0);

    removeRecursive(base);
}

}  // namespace

int main() {
    testSanitizePureFunction();
    testSingleRecipient();
    testMultipleRecipients();
    testInvalidRecipientRejected();
    testPartialFailureKeepsCopies();
    testRootNotWritable();
    testEmptyMessage();
    testNeverPermanentFail();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("all maildir_sink tests passed");
    return 0;
}
