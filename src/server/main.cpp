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
// 接中的工作线程会在进程退出时被拆除。工作线程各自持有一份 maildir root 拷贝（按值
// 传入），投递经 MaildirSink 落地，仅在失败时向 stderr 打印一行诊断（多线程并发写
// stderr，v1 容忍行间交错）。
//
// 工作线程在启动后共享一份**只读的 auth::UserStore**（经 shared_ptr<const UserStore>
// 按值传入，每线程各持一份所有权，故 main 返回后仍在运行的线程不会悬空）。该对象构造
// 后内容不可变，查询是 const 且线程安全；其内部的计数信号量把并发中的 Argon2 校验数
// 限制在上限内，防止未认证路径上每次 64 MiB 的哈希放大成内存耗尽。除此之外工作线程
// 仍不触碰任何共享状态。

#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

#include <sys/socket.h>

#include "mail/auth/password.hpp"
#include "mail/auth/user_store.hpp"
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
#include "user_store_verifier.hpp"

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

void serveConnection(mail::net::Connection conn, std::string root,
                     std::shared_ptr<const mail::auth::UserStore> users) {
    conn.setReceiveTimeout(std::chrono::seconds(
        static_cast<std::chrono::seconds::rep>(mail::kCommandTimeoutSeconds)));
    mail::net::LineReader reader(conn, mail::kMaxWireLineOctets);
    mail::app::MaildirSink sink(std::move(root));

    // verifier 与 config 二选一：有用户档则启用真实校验与 AUTH，否则退回原有的
    // AcceptAll + 默认 config（AUTH 关闭），完全向后兼容。两个 verifier 对象都是栈
    // 局部，其存活覆盖整个 Session。
    mail::smtp::AcceptAllVerifier acceptAll;
    mail::app::UserStoreVerifier userVerifier(users);
    mail::smtp::RecipientVerifier& verifier =
        users ? static_cast<mail::smtp::RecipientVerifier&>(userVerifier)
              : static_cast<mail::smtp::RecipientVerifier&>(acceptAll);

    mail::smtp::SessionConfig config;
    if (users) {
        config.allowPlaintextAuth = true;
        config.requireAuthForMail = true;
        config.userStore = users.get();
    }

    mail::smtp::Session session(reader, conn, sink, verifier, config);
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

    // 可选的用户档路径（argv[3]）。缺省则 AUTH 关闭、收件人一律 AcceptAll，与既往行为
    // 完全一致（不新增必填参数）。
    std::string usersPath;
    if (argc > 3) {
        usersPath = argv[3];
    }

    // 加密设施一次性初始化。哪怕本次未提供用户档，也在进入 accept 循环前确认库可用，
    // 库不可用即启动失败——绝不在未初始化状态下带着"看似能认证"的接口上线。
    if (!mail::auth::initCrypto()) {
        std::cerr << "crypto init failed\n";
        return 1;
    }

    // 提供了用户档则在启动时一次性载入。载入失败硬失败退出（与 bind 失败同风格），
    // 宁可启动即失败也不要带着残缺 / 缺失的账户表上线。载入后转成
    // shared_ptr<const UserStore> 分发给各工作线程。
    std::shared_ptr<const mail::auth::UserStore> users;
    if (!usersPath.empty()) {
        auto opened = mail::auth::UserStore::open(usersPath);
        if (!opened) {
            std::cerr << "load users file failed: " << usersPath
                      << " (errno " << opened.error_errno() << ")\n";
            return 1;
        }
        users = std::shared_ptr<const mail::auth::UserStore>(
            std::move(opened).value());
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
            std::thread(serveConnection, std::move(accepted).value(), root,
                        users)
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
