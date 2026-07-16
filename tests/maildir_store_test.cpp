// mail::store::MaildirStore 的落盘集成测试。在 mkdtemp 建立的独立临时根内演练
// open() 与 deliverMessage() 的完整崩溃安全序列，逐字节核对落盘内容，并覆盖目录创建、
// 幂等、失败路径、大消息、唯一性等场景。结束时递归清理临时根。
//
// 不使用测试框架：一个极小的 CHECK 宏记录失败，任一检查失败时进程返回非零（沿用
// smtp_session_test.cpp / maildir_name_test.cpp 的模式）。

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

#include "mail/io_status.hpp"
#include "mail/store/maildir.hpp"

using mail::IoStatus;
using mail::store::MaildirStore;

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
    char tmpl[] = "/tmp/maildir_store_test_XXXXXX";
    char* p = ::mkdtemp(tmpl);
    return p ? std::string(p) : std::string();
}

// 列出目录下的条目，跳过 "." 与 ".."。目录不可读时返回空。
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

// path 是否为目录且权限恰为 0700。
bool isDirMode0700(const std::string& path) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) {
        return false;
    }
    if (!S_ISDIR(st.st_mode)) {
        return false;
    }
    return (st.st_mode & 0777) == 0700;
}

// 读取整个文件为字节串（二进制）。
std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// 递归删除 path（文件或目录）。先把目录权限放开到 0700 以便遍历/删除（测试可能把
// 某个子目录设为只读）。尽力而为，忽略中途错误。
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

// 1. open 建目录：新路径 open() 后 root 及 tmp/new/cur 均存在、为目录、权限 0700。
void testOpenBuildsDirs() {
    std::string base = makeTempRoot();
    CHECK(!base.empty());
    std::string root = base + "/mbox";

    auto r = MaildirStore::open(root);
    CHECK(r.has_value());
    if (r.has_value()) {
        CHECK(isDirMode0700(root));
        CHECK(isDirMode0700(root + "/tmp"));
        CHECK(isDirMode0700(root + "/new"));
        CHECK(isDirMode0700(root + "/cur"));
        CHECK(r.value().root() == root);
    }

    removeRecursive(base);
}

// 2. open 幂等：对同一 root 再次 open() 仍成功。
void testOpenIdempotent() {
    std::string base = makeTempRoot();
    CHECK(!base.empty());
    std::string root = base + "/mbox";

    auto r1 = MaildirStore::open(root);
    CHECK(r1.has_value());
    auto r2 = MaildirStore::open(root);
    CHECK(r2.has_value());
    if (r2.has_value()) {
        CHECK(isDirMode0700(root + "/tmp"));
    }

    removeRecursive(base);
}

// 3. open 失败：root 落在一个普通文件之下（file/sub），mkdir 必失败（ENOTDIR）。
void testOpenFailsUnderFile() {
    std::string base = makeTempRoot();
    CHECK(!base.empty());
    std::string filePath = base + "/afile";
    int fd = ::open(filePath.c_str(), O_CREAT | O_WRONLY, 0600);
    CHECK(fd >= 0);
    if (fd >= 0) {
        ::close(fd);
    }

    auto r = MaildirStore::open(filePath + "/sub");
    CHECK(!r.has_value());
    if (!r.has_value()) {
        CHECK(r.error() == IoStatus::Error);
        CHECK(r.error_errno() != 0);
    }

    removeRecursive(base);
}

// 4. 基本投递：内容逐字节等于 data；tmp/ 空；new/ 恰一文件；返回的 basename 与实际
//    文件名一致。
void testBasicDelivery() {
    std::string base = makeTempRoot();
    CHECK(!base.empty());
    std::string root = base + "/mbox";

    auto r = MaildirStore::open(root);
    CHECK(r.has_value());
    if (r.has_value()) {
        MaildirStore store = std::move(r).value();
        std::string data = "From: a\r\n\r\nbody\r\n";
        auto d = store.deliverMessage(data);
        CHECK(d.has_value());
        if (d.has_value()) {
            CHECK(listDir(root + "/tmp").empty());
            std::vector<std::string> newFiles = listDir(root + "/new");
            CHECK(newFiles.size() == 1);
            if (newFiles.size() == 1) {
                CHECK(newFiles[0] == d.value());
                CHECK(slurp(root + "/new/" + newFiles[0]) == data);
            }
        }
    }

    removeRecursive(base);
}

