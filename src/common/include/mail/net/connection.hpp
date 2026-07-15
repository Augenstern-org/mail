#pragma once

#include <chrono>
#include <cstddef>
#include <span>
#include <string_view>
#include <utility>

#include "mail/file_descriptor.hpp"
#include "mail/io_status.hpp"
#include "mail/net/byte_sink.hpp"
#include "mail/net/byte_source.hpp"

namespace mail::net {

// 单个已接受（或已连接）的 TCP 流。仅移动；拥有自己的 fd。
//
// readSome/writeAll 在内部重试 EINTR，故在正常阻塞套接字使用下调用方不会从
// Connection 看到 IoStatus::Interrupted。
class Connection : public ByteSource, public ByteSink {
public:
    Connection() noexcept = default;
    explicit Connection(FileDescriptor fd) noexcept : fd_(std::move(fd)) {}

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    Connection(Connection&&) noexcept = default;
    Connection& operator=(Connection&&) noexcept = default;

    ~Connection() override = default;

    // ByteSource。把 recv()==0 映射为 Closed，其他错误映射为 Error（errno 为紧邻的
    // 调用方保留）。EAGAIN/EWOULDBLOCK 的映射取决于是否设置过接收超时：设过则视为
    // 超时映射为 Timeout，否则映射为 WouldBlock。
    IoStatus readSome(std::span<std::byte> buf, std::size_t& outBytes) override;

    // 写完全部字节，否则失败。以 MSG_NOSIGNAL 循环调用 ::send，处理部分写并重试
    // EINTR。仅当全部字节送出时返回 Ok；对端消失（EPIPE/ECONNRESET）时返回 Closed；
    // 其余情况返回 Error。span 重载为非虚；string_view 重载实现 ByteSink。
    IoStatus writeAll(std::span<const std::byte> data);
    IoStatus writeAll(std::string_view data) override;

    // 设置接收超时（SO_RCVTIMEO）。设置后阻塞 recv 在超时到期时以 EAGAIN 返回，进而
    // 被 readSome 映射为 IoStatus::Timeout。对空 Connection 返回 false；setsockopt
    // 成功时置内部标记并返回 true，失败返回 false。
    bool setReceiveTimeout(std::chrono::seconds timeout) noexcept;

    // 半关闭写方向（::shutdown SHUT_WR）。幂等；对空 Connection 为无操作。
    void shutdownWrite() noexcept;

    int fd() const noexcept { return fd_.get(); }
    explicit operator bool() const noexcept { return static_cast<bool>(fd_); }

private:
    FileDescriptor fd_;
    bool recvTimeoutSet_ = false;
};

}  // namespace mail::net
