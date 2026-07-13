#pragma once

#include <cstdint>
#include <string_view>
#include <utility>

#include "mail/file_descriptor.hpp"
#include "mail/net/connection.hpp"
#include "mail/result.hpp"

namespace mail::net {

// 已绑定并处于监听状态的 TCP 套接字。仅移动；拥有自己的 fd。
class Listener {
public:
    Listener() noexcept = default;

    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;

    Listener(Listener&&) noexcept = default;
    Listener& operator=(Listener&&) noexcept = default;

    ~Listener() = default;

    // 创建套接字，设置 SO_REUSEADDR，绑定到 host:port，并开始 listen。
    //
    // host 用 inet_pton 解析 "127.0.0.1" / "0.0.0.0" 等 IPv4 数字形式。空串、"*"
    // 与 "0.0.0.0" 均表示 INADDR_ANY。主机名不做解析（本层无 DNS）。
    //
    // 失败时返回 Result::failure，携带相应的 IoStatus 与 errno。
    static Result<Listener> bind(std::string_view host, std::uint16_t port,
                                 int backlog = 128);

    // 接受一个连接。内部重试 EINTR。若监听器已被停止（见 stop()），::shutdown 之后
    // accept4 会以 EINVAL 失败，此时返回 Result::failure(IoStatus::Closed)，使
    // accept 循环得以干净退出而非空转。
    Result<Connection> accept();

    // 通过半关闭监听套接字唤醒阻塞在 accept() 的线程。幂等，且可从信号触发路径 /
    // 另一线程安全调用。对空 Listener 为无操作。
    void stop() noexcept;

    int fd() const noexcept { return fd_.get(); }
    explicit operator bool() const noexcept { return static_cast<bool>(fd_); }

private:
    explicit Listener(FileDescriptor fd) noexcept : fd_(std::move(fd)) {}

    FileDescriptor fd_;
};

}  // namespace mail::net
