#pragma once

#include <cerrno>
#include <unistd.h>

// POSIX 文件描述符的仅移动 RAII 包装，头文件实现。
//
// 保证：
//   - 绝不重复关闭：移动之后，源对象持有 -1。
//   - 析构在 ::close 前后保留 errno，使清理路径上的关闭不会覆盖调用方仍然关心的
//     errno。
//   - 负的 fd 视为"空"，绝不传给 ::close。

namespace mail {

class FileDescriptor {
public:
    FileDescriptor() noexcept = default;
    explicit FileDescriptor(int fd) noexcept : fd_(fd) {}

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    FileDescriptor(FileDescriptor&& other) noexcept : fd_(other.fd_) {
        other.fd_ = -1;
    }

    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
        if (this != &other) {
            reset(other.fd_);
            other.fd_ = -1;
        }
        return *this;
    }

    ~FileDescriptor() { reset(); }

    int get() const noexcept { return fd_; }

    explicit operator bool() const noexcept { return fd_ >= 0; }

    // 放弃所有权但不关闭；返回原始 fd。
    int release() noexcept {
        int fd = fd_;
        fd_ = -1;
        return fd;
    }

    // 关闭当前持有的 fd（若有）并接管 `fd`（默认：无）。在内部 ::close 前后保留
    // errno。
    void reset(int fd = -1) noexcept {
        if (fd_ >= 0) {
            int saved = errno;
            ::close(fd_);
            errno = saved;
        }
        fd_ = fd;
    }

private:
    int fd_ = -1;
};

}  // namespace mail
