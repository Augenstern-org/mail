#include "mail/net/listener.hpp"

#include <cerrno>
#include <cstring>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace mail::net {

namespace {

// 把受支持的数字主机形式解析为网络字节序的 IPv4 地址。成功返回 true。空串、"*"
// 与 "0.0.0.0" 映射为 INADDR_ANY。
bool parseHost(std::string_view host, in_addr& out) {
    if (host.empty() || host == "*" || host == "0.0.0.0") {
        out.s_addr = htonl(INADDR_ANY);
        return true;
    }
    // inet_pton 需要以 NUL 结尾的字符串；host 是 string_view。
    char buf[INET_ADDRSTRLEN];
    if (host.size() >= sizeof(buf)) {
        return false;
    }
    std::memcpy(buf, host.data(), host.size());
    buf[host.size()] = '\0';
    return ::inet_pton(AF_INET, buf, &out) == 1;
}

}  // namespace

Result<Listener> Listener::bind(std::string_view host, std::uint16_t port,
                                int backlog) {
    in_addr addr{};
    if (!parseHost(host, addr)) {
        return Result<Listener>::failure(IoStatus::Error, EINVAL);
    }

    int raw = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (raw < 0) {
        return Result<Listener>::failure(IoStatus::Error, errno);
    }
    FileDescriptor fd(raw);

    int one = 1;
    if (::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) {
        return Result<Listener>::failure(IoStatus::Error, errno);
    }

    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr = addr;

    if (::bind(fd.get(), reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)) < 0) {
        return Result<Listener>::failure(IoStatus::Error, errno);
    }

    if (::listen(fd.get(), backlog) < 0) {
        return Result<Listener>::failure(IoStatus::Error, errno);
    }

    return Result<Listener>::success(Listener(std::move(fd)));
}

Result<Connection> Listener::accept() {
    if (!fd_) {
        return Result<Connection>::failure(IoStatus::Error, EBADF);
    }

    for (;;) {
        int raw = ::accept4(fd_.get(), nullptr, nullptr, SOCK_CLOEXEC);
        if (raw >= 0) {
            FileDescriptor conn(raw);
            // 关闭 Nagle，避免小的协议响应被延迟。
            int one = 1;
            ::setsockopt(conn.get(), IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            return Result<Connection>::success(Connection(std::move(conn)));
        }
        if (errno == EINTR) {
            continue;  // 内部重试
        }
        if (errno == EINVAL) {
            // 之前的 stop() 对监听套接字调用了 ::shutdown，使 accept4 以 EINVAL
            // 失败。将其视为有序关闭，让 accept 循环得以退出而非空转。
            return Result<Connection>::failure(IoStatus::Closed, errno);
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return Result<Connection>::failure(IoStatus::WouldBlock, errno);
        }
        // ECONNABORTED 等单连接级失败会被上报；是否继续 accept 由调用方决定。
        return Result<Connection>::failure(IoStatus::Error, errno);
    }
}

void Listener::stop() noexcept {
    if (fd_) {
        // 唤醒任何阻塞在 accept4() 的线程；若已关闭则无害。
        ::shutdown(fd_.get(), SHUT_RDWR);
    }
}

}  // namespace mail::net
