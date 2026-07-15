#include "mail/net/connection.hpp"

#include <cerrno>
#include <cstddef>

#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>

namespace mail::net {

IoStatus Connection::readSome(std::span<std::byte> buf, std::size_t& outBytes) {
    outBytes = 0;
    if (!fd_) {
        errno = EBADF;
        return IoStatus::Error;
    }
    if (buf.empty()) {
        return IoStatus::Ok;
    }

    for (;;) {
        ssize_t n = ::recv(fd_.get(), buf.data(), buf.size(), 0);
        if (n > 0) {
            outBytes = static_cast<std::size_t>(n);
            return IoStatus::Ok;
        }
        if (n == 0) {
            return IoStatus::Closed;  // 对端有序关闭
        }
        // n < 0
        if (errno == EINTR) {
            continue;  // 内部重试
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return recvTimeoutSet_ ? IoStatus::Timeout : IoStatus::WouldBlock;
        }
        return IoStatus::Error;
    }
}

IoStatus Connection::writeAll(std::span<const std::byte> data) {
    if (!fd_) {
        errno = EBADF;
        return IoStatus::Error;
    }

    std::size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = ::send(fd_.get(), data.data() + sent, data.size() - sent,
                           MSG_NOSIGNAL);
        if (n > 0) {
            sent += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;  // 内部重试
        }
        if (n < 0 && (errno == EPIPE || errno == ECONNRESET)) {
            return IoStatus::Closed;  // 对端已消失
        }
        // n == 0（阻塞流套接字下不应发生）或其他错误：报为 Error，保留 errno。
        return IoStatus::Error;
    }
    return IoStatus::Ok;
}

IoStatus Connection::writeAll(std::string_view data) {
    return writeAll(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(data.data()), data.size()));
}

bool Connection::setReceiveTimeout(std::chrono::seconds timeout) noexcept {
    if (!fd_) {
        return false;
    }
    struct timeval tv;
    tv.tv_sec = static_cast<time_t>(timeout.count());
    tv.tv_usec = 0;
    if (::setsockopt(fd_.get(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
        return false;
    }
    recvTimeoutSet_ = true;
    return true;
}

void Connection::shutdownWrite() noexcept {
    if (fd_) {
        ::shutdown(fd_.get(), SHUT_WR);
    }
}

}  // namespace mail::net
