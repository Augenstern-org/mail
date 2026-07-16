// 一连接一线程的极简 SMTP 服务器。
//
// 每个被接受的连接由一个工作线程独占，线程内构造 SMTP 会话状态机（mail::smtp::
// Session）驱动完整事务序列：EHLO/HELO → MAIL → RCPT → DATA → QUIT。收件人一律
// 接受（AcceptAllVerifier），收到的消息经 MaildirSink 真正投递到本地 Maildir：每个
// 收件人一份独立拷贝，落入 <root>/<local-part>/new/。落盘失败映射为 451。SIGINT 干净
// 地停止 accept 循环。
//
// 并发（v1，见 docs/architecture.md 第 5 节）：一连接一线程。工作线程被 detach——
// 每个线程按移动语义拥有自己的 Connection，在 QUIT、对端关闭、超时或出错时运行至结
// 束。detach 是可接受的 v1 简化：SIGINT 时 accept 循环停止、main 返回；任何仍在连
// 接中的工作线程会在进程退出时被拆除。工作线程在启动后不触碰共享状态，故关闭时不会
// 崩溃。工作线程各自持有一份 maildir root 拷贝（按值传入），投递经 MaildirSink 落地，
// 仅在失败时向 stderr 打印一行诊断（多线程并发写 stderr，v1 容忍行间交错）。

#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

#include <sys/socket.h>

#include "mail/io_status.hpp"
#include "mail/limits.hpp"
#include "mail/net/byte_sink.hpp"
#include "mail/net/connection.hpp"
#include "mail/net/line_reader.hpp"
#include "mail/net/listener.hpp"
#include "mail/smtp/message_sink.hpp"
#include "mail/smtp/recipient_verifier.hpp"
#include "mail/smtp/session.hpp"

#include "maildir_sink.hpp"

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

void serveConnection(mail::net::Connection conn, std::string root) {
    conn.setReceiveTimeout(std::chrono::seconds(
        static_cast<std::chrono::seconds::rep>(mail::kCommandTimeoutSeconds)));
    mail::net::LineReader reader(conn, mail::kMaxDataWireLineOctets);
    mail::app::MaildirSink sink(std::move(root));
    mail::smtp::AcceptAllVerifier verifier;
    mail::smtp::Session session(reader, conn, sink, verifier, {});
    session.run();
    conn.shutdownWrite();
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

    // Maildir 根目录：自由路径字符串，不做预校验；MaildirSink 在投递时按需 open，失
    // 败即映射为 451。默认 "./mailroot"。
    std::string root = "./mailroot";
    if (argc > 2) {
        root = argv[2];
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
            std::thread(serveConnection, std::move(accepted).value(), root)
                .detach();
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
