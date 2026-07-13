// 演练 mail net 层的极简 一连接一线程 回显服务器。
//
// 这是 M1 的手工验证目标：`telnet 127.0.0.1 2525` 连上后，每输入一整行（CRLF 结
// 尾）都会被回显。"QUIT"（不区分大小写）结束会话；超长行会被拒绝。SIGINT 干净地
// 停止 accept 循环。
//
// 并发（v1，见 docs/architecture.md 第 5 节）：一连接一线程。工作线程被 detach——
// 每个线程按移动语义拥有自己的 Connection，在对端关闭、QUIT 或出错时运行至结束。
// detach 是可接受的 v1 简化：SIGINT 时 accept 循环停止、main 返回；任何仍在连接中
// 的工作线程会在进程退出时被拆除。工作线程在启动后不触碰共享状态，故关闭时不会崩溃。

#include <cctype>
#include <charconv>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

#include <sys/socket.h>
#include <unistd.h>

#include "mail/io_status.hpp"
#include "mail/limits.hpp"
#include "mail/net/connection.hpp"
#include "mail/net/line_reader.hpp"
#include "mail/net/listener.hpp"

namespace {

// 在 accept 循环之前由 main 设置一次；由 SIGINT 处理函数读取。用 sig_atomic_t 存原
// 始 fd 让处理函数保持异步信号安全（仅对一个整数做 ::shutdown 系统调用）。
volatile std::sig_atomic_t g_listen_fd = -1;

void handleSigint(int) {
    int fd = g_listen_fd;
    if (fd >= 0) {
        ::shutdown(fd, SHUT_RDWR);  // 使 accept4() 以 EINVAL 唤醒
    }
}

bool equalsIgnoreCase(const std::string& s, const char* lit) {
    std::size_t i = 0;
    for (; i < s.size() && lit[i] != '\0'; ++i) {
        unsigned char a = static_cast<unsigned char>(s[i]);
        unsigned char b = static_cast<unsigned char>(lit[i]);
        if (std::tolower(a) != std::tolower(b)) {
            return false;
        }
    }
    return i == s.size() && lit[i] == '\0';
}

void serveConnection(mail::net::Connection conn) {
    mail::net::LineReader reader(conn, mail::kDefaultMaxLineOctets);
    std::string line;
    for (;;) {
        mail::IoStatus st = reader.readLine(line);
        if (st == mail::IoStatus::Ok) {
            if (equalsIgnoreCase(line, "QUIT")) {
                conn.writeAll("bye\r\n");
                conn.shutdownWrite();
                return;
            }
            // 把收到的行原样回显，重新以 CRLF 结尾。
            if (conn.writeAll(line) != mail::IoStatus::Ok ||
                conn.writeAll("\r\n") != mail::IoStatus::Ok) {
                return;
            }
            continue;
        }
        if (st == mail::IoStatus::LineTooLong) {
            conn.writeAll("line too long\r\n");
            conn.shutdownWrite();
            return;
        }
        // Closed、Error（含裸 CR/LF 分帧）或任何其他状态：结束。
        return;
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::uint16_t port = 2525;
    if (argc > 1) {
        // 严格解析端口：不接受前导空白、正负号或尾部残余（如 "2525abc"）。
        std::string_view arg(argv[1]);
        unsigned long value = 0;
        auto [ptr, ec] =
            std::from_chars(arg.data(), arg.data() + arg.size(), value);
        if (ec != std::errc{} || ptr != arg.data() + arg.size() || value < 1 ||
            value > 65535) {
            std::cerr << "invalid port: " << argv[1] << '\n';
            return 1;
        }
        port = static_cast<std::uint16_t>(value);
    }

    // 绝不让写向已消失对端的操作杀死进程；writeAll 已使用 MSG_NOSIGNAL，这里是双保险。
    std::signal(SIGPIPE, SIG_IGN);

    auto bound = mail::net::Listener::bind("127.0.0.1", port);
    if (!bound) {
        std::cerr << "bind failed on 127.0.0.1:" << port
                  << " (errno " << bound.error_errno() << ")\n";
        return 1;
    }
    mail::net::Listener listener = std::move(bound).value();

    g_listen_fd = listener.fd();
    std::signal(SIGINT, handleSigint);

    std::cout << "server listening on 127.0.0.1:" << port << std::endl;

    for (;;) {
        auto accepted = listener.accept();
        if (accepted) {
            std::thread(serveConnection, std::move(accepted).value()).detach();
            continue;
        }
        if (accepted.error() == mail::IoStatus::Closed) {
            break;  // stop() 已被调用（SIGINT）；干净退出
        }
        // 瞬时的单连接 accept 错误：继续服务。
        continue;
    }

    return 0;
}
