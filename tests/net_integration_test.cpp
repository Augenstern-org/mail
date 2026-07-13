// net 层的集成测试：在 127.0.0.1 上绑定一个真实 Listener，由后台线程接受一个连接并
// 回显一行，然后用一个普通客户端套接字连上、发送 "ping\r\n"，并断言读回 "ping\r\n"。
//
// 仅限 POSIX，这没问题：整个测试套件面向 Linux。不使用测试框架。

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "mail/io_status.hpp"
#include "mail/net/connection.hpp"
#include "mail/net/line_reader.hpp"
#include "mail/net/listener.hpp"

using mail::IoStatus;
using mail::net::Connection;
using mail::net::LineReader;
using mail::net::Listener;

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

// 接受一个连接，读取一行，以 CRLF 回显，然后返回。
void echoOnce(Listener& listener) {
    auto accepted = listener.accept();
    if (!accepted) {
        return;  // 监听器已停止或出错；无可服务
    }
    Connection conn = std::move(accepted).value();
    LineReader reader(conn);
    std::string line;
    if (reader.readLine(line) == IoStatus::Ok) {
        conn.writeAll(line);
        conn.writeAll("\r\n");
        conn.shutdownWrite();
    }
}

}  // namespace

int main() {
    // 尝试一小段高位端口，避免某个端口被占用时测试无谓失败。
    Listener listener;
    std::uint16_t port = 0;
    for (std::uint16_t p = 39000; p <= 39050; ++p) {
        auto bound = Listener::bind("127.0.0.1", p);
        if (bound) {
            listener = std::move(bound).value();
            port = p;
            break;
        }
    }
    if (!listener) {
        std::fprintf(stderr, "could not bind any test port in 39000-39050\n");
        return 1;
    }

    std::thread server(echoOnce, std::ref(listener));

    // 客户端：普通 POSIX 套接字。
    int cfd = ::socket(AF_INET, SOCK_STREAM, 0);
    CHECK(cfd >= 0);

    // 给 recv 设上限，使服务器异常时测试失败而非挂起。
    timeval tv{};
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    ::setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    CHECK(::connect(cfd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == 0);

    const char request[] = "ping\r\n";
    ssize_t sent = ::send(cfd, request, sizeof(request) - 1, 0);
    CHECK(sent == static_cast<ssize_t>(sizeof(request) - 1));

    // 精确读回 "ping\r\n"（6 字节）。
    char buf[16];
    std::size_t total = 0;
    const std::size_t want = 6;
    while (total < want) {
        ssize_t n = ::recv(cfd, buf + total, sizeof(buf) - total, 0);
        if (n <= 0) {
            break;  // EOF、超时或错误
        }
        total += static_cast<std::size_t>(n);
    }
    CHECK(total == want);
    CHECK(std::memcmp(buf, "ping\r\n", want) == 0);

    ::close(cfd);

    // 服务器线程已完成回显（上面的 recv 只有在它回显后才解除阻塞），因此它已结束；
    // stop() 只是针对提前出错路径的防御性调用。
    listener.stop();
    server.join();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("net integration test passed");
    return 0;
}
