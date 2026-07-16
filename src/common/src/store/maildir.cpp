#include "mail/store/maildir.hpp"

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <ctime>
#include <string>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include "mail/file_descriptor.hpp"
#include "mail/io_status.hpp"
#include "mail/result.hpp"
#include "mail/store/maildir_name.hpp"

// MaildirStore 写侧实现。所有时钟/pid/主机名的实取都收在本文件；头文件零系统调用。
// 崩溃安全序列参见头文件说明：tmp 独占创建 → 全量写 → fsync 文件 → 检查 close →
// rename 到 new/ → fsync new/ 目录项。

namespace mail::store {

namespace {

// per-process 投递序号（Maildir 唯一名的 Q 段）。fetch_add 保证同进程内单调递增，
// 是唯一名在同一微秒、同一 pid 下仍互异的主要来源。
std::atomic<unsigned long> g_deliverySequence{0};

// 以 0700 创建目录；已存在（EEXIST）视为成功，返回 0。其余失败返回捕获的 errno。
int makeDir0700(const std::string& path) {
    if (::mkdir(path.c_str(), 0700) == 0) {
        return 0;
    }
    if (errno == EEXIST) {
        return 0;
    }
    return errno;
}

// 取主机名并做 Maildir 转义；gethostname 失败或结果为空时退回 "localhost"。转义后的
// 结果再次经 escapeMaildirHost 是幂等的（'/' ':' 已消去），故可安全地在投递时复用。
std::string escapedHostname() {
    char buf[256];
    if (::gethostname(buf, sizeof(buf)) != 0) {
        return escapeMaildirHost("localhost");
    }
    // gethostname 在名字被截断时可能不写终止符，手工兜底一个 NUL。
    buf[sizeof(buf) - 1] = '\0';
    if (buf[0] == '\0') {
        return escapeMaildirHost("localhost");
    }
    return escapeMaildirHost(buf);
}

// 基于实取时钟/pid/序号生成一个 Maildir 唯一名。escapedHost 已转义并非空，直接注入
// 纯函数层（其内部再转义为幂等操作，且不会触发空 host 兜底）。
std::string makeUniqueName(const std::string& escapedHost) {
    timeval tv{};
    ::gettimeofday(&tv, nullptr);

    MaildirNameParts parts;
    parts.epochSeconds = static_cast<std::time_t>(tv.tv_sec);
    parts.microseconds = static_cast<long>(tv.tv_usec);
    parts.pid = static_cast<long>(::getpid());
    parts.sequence = g_deliverySequence.fetch_add(1, std::memory_order_relaxed);
    parts.hostname = escapedHost;
    return makeMaildirUniqueName(parts);
}

}  // namespace

MaildirStore::MaildirStore(std::string root, std::string escapedHost)
    : root_(std::move(root)), escapedHost_(std::move(escapedHost)) {}

Result<MaildirStore> MaildirStore::open(std::string root) {
    // root 及 tmp/new/cur 三个子目录逐层创建（0700，EEXIST 容忍）。任一步真失败即带
    // 捕获的 errno 返回。
    if (int err = makeDir0700(root); err != 0) {
        return Result<MaildirStore>::failure(IoStatus::Error, err);
    }
    for (const char* sub : {"/tmp", "/new", "/cur"}) {
        if (int err = makeDir0700(root + sub); err != 0) {
            return Result<MaildirStore>::failure(IoStatus::Error, err);
        }
    }

    return Result<MaildirStore>::success(
        MaildirStore(std::move(root), escapedHostname()));
}

Result<std::string> MaildirStore::deliverMessage(std::string_view data) {
    const std::string tmpDir = root_ + "/tmp/";
    const std::string newDir = root_ + "/new/";

    // 1) 在 tmp/ 内以 O_EXCL 独占创建唯一名文件；EEXIST 表示名字碰撞，重取时钟/序号
    //    换名重试，上限 16 次。其余 open 失败立即带 errno 返回（尚无 tmp 文件可清理）。
    constexpr int kMaxNameRetries = 16;
    std::string name;
    std::string tmpPath;
    FileDescriptor file;
    int openErrno = 0;
    for (int attempt = 0; attempt < kMaxNameRetries; ++attempt) {
        name = makeUniqueName(escapedHost_);
        tmpPath = tmpDir + name;
        int raw = ::open(tmpPath.c_str(),
                         O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
        if (raw >= 0) {
            file = FileDescriptor(raw);
            break;
        }
        openErrno = errno;
        if (openErrno == EEXIST) {
            continue;  // 换名重试
        }
        return Result<std::string>::failure(IoStatus::Error, openErrno);
    }
    if (!file) {
        // 16 次仍碰撞：以最后一次的 EEXIST 返回。
        return Result<std::string>::failure(IoStatus::Error, openErrno);
    }

    // 自此 tmp 文件已落地：后续任一步失败都要先记下首个失败 errno（由调用点在 unlink
    // 之前捕获并传入），再 unlink 自己的 tmp 文件（unlink 自身失败不得覆盖该 errno），
    // 最后带首个失败点的 errno 返回。
    auto failCleanup = [&](int firstErrno) -> Result<std::string> {
        ::unlink(tmpPath.c_str());  // 失败无妨：不覆盖已捕获的 firstErrno
        return Result<std::string>::failure(IoStatus::Error, firstErrno);
    };

    // 2) 全量写入，处理 partial write 与 EINTR，直至写完（10MiB 单循环不分块）。
    std::size_t written = 0;
    while (written < data.size()) {
        ssize_t n = ::write(file.get(), data.data() + written,
                            data.size() - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return failCleanup(errno);
        }
        written += static_cast<std::size_t>(n);
    }

    // 3) fsync 使文件数据落盘。
    if (::fsync(file.get()) != 0) {
        return failCleanup(errno);
    }

    // 4) 显式 close 并检查返回值：close 可上报延迟写回错误，失败视为投递失败。取出裸
    //    fd 手工 close，使 FileDescriptor 不再持有（避免析构重复 close）。
    int fd = file.release();
    if (::close(fd) != 0) {
        return failCleanup(errno);
    }

    // 5) rename tmp/ → new/：Maildir 投递的原子提交点。
    const std::string newPath = newDir + name;
    if (::rename(tmpPath.c_str(), newPath.c_str()) != 0) {
        return failCleanup(errno);
    }

    // 6) fsync new/ 目录项，使 rename 产生的目录项本身落盘（崩溃后文件不丢名）。
    int rawDir = ::open(newDir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (rawDir < 0) {
        return failCleanup(errno);
    }
    FileDescriptor dir(rawDir);
    if (::fsync(dir.get()) != 0) {
        return failCleanup(errno);
    }

    return Result<std::string>::success(name);
}

}  // namespace mail::store
