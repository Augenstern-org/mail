# 邮件服务器架构设计

本文件描述一个从零实现的邮件系统的服务端架构。服务端运行于 Linux，使用 C++20 编写；协议栈（SMTP、IMAP、MIME、Maildir）全部自行实现，不依赖现有 MTA/MDA（如 Postfix、Dovecot）。

## 1. 目标与范围

### 目标

- 自行实现 SMTP（发信/投递）与 IMAP（收信/邮箱管理）两套协议。
- 支持多用户、认证、TLS。
- 服务端与客户端共享同一套协议与消息处理代码。

### 范围边界（v1 明确不做）

| 不做 | 原因 |
|---|---|
| 对外中继 / 联邦投递 | 需真实域名、DNS MX/PTR、面对 25 端口封锁与投递声誉问题 |
| SPF / DKIM / DMARC | 仅联网互发时才需要；本地投递无关 |
| 多域名 | v1 单域名，简化路由 |
| 反垃圾 / 反病毒 | 超出学习范围 |

**投递模型**：单域名、本地投递。`RCPT TO` 仅接受本机已注册用户，不向外部服务器转发。这一约束把实现量压缩到协议本身。

## 2. 总体架构（分层）

自底向上分为六层，上层依赖下层，禁止反向依赖：

```
┌─────────────────────────────────────────────┐
│  Application    server 可执行 / custom 客户端  │
├─────────────────────────────────────────────┤
│  Protocol       SMTP 会话状态机 / IMAP 会话状态机 │
├─────────────────────────────────────────────┤
│  Message        RFC 5322 消息模型 / MIME 解析编码 │
├─────────────────────────────────────────────┤
│  Storage        Maildir 抽象 / 邮箱 / 标志位     │
├─────────────────────────────────────────────┤
│  Auth & Security 用户库 / 口令哈希 / SASL / TLS   │
├─────────────────────────────────────────────┤
│  Platform/Net   TCP 监听 / 连接 / CRLF 行读取器   │
└─────────────────────────────────────────────┘
```

除最上层的两个可执行目标外，其余各层沉淀在共享静态库 `mailcommon`（`src/common/`），由服务端与客户端共同链接。

## 3. 模块划分

| 层 | 模块 | 职责 |
|---|---|---|
| Platform/Net | `net::Listener` | `socket`/`bind`/`listen`/`accept` |
| Platform/Net | `net::Connection` | 单连接读写、缓冲 |
| Platform/Net | `net::LineReader` | 按 `CRLF` 分帧的缓冲读取器（协议热点） |
| Auth & Security | `auth::UserStore` | 用户加载、查询 |
| Auth & Security | `auth::Password` | 口令哈希与校验 |
| Auth & Security | `auth::Sasl` | SASL PLAIN / LOGIN 机制 |
| Auth & Security | `security::TlsContext` | OpenSSL 上下文、STARTTLS/隐式 TLS 握手 |
| Storage | `store::MaildirStore` | `tmp`/`new`/`cur` 三目录、唯一文件名生成 |
| Storage | `store::Mailbox` | 邮箱枚举、消息列表、标志位 |
| Message | `mime::Message` | RFC 5322 头/体模型 |
| Message | `mime::Parser` / `mime::Encoder` | MIME 多部分、编码（base64/quoted-printable） |
| Protocol | `smtp::Session` | SMTP 命令状态机 |
| Protocol | `imap::Session` | IMAP 带 tag 命令状态机 |
| Application | `server` | 组合各模块，监听 SMTP+IMAP 端口 |
| Application | `client` | 客户端（形态待定，先做 CLI 验证） |

## 4. 关键数据流

**发信 → 落盘（SMTP）**

```
客户端 --EHLO/AUTH--> smtp::Session
       --MAIL FROM/RCPT TO/DATA--> 校验信封（RCPT 仅本机用户）
       --<CRLF>.<CRLF>--> mime 校验 --> store::MaildirStore 写入收件人 new/
```

**收信（IMAP）**

```
客户端 --LOGIN--> imap::Session（校验 auth::UserStore）
       --SELECT INBOX--> store::Mailbox 映射 Maildir
       --FETCH--> 读取消息、解析头/结构
       --STORE flags / EXPUNGE--> 更新 cur/ 文件名标志位
```

## 5. 并发模型

- **v1：一连接一线程**。最易写对，先把协议逻辑跑通。
- **v2：`epoll` 事件循环**。协议状态机与 IO 解耦后再切换，不改协议层代码。

## 6. 存储格式

采用 **Maildir**（非 mbox）：

- 每封信一个独立文件，写入无需全局锁，避免 mbox 的并发写锁问题。
- `tmp/` 写入中、`new/` 新到未读、`cur/` 已被客户端看到；标志位（`\Seen`、`\Flagged` 等）编码在 `cur/` 文件名后缀。
- 唯一文件名：`<epoch>.M<usec>P<pid>Q<seq>.<escaped-host>` 惯例（现代 Maildir，host 转义 `/`→`\057`、`:`→`\072`）。

## 7. 认证与安全

- **口令**：不存明文。使用 Argon2 或 bcrypt；若暂不引入依赖，退回 salted SHA-256（仅学习阶段过渡）。
- **SASL**：SMTP 提交与 IMAP 均支持 PLAIN / LOGIN；明文机制**必须**在 TLS 之上才允许。
- **TLS**：OpenSSL 实现。SMTP 支持 STARTTLS（587）与隐式 TLS（465）；IMAP 支持 STARTTLS（143）与隐式 TLS（993）。

### 依赖管理

第三方依赖统一走 **vcpkg**（manifest 模式，`vcpkg.json`），CMake 通过 toolchain 文件集成。预期依赖：`openssl`（TLS）、口令哈希库（`libsodium`/Argon2，见未决项 D）、以及后续的测试/基准框架（如 `benchmark`）。构建工具链为 gcc + cmake + ninja。

## 8. 目录结构（目标）

```
mail/
├── CMakeLists.txt          # 顶层：选项、enable_testing、子目录
├── docs/
│   ├── plan.md             # 实施计划与里程碑
│   └── architecture.md     # 本文件
├── src/
│   ├── common/             # mailcommon 静态库（各协议/存储/网络模块）
│   ├── server/             # server 可执行
│   └── client/             # client 可执行（形态待定）
├── tests/                  # CTest 单测与冒烟测试
└── benchmarks/             # 微基准（默认关闭）
```

> `src/common/`（mailcommon 静态库）自 M1 起已落地，其下 net / smtp / store / auth 等模块随里程碑推进逐步补齐。

## 9. 测试策略

- **单元测试（CTest）**：解析器、编解码、Maildir 文件名逻辑等纯函数是首要覆盖对象。
- **冒烟测试**：`tests/` 现有 `server_smoke` / `custom_smoke` 直接运行可执行并校验输出，作为构建健全性检查。
- **集成验证**：以现成客户端（telnet、swaks、Thunderbird、mutt）直连服务端跑真实收发。

## 10. 未决事项

| # | 事项 | 说明 |
|---|---|---|
| C | 客户端形态 | CLI / Web / 桌面，暂缓；先用现成客户端验证服务端。 |

> 已解决：开发环境统一为 WSL2 + gcc + cmake + ninja + vcpkg（CLion 工具链已切至 WSL）；客户端目标已由 `custom` 重命名为 `client`；D 口令哈希算法已于 M4 定为 libsodium Argon2id（INTERACTIVE 档），经 apt `libsodium-dev` + pkg-config 引入。
