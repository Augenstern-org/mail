# 实施计划

从零实现一套邮件系统：服务端（Linux）+ 客户端。协议自行实现，不依赖现有 MTA。
设计细节见 [architecture.md](./architecture.md)。

## 已确定的决策

| 决策 | 选择 |
|---|---|
| 项目目标 | 学习 / 原型：从零手写协议 |
| 协议范围 | SMTP + IMAP |
| 服务端语言 | C++（C++20） |
| 投递模型 | 本地投递、单域名、不对外中继 |
| 客户端形态 | 暂不定，先专注服务端；用现成客户端验证 |
| 存储格式 | Maildir |
| 并发模型 | v1 一连接一线程 → v2 epoll |
| 开发环境 | WSL2 + gcc + cmake + ninja + vcpkg |

## 里程碑

| 阶段 | 内容 | 验证方式 |
|---|---|---|
| **M0 骨架** | CMake 工程、目录结构、日志、配置文件加载 | 能编译能跑；`ctest` 冒烟通过 |
| **M1 TCP 服务层** | `net::Listener`/`Connection`/`LineReader`，按 CRLF 逐行读 | `telnet localhost` 连上并回显 |
| **M2 SMTP 接收** | `smtp::Session` 状态机：EHLO→MAIL FROM→RCPT TO→DATA→`.`→QUIT，解析信封 | telnet 手打完整 SMTP 会话投一封信 |
| **M3 Maildir 存储** | `store::Maildir`（tmp/new/cur + 唯一文件名） | 信落盘、格式合规 |
| **M4 用户 + 认证** | `auth::UserStore`/`Password`/`Sasl`，AUTH PLAIN/LOGIN | 认证失败拒发、成功放行；口令哈希存储 |
| **M5 IMAP 服务** | `imap::Session` 最小子集：CAPABILITY/LOGIN/LIST/SELECT/FETCH/STORE/EXPUNGE/LOGOUT | 收下 M2 投递的信；FETCH BODYSTRUCTURE 先用简化响应 |
| **M6 TLS** | `security::TlsContext`（OpenSSL），STARTTLS + 隐式 TLS | `openssl s_client` 握手成功 |
| **M7 MIME 精化** | `mime::Parser`/`Encoder` 完整多部分与编码 | 带附件的信正确解析/呈现 |
| **M8 真实客户端验收** | — | Thunderbird / mutt 直连跑通一整圈收发 |

> 备注：M5 的 IMAP 是最重的一块，`FETCH BODYSTRUCTURE` 牵涉 MIME 解析。策略是 M5 先用简化响应把"能收信"跑通，MIME 精细解析留到 M7 单独处理。

## 当前进度

- ✅ 项目骨架（CLion 生成）：`src/server`、`src/custom` 两个 Hello World 可执行。
- ✅ 顶层 CMake 修正：`cmake_minimum_required` 3.31 → 3.20（覆盖主流 Linux 发行版）；补 `CMAKE_CXX_STANDARD_REQUIRED`/`CXX_EXTENSIONS OFF`；加 `MAIL_BUILD_TESTS`/`MAIL_BUILD_BENCHMARKS` 选项与 `enable_testing()`。
- ✅ CTest 接入：`tests/` 冒烟测试（`server_smoke`/`custom_smoke`）。
- ✅ benchmarks 插座就位（默认关闭，无真实目标——待热点代码落地再填）。
- ✅ 文档：本计划 + 架构设计。
- ✅ `src/custom` 重命名为 `src/client`（`git mv` 保留历史，目标名/引用/冒烟测试同步更新）。
- ✅ 开发环境确定：WSL2 + gcc + cmake + ninja + vcpkg。
- ✅ **M0**：`vcpkg.json`（空依赖清单）+ `CMakePresets.json`（非强制，供后续依赖用）。
- ✅ **M1 传输层**（`src/common` → `mailcommon` 静态库）：
  - `FileDescriptor`（RAII，析构跨 `close` 保 errno）、`IoStatus`/`Result<T>`、`limits.hpp`（RFC 5321/7888 常量）。
  - `net::Listener`（`accept4(SOCK_CLOEXEC)` + `SO_REUSEADDR` + `TCP_NODELAY`，`stop()` 幂等）、`net::Connection : ByteSource`（EINTR 重试、partial-write 循环、`MSG_NOSIGNAL`）。
  - `net::LineReader`（单缓冲同时支撑 `readLine`/`readExactly`；`LineTooLong` 只丢超长行、保留后续帧，含跨包丢弃模式）。
  - `server` 改为 thread-per-connection echo（`telnet 127.0.0.1 2525` 可回显；`QUIT` 回 `bye` 并关闭）。
  - 测试：`line_reader`（单测，含 pipelining/LineTooLong 边界）、`net_integration`（loopback 收发闭环）、`client_smoke`。
  - 验证（WSL，g++ 13.3 + `-Wall -Wextra` 零警告）：3/3 CTest 通过 + 真 server 二进制活体 echo 通过。
- ✅ **M2 SMTP 接收**（`mail::smtp`，接口/实现分置 `include/mail/smtp/` + `src/smtp/`）：
  - `parseCommand` 纯函数解析层（tagged struct `Command`；`<>` 空反向路径、source route 剥离、quoted-string 透传、`SIZE=`/`BODY=` 参数，其余参数 555）+ `Reply`/`serialize`（RFC 5321 §4.2.1 多行响应）。
  - `smtp::Session` 四态状态机（Start/Greeted/MailGiven/RcptGiven；DATA 为 `collectData` 内部子循环）：EHLO 声明 PIPELINING/SIZE/8BITMIME；DATA 毒化排空（超长行/超限记 552，排空至 `.` 终止行统一回码）；裸 LF 分帧错误回 500 后关闭（SMTP smuggling 防御）；空闲 300s 超时回 421。
  - 扩展点：`MessageSink`（M3 Maildir 接入）、`RecipientVerifier`（M4 用户校验接入）；适配器一律放应用层，storage 不依赖 protocol。
  - net 层扩展：`ByteSink` 抽象（与 `ByteSource` 对偶，`Connection` 双实现）、`Connection::setReceiveTimeout`（SO_RCVTIMEO，EAGAIN→Timeout 门控）；`limits.hpp` 补 5 常量（路径 256 / 收件人 100 / 消息 10 MiB / DATA 线径 1001 / 超时 300s）。
  - `server` 由 echo 换为 `Session` 驱动（`DiscardSink` 打印信封摘要后丢弃）。
  - 测试：`smtp_command`（文法边界单测）、`smtp_session`（ChunkSource 喂整场会话，11 场景 + 附加）；5/5 CTest 通过 + 活体冒烟（telnet 级完整投信，dot-unstuffing 以字节数确证）。
- ⬜ 下一步：**M3 Maildir 存储**——`store::Maildir`（tmp/new/cur 三段式 + 唯一文件名），应用层写 `MaildirSink : smtp::MessageSink` 适配器接入投递链。

## 未决事项 / 待确认

| # | 事项 | 影响 |
|---|---|---|
| C | 客户端形态（CLI/Web/桌面） | 可延后到服务端稳定 |
| D | 口令哈希算法（Argon2/libsodium 首选） | M4 前确定 |

> 已解决：A 开发环境 → WSL2 + gcc/cmake/ninja/vcpkg；B `custom` → 已重命名 `client`。
