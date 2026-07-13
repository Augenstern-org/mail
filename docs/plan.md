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
- ⬜ 下一步：**M0 收尾 + M1**——引入 vcpkg manifest；`src/common` 建库，实现 `net` 层 TCP + CRLF 行读取。

## 未决事项 / 待确认

| # | 事项 | 影响 |
|---|---|---|
| C | 客户端形态（CLI/Web/桌面） | 可延后到服务端稳定 |
| D | 口令哈希算法（Argon2/libsodium 首选） | M4 前确定 |

> 已解决：A 开发环境 → WSL2 + gcc/cmake/ninja/vcpkg；B `custom` → 已重命名 `client`。