// 5. 10MiB 投递：含 CRLF 的 10*1024*1024 字节，逐字节比对落盘结果。
void testLargeDelivery() {
    std::string base = makeTempRoot();
    CHECK(!base.empty());
    std::string root = base + "/mbox";

    auto r = MaildirStore::open(root);
    CHECK(r.has_value());
    if (r.has_value()) {
        MaildirStore store = std::move(r).value();
        const std::size_t target = 10 * 1024 * 1024;
        std::string data;
        data.reserve(target + 16);
        const std::string unit = "maildir large delivery line\r\n";
        while (data.size() < target) {
            data += unit;
        }
        data.resize(target);  // 恰好 10MiB（可能在某行中截断，仍含大量 CRLF）

        auto d = store.deliverMessage(data);
        CHECK(d.has_value());
        if (d.has_value()) {
            CHECK(listDir(root + "/tmp").empty());
            std::string got = slurp(root + "/new/" + d.value());
            CHECK(got.size() == target);
            CHECK(got == data);
        }
    }

    removeRecursive(base);
}

// 6. 两次投递：名互异，new/ 恰两文件，各自内容正确。
void testTwoDeliveries() {
    std::string base = makeTempRoot();
    CHECK(!base.empty());
    std::string root = base + "/mbox";

    auto r = MaildirStore::open(root);
    CHECK(r.has_value());
    if (r.has_value()) {
        MaildirStore store = std::move(r).value();
        std::string data1 = "message one body\r\n";
        std::string data2 = "the second message\r\ndone\r\n";
        auto d1 = store.deliverMessage(data1);
        auto d2 = store.deliverMessage(data2);
        CHECK(d1.has_value());
        CHECK(d2.has_value());
        if (d1.has_value() && d2.has_value()) {
            CHECK(d1.value() != d2.value());
            CHECK(listDir(root + "/new").size() == 2);
            CHECK(slurp(root + "/new/" + d1.value()) == data1);
            CHECK(slurp(root + "/new/" + d2.value()) == data2);
        }
    }

    removeRecursive(base);
}

// 7. tmp/ 不可写：去掉 tmp/ 子目录的写权限后投递 → failure，errno==EACCES，tmp/ 无
//    残留；末尾恢复 0700 以便清理。
//
//    偏差说明：简报原文为「chmod(邮箱目录, 0500)」，但把邮箱根目录设为 0500 并不能
//    阻止在其 tmp/ 子目录内创建文件——O_CREAT 只校验 tmp/ 自身的写权限，rename 只校验
//    tmp/ 与 new/ 的写权限，均与根目录权限无关。故此处对 tmp/ 子目录去写权限，才能真正
//    触发投递失败（EACCES）并验证 tmp/ 无残留，与验收断言一致。
void testTmpNotWritable() {
    std::string base = makeTempRoot();
    CHECK(!base.empty());
    std::string root = base + "/mbox";

    auto r = MaildirStore::open(root);
    CHECK(r.has_value());
    if (r.has_value()) {
        MaildirStore store = std::move(r).value();
        std::string tmpDir = root + "/tmp";
        CHECK(::chmod(tmpDir.c_str(), 0500) == 0);

        auto d = store.deliverMessage("hello\r\n");
        CHECK(!d.has_value());
        if (!d.has_value()) {
            CHECK(d.error() == IoStatus::Error);
            CHECK(d.error_errno() == EACCES);
        }

        CHECK(::chmod(tmpDir.c_str(), 0700) == 0);  // 恢复以便校验与清理
        CHECK(listDir(tmpDir).empty());             // 失败路径不留半截 tmp 文件
    }

    removeRecursive(base);
}

// 8(a). 快速连续投递 100 封：全部成功，名字集合无重复，new/ 恰 100 文件，tmp/ 无残留。
//       黑盒下无法预置唯一名碰撞（时钟/pid/seq 均在实现内部实取），故以高频投递验证
//       唯一性；EEXIST 换名重试分支（上限 16、超限返回 Error+EEXIST）由实现评审覆盖。
void testManyDeliveriesUnique() {
    std::string base = makeTempRoot();
    CHECK(!base.empty());
    std::string root = base + "/mbox";

    auto r = MaildirStore::open(root);
    CHECK(r.has_value());
    if (r.has_value()) {
        MaildirStore store = std::move(r).value();
        std::set<std::string> names;
        for (int i = 0; i < 100; ++i) {
            auto d = store.deliverMessage("msg " + std::to_string(i) + "\r\n");
            CHECK(d.has_value());
            if (d.has_value()) {
                names.insert(d.value());
            }
        }
        CHECK(names.size() == 100);
        CHECK(listDir(root + "/new").size() == 100);
        CHECK(listDir(root + "/tmp").empty());
    }

    removeRecursive(base);
}

}  // namespace

int main() {
    testOpenBuildsDirs();
    testOpenIdempotent();
    testOpenFailsUnderFile();
    testBasicDelivery();
    testLargeDelivery();
    testTwoDeliveries();
    testTmpNotWritable();
    testManyDeliveriesUnique();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("all maildir_store tests passed");
    return 0;
}
