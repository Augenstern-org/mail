// mail::smtp::Session 的整场会话单元测试。完全经由内存伪实现驱动：脚本化分块的
// ByteSource（沿用 line_reader_test.cpp 的 ChunkSource）喂入命令字节，内存 ByteSink
// 收集线路上的应答字节，MessageSink / RecipientVerifier 用可配置伪实现替换。不涉及
// 套接字，也不改动任何产品代码。
//
// 不使用测试框架：一个极小的 CHECK 宏记录失败，任一检查失败时进程返回非零（沿用
// line_reader_test.cpp 的模式）。

#include <cstddef>
#include <cstdio>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mail/auth/password.hpp"
#include "mail/auth/user_store.hpp"
#include "mail/base64.hpp"
#include "mail/io_status.hpp"
#include "mail/limits.hpp"
#include "mail/net/byte_sink.hpp"
#include "mail/net/byte_source.hpp"
#include "mail/net/line_reader.hpp"
#include "mail/smtp/command.hpp"
#include "mail/smtp/envelope.hpp"
#include "mail/smtp/message_sink.hpp"
#include "mail/smtp/recipient_verifier.hpp"
#include "mail/smtp/reply.hpp"
#include "mail/smtp/session.hpp"

using mail::IoStatus;
using mail::auth::UserStore;
using mail::net::ByteSink;
using mail::net::ByteSource;
using mail::net::LineReader;
using mail::smtp::AcceptAllVerifier;
using mail::smtp::Envelope;
using mail::smtp::MessageSink;
using mail::smtp::RcptDecision;
using mail::smtp::RecipientVerifier;
using mail::smtp::Session;
using mail::smtp::SessionConfig;
using mail::smtp::SinkStatus;

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

// 每次 readSome 调用最多投喂一个预设分块，使分块边界对读取器可见。耗尽后返回 Closed。
// 照抄自 line_reader_test.cpp 的 ChunkSource。
class ChunkSource : public ByteSource {
public:
    explicit ChunkSource(std::vector<std::string> chunks)
        : chunks_(std::move(chunks)) {}

    IoStatus readSome(std::span<std::byte> buf, std::size_t& outBytes) override {
        outBytes = 0;
        if (chunk_ >= chunks_.size()) {
            return IoStatus::Closed;
        }
        const std::string& cur = chunks_[chunk_];
        std::size_t remaining = cur.size() - off_;
        std::size_t n = remaining < buf.size() ? remaining : buf.size();
        for (std::size_t i = 0; i < n; ++i) {
            buf[i] = static_cast<std::byte>(
                static_cast<unsigned char>(cur[off_ + i]));
        }
        off_ += n;
        if (off_ == cur.size()) {
            ++chunk_;
            off_ = 0;
        }
        outBytes = n;
        return IoStatus::Ok;
    }

private:
    std::vector<std::string> chunks_;
    std::size_t chunk_ = 0;
    std::size_t off_ = 0;
};

// 内存 ByteSink：writeAll 追加进内部缓冲并总返回 Ok。测试据此按 "\r\n" 切行核对回码。
class StringSink : public ByteSink {
public:
    IoStatus writeAll(std::string_view data) override {
        buffer_.append(data.data(), data.size());
        return IoStatus::Ok;
    }

    const std::string& bytes() const { return buffer_; }

private:
    std::string buffer_;
};

// 记录每次 deliver 的信封拷贝与消息原文；返回值可配置（默认 Ok）。
class CollectingSink : public MessageSink {
public:
    struct Delivery {
        Envelope envelope;
        std::string data;
    };

    SinkStatus deliver(const Envelope& envelope, std::string_view data) override {
        deliveries.push_back({envelope, std::string(data)});
        return status;
    }

    std::vector<Delivery> deliveries;
    SinkStatus status = SinkStatus::Ok;
};

// 固定裁决的收件人校验器，用于测 550/450 路径。
class FixedVerifier : public RecipientVerifier {
public:
    explicit FixedVerifier(RcptDecision decision) : decision_(decision) {}

    RcptDecision verify(std::string_view /*mailbox*/) override {
        return decision_;
    }

private:
    RcptDecision decision_;
};

// 按 "\r\n" 切分线路字节为逐行文本（丢弃末尾不完整片段）。
std::vector<std::string> splitCrlf(const std::string& wire) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (true) {
        std::size_t pos = wire.find("\r\n", start);
        if (pos == std::string::npos) {
            break;
        }
        out.push_back(wire.substr(start, pos - start));
        start = pos + 2;
    }
    return out;
}

bool startsWith(const std::string& s, std::string_view prefix) {
    return s.size() >= prefix.size() &&
           std::string_view(s).substr(0, prefix.size()) == prefix;
}

// 统计以给定前缀开头的行数。
std::size_t countPrefix(const std::vector<std::string>& lines,
                        std::string_view prefix) {
    std::size_t n = 0;
    for (const std::string& l : lines) {
        if (startsWith(l, prefix)) {
            ++n;
        }
    }
    return n;
}

// ---- 场景 1：正常投信全流程，含 dot-stuffing 还原 ----
void testHappyPath() {
    std::string script =
        "EHLO client.example.org\r\n"
        "MAIL FROM:<alice@example.com>\r\n"
        "RCPT TO:<bob@example.com>\r\n"
        "DATA\r\n"
        "Subject: hi\r\n"
        "..leading\r\n"   // dot-stuffing：应还原为 ".leading"
        "body\r\n"
        ".\r\n"
        "QUIT\r\n";
    ChunkSource src({script});
    LineReader reader(src, mail::kMaxWireLineOctets);
    StringSink wire;
    CollectingSink sink;
    AcceptAllVerifier v;
    Session s(reader, wire, sink, v, SessionConfig{});
    CHECK(s.run() == IoStatus::Ok);  // 以 QUIT 的写状态结束

    std::vector<std::string> lines = splitCrlf(wire.bytes());
    // 0: 220 问候（单行，含域名）
    CHECK(lines.size() >= 10);
    CHECK(startsWith(lines[0], "220 "));
    CHECK(lines[0].find("localhost") != std::string::npos);
    // 1..4: EHLO 四行
    CHECK(lines[1] == "250-localhost");
    CHECK(lines[2] == "250-PIPELINING");
    CHECK(startsWith(lines[3], "250-SIZE "));
    CHECK(lines[4] == "250 8BITMIME");
    // 5: MAIL 250；6: RCPT 250；7: DATA 354；8: 投递 250；9: QUIT 221
    CHECK(startsWith(lines[5], "250 "));
    CHECK(startsWith(lines[6], "250 "));
    CHECK(startsWith(lines[7], "354 "));
    CHECK(startsWith(lines[8], "250 "));
    CHECK(startsWith(lines[9], "221 "));

    // 信封与正文核对：去 dot-stuffing、CRLF 结尾、不含终止行。
    CHECK(sink.deliveries.size() == 1);
    CHECK(sink.deliveries[0].envelope.sender == "alice@example.com");
    CHECK(sink.deliveries[0].envelope.recipients.size() == 1);
    CHECK(sink.deliveries[0].envelope.recipients[0] == "bob@example.com");
    CHECK(sink.deliveries[0].data == "Subject: hi\r\n.leading\r\nbody\r\n");
}

// ---- 场景 2：PIPELINING，一个分块塞多条命令，逐条回码 ----
void testPipelining() {
    // 单个 chunk 内 EHLO/MAIL/RCPT/RSET/NOOP/QUIT 连续到达。
    std::string script =
        "EHLO x\r\nMAIL FROM:<a@b>\r\nRCPT TO:<c@d>\r\nRSET\r\nNOOP\r\nQUIT\r\n";
    ChunkSource src({script});
    LineReader reader(src, mail::kMaxWireLineOctets);
    StringSink wire;
    CollectingSink sink;
    AcceptAllVerifier v;
    Session s(reader, wire, sink, v, SessionConfig{});
    s.run();

    std::vector<std::string> lines = splitCrlf(wire.bytes());
    // 220 + EHLO(4) + MAIL + RCPT + RSET + NOOP + QUIT = 10 行
    CHECK(lines.size() == 10);
    CHECK(startsWith(lines[0], "220 "));
    CHECK(startsWith(lines[5], "250 "));  // MAIL
    CHECK(startsWith(lines[6], "250 "));  // RCPT
    CHECK(startsWith(lines[7], "250 "));  // RSET
    CHECK(startsWith(lines[8], "250 "));  // NOOP
    CHECK(startsWith(lines[9], "221 "));  // QUIT
}

// ---- 场景 3：超长命令（剥 CRLF 后 >510）→ 500，会话可继续 ----
void testOverlongCommandContinues() {
    std::string longCmd(600, 'A');  // 600 字节，>510 但 <2046（以 Ok 到达）
    ChunkSource src({"EHLO x\r\n", longCmd + "\r\n", "NOOP\r\n", "QUIT\r\n"});
    LineReader reader(src, mail::kMaxWireLineOctets);
    StringSink wire;
    CollectingSink sink;
    AcceptAllVerifier v;
    Session s(reader, wire, sink, v, SessionConfig{});
    s.run();

    std::vector<std::string> lines = splitCrlf(wire.bytes());
    // 超长命令行回 500，其后 NOOP 仍能正常回 250，证明会话未终止。
    CHECK(countPrefix(lines, "500") == 1);
    CHECK(startsWith(lines.back(), "221 "));  // 末行为 QUIT 的 221
    // 220 + EHLO(4) + 500 + NOOP(250) + QUIT(221) = 8 行
    CHECK(lines.size() == 8);
    CHECK(startsWith(lines[5], "500"));
    CHECK(startsWith(lines[6], "250 "));
}

// ---- 场景 4：DATA 内超长行（>1001 数据行上限）→ 毒化 552，终止行仍存活 ----
void testDataOverlongLinePoisoned() {
    // 1005 + CRLF > kMaxDataWireLineOctets（1001）；分帧上限已放宽到 2048，故此行以
    // Ok 到达，由 collectData 的行长自查毒化。
    std::string longData(1005, 'X');
    std::string script =
        "EHLO x\r\n"
        "MAIL FROM:<a@b>\r\n"
        "RCPT TO:<c@d>\r\n"
        "DATA\r\n"
        "normal\r\n" +
        longData + "\r\n"  // 超长数据行
        ".\r\n"            // 独立终止行：必须存活并被识别
        "QUIT\r\n";
    ChunkSource src({script});
    LineReader reader(src, mail::kMaxWireLineOctets);
    StringSink wire;
    CollectingSink sink;
    AcceptAllVerifier v;
    Session s(reader, wire, sink, v, SessionConfig{});
    s.run();

    std::vector<std::string> lines = splitCrlf(wire.bytes());
    // 毒化后对终止行回 552；关键不变量：终止行被识别（否则会挂起或吞掉），QUIT 得到 221。
    CHECK(countPrefix(lines, "552") == 1);
    CHECK(startsWith(lines.back(), "221 "));
    // 毒化路径不得调用 deliver。
    CHECK(sink.deliveries.empty());
}

// ---- 场景 4b：新分帧窗口内的超长数据行（1500 字节，以 Ok 到达）→ 仍毒化 552 ----
void testDataLineAtNewFramingBoundary() {
    // 1500 + CRLF 小于分帧上限 2048，故 LineReader 返回 Ok；但 1502 > 1001，必须由
    // collectData 的行长自查毒化。这是"放宽分帧不得架空文本行上限"的回归守卫。
    std::string longData(1500, 'X');
    std::string script =
        "EHLO x\r\n"
        "MAIL FROM:<a@b>\r\n"
        "RCPT TO:<c@d>\r\n"
        "DATA\r\n"
        "normal\r\n" +
        longData + "\r\n"
        ".\r\n"
        "QUIT\r\n";
    ChunkSource src({script});
    LineReader reader(src, mail::kMaxWireLineOctets);
    StringSink wire;
    CollectingSink sink;
    AcceptAllVerifier v;
    Session s(reader, wire, sink, v, SessionConfig{});
    s.run();

    std::vector<std::string> lines = splitCrlf(wire.bytes());
    CHECK(countPrefix(lines, "552") == 1);
    CHECK(startsWith(lines.back(), "221 "));
    CHECK(sink.deliveries.empty());
}

// ---- 场景 4c：恰好贴住数据行上限（999 + CRLF == 1001）→ 正常接受并投递 ----
void testDataLineJustUnderLimit() {
    // 999 + 2 == kMaxDataWireLineOctets，不满足 > 1001，故不毒化。
    std::string data(999, 'Y');
    std::string script =
        "EHLO x\r\n"
        "MAIL FROM:<a@b>\r\n"
        "RCPT TO:<c@d>\r\n"
        "DATA\r\n" +
        data + "\r\n"
        ".\r\n"
        "QUIT\r\n";
    ChunkSource src({script});
    LineReader reader(src, mail::kMaxWireLineOctets);
    StringSink wire;
    CollectingSink sink;
    AcceptAllVerifier v;
    Session s(reader, wire, sink, v, SessionConfig{});
    s.run();

    std::vector<std::string> lines = splitCrlf(wire.bytes());
    CHECK(countPrefix(lines, "552") == 0);
    CHECK(sink.deliveries.size() == 1);
    CHECK(sink.deliveries[0].data == data + "\r\n");
}

// ---- 场景 5：消息累积超 maxMessageOctets → 552 ----
void testMessageTooLargePoisoned() {
    SessionConfig cfg;
    cfg.maxMessageOctets = 10;  // 小上限，避免构造巨大缓冲
    std::string script =
        "EHLO x\r\n"
        "MAIL FROM:<a@b>\r\n"
        "RCPT TO:<c@d>\r\n"
        "DATA\r\n"
        "hello\r\n"  // 5 + CRLF = 7 <= 10，累积
        "world\r\n"  // 7 + 5 + 2 = 14 > 10，毒化
        ".\r\n"
        "QUIT\r\n";
    ChunkSource src({script});
    LineReader reader(src, mail::kMaxWireLineOctets);
    StringSink wire;
    CollectingSink sink;
    AcceptAllVerifier v;
    Session s(reader, wire, sink, v, cfg);
    s.run();

    std::vector<std::string> lines = splitCrlf(wire.bytes());
    CHECK(countPrefix(lines, "552") == 1);
    CHECK(sink.deliveries.empty());
}

// ---- 场景 6：MAIL ... SIZE= 超限 → 提前 552 ----
void testMailSizeExceedsLimit() {
    SessionConfig cfg;
    cfg.maxMessageOctets = 1000;
    std::string script =
        "EHLO x\r\n"
        "MAIL FROM:<a@b> SIZE=5000\r\n"  // 5000 > 1000
        "RCPT TO:<c@d>\r\n"              // 事务未开启，应 503
        "QUIT\r\n";
    ChunkSource src({script});
    LineReader reader(src, mail::kMaxWireLineOctets);
    StringSink wire;
    CollectingSink sink;
    AcceptAllVerifier v;
    Session s(reader, wire, sink, v, cfg);
    s.run();

    std::vector<std::string> lines = splitCrlf(wire.bytes());
    // 220 + EHLO(4) + MAIL(552) + RCPT(503) + QUIT(221)
    CHECK(startsWith(lines[5], "552"));
    CHECK(startsWith(lines[6], "503"));  // MAIL 被拒，状态未进 MailGiven
}

// ---- 场景 7：RCPT 第 101 个 → 452 ----
void testTooManyRecipients() {
    // maxRecipients 默认 100；程序化生成 101 条 RCPT。
    std::string script = "EHLO x\r\nMAIL FROM:<a@b>\r\n";
    for (int i = 0; i < 101; ++i) {
        script += "RCPT TO:<u" + std::to_string(i) + "@d>\r\n";
    }
    script += "QUIT\r\n";
    ChunkSource src({script});
    LineReader reader(src, mail::kMaxWireLineOctets);
    StringSink wire;
    CollectingSink sink;
    AcceptAllVerifier v;
    Session s(reader, wire, sink, v, SessionConfig{});
    s.run();

    std::vector<std::string> lines = splitCrlf(wire.bytes());
    // 前 100 条 RCPT + MAIL 各回 250（共 101 行 250 OK），第 101 条回 452。
    CHECK(countPrefix(lines, "452") == 1);
    CHECK(countPrefix(lines, "250 OK") == 101);
}

// ---- 场景 8：乱序时序 → 503 ----
void testOutOfSequence() {
    // 8a：未 EHLO 直接 MAIL → 503。
    {
        ChunkSource src({"MAIL FROM:<a@b>\r\nQUIT\r\n"});
        LineReader reader(src, mail::kMaxWireLineOctets);
        StringSink wire;
        CollectingSink sink;
        AcceptAllVerifier v;
        Session s(reader, wire, sink, v, SessionConfig{});
        s.run();
        std::vector<std::string> lines = splitCrlf(wire.bytes());
        CHECK(startsWith(lines[1], "503"));  // 紧随 220 之后
    }
    // 8b：EHLO 后未 RCPT 直接 DATA → 503。
    {
        ChunkSource src({"EHLO x\r\nMAIL FROM:<a@b>\r\nDATA\r\nQUIT\r\n"});
        LineReader reader(src, mail::kMaxWireLineOctets);
        StringSink wire;
        CollectingSink sink;
        AcceptAllVerifier v;
        Session s(reader, wire, sink, v, SessionConfig{});
        s.run();
        std::vector<std::string> lines = splitCrlf(wire.bytes());
        // 220 + EHLO(4) + MAIL(250) + DATA(503) + QUIT(221)
        CHECK(startsWith(lines[6], "503"));
    }
}

// ---- 场景 9：RSET 后事务清空，再 MAIL 能成功 ----
void testRsetClearsTransaction() {
    std::string script =
        "EHLO x\r\n"
        "MAIL FROM:<a@b>\r\n"
        "RCPT TO:<c@d>\r\n"
        "RSET\r\n"
        "MAIL FROM:<e@f>\r\n"  // RSET 后应重新可 MAIL
        "RCPT TO:<g@h>\r\n"    // 且事务干净，可继续 RCPT
        "QUIT\r\n";
    ChunkSource src({script});
    LineReader reader(src, mail::kMaxWireLineOctets);
    StringSink wire;
    CollectingSink sink;
    AcceptAllVerifier v;
    Session s(reader, wire, sink, v, SessionConfig{});
    s.run();

    std::vector<std::string> lines = splitCrlf(wire.bytes());
    // 220 + EHLO(4) + MAIL(250) + RCPT(250) + RSET(250) + MAIL(250) + RCPT(250) + QUIT(221)
    CHECK(lines.size() == 11);
    CHECK(startsWith(lines[7], "250 "));  // RSET
    CHECK(startsWith(lines[8], "250 "));  // 第二次 MAIL 成功
    CHECK(startsWith(lines[9], "250 "));  // 第二次 RCPT 成功
}

// ---- 场景 10：MAIL FROM:<> 空反向路径被接受 ----
void testEmptyReversePathAccepted() {
    std::string script =
        "EHLO x\r\n"
        "MAIL FROM:<>\r\n"
        "RCPT TO:<c@d>\r\n"
        "DATA\r\n"
        "hi\r\n"
        ".\r\n"
        "QUIT\r\n";
    ChunkSource src({script});
    LineReader reader(src, mail::kMaxWireLineOctets);
    StringSink wire;
    CollectingSink sink;
    AcceptAllVerifier v;
    Session s(reader, wire, sink, v, SessionConfig{});
    s.run();

    std::vector<std::string> lines = splitCrlf(wire.bytes());
    CHECK(startsWith(lines[5], "250 "));  // MAIL FROM:<> 接受
    CHECK(sink.deliveries.size() == 1);
    CHECK(sink.deliveries[0].envelope.sender.empty());  // 空反向路径
    CHECK(sink.deliveries[0].data == "hi\r\n");
}

// ---- 场景 11：裸 LF 分帧 → 500 后会话结束（run 返回 Error）----
void testBareLfEndsSession() {
    ChunkSource src({"NOOP\r\n", "X\n"});  // 第二块含裸 LF
    LineReader reader(src, mail::kMaxWireLineOctets);
    StringSink wire;
    CollectingSink sink;
    AcceptAllVerifier v;
    Session s(reader, wire, sink, v, SessionConfig{});
    CHECK(s.run() == IoStatus::Error);  // smuggling 防御：回 500 后终止

    std::vector<std::string> lines = splitCrlf(wire.bytes());
    // 220 + NOOP(250) + 500，之后会话结束。
    CHECK(lines.size() == 3);
    CHECK(startsWith(lines[1], "250 "));
    CHECK(startsWith(lines[2], "500"));
}

// ---- 附加：ParseError 优先于 503，且 Syntax→501 / BadParam→555 ----
void testParseErrorPrecedence() {
    // 从 Start 状态发一条语法错误的 MAIL：ParseError(501) 应先于 503 时序错误。
    ChunkSource src(
        {"MAIL FROM:user@b\r\n"       // 缺尖括号 → Syntax → 501
         "MAIL FROM:<a@b> BOGUS=1\r\n"  // 未知参数 → BadParam → 555（仍在 Start，但 ParseError 优先）
         "QUIT\r\n"});
    LineReader reader(src, mail::kMaxWireLineOctets);
    StringSink wire;
    CollectingSink sink;
    AcceptAllVerifier v;
    Session s(reader, wire, sink, v, SessionConfig{});
    s.run();

    std::vector<std::string> lines = splitCrlf(wire.bytes());
    CHECK(startsWith(lines[1], "501"));  // Syntax，而非 503
    CHECK(startsWith(lines[2], "555"));  // BadParam，而非 503
}

// ---- 附加：VRFY→252、NOOP→250、Unknown→500 ----
void testMiscVerbs() {
    ChunkSource src({"VRFY someone\r\nNOOP\r\nFROBNICATE\r\nQUIT\r\n"});
    LineReader reader(src, mail::kMaxWireLineOctets);
    StringSink wire;
    CollectingSink sink;
    AcceptAllVerifier v;
    Session s(reader, wire, sink, v, SessionConfig{});
    s.run();

    std::vector<std::string> lines = splitCrlf(wire.bytes());
    CHECK(startsWith(lines[1], "252"));  // VRFY
    CHECK(startsWith(lines[2], "250 "));  // NOOP
    CHECK(startsWith(lines[3], "500"));  // Unknown 动词
    CHECK(startsWith(lines[4], "221 "));  // QUIT
}

// ---- 附加：HELO 单行 250 ----
void testHeloSingleLine() {
    ChunkSource src({"HELO client\r\nQUIT\r\n"});
    LineReader reader(src, mail::kMaxWireLineOctets);
    StringSink wire;
    CollectingSink sink;
    AcceptAllVerifier v;
    Session s(reader, wire, sink, v, SessionConfig{});
    s.run();

    std::vector<std::string> lines = splitCrlf(wire.bytes());
    // 220 + HELO(单行 250) + QUIT(221) = 3 行（HELO 不产生多行扩展）
    CHECK(lines.size() == 3);
    CHECK(startsWith(lines[1], "250 "));  // 空格分隔符 = 单行
    CHECK(lines[1].find("localhost") != std::string::npos);
}

// ---- 附加：RCPT 永久拒绝 → 550 ----
void testRcptRejectPermanent() {
    ChunkSource src({"EHLO x\r\nMAIL FROM:<a@b>\r\nRCPT TO:<c@d>\r\nQUIT\r\n"});
    LineReader reader(src, mail::kMaxWireLineOctets);
    StringSink wire;
    CollectingSink sink;
    FixedVerifier v(RcptDecision::RejectPermanent);
    Session s(reader, wire, sink, v, SessionConfig{});
    s.run();

    std::vector<std::string> lines = splitCrlf(wire.bytes());
    CHECK(startsWith(lines[6], "550"));  // RCPT 被永久拒绝
}

// ---- 附加：deliver 返回 Transient→451 / Permanent→554 ----
void testDeliverFailureCodes() {
    for (int variant = 0; variant < 2; ++variant) {
        ChunkSource src(
            {"EHLO x\r\nMAIL FROM:<a@b>\r\nRCPT TO:<c@d>\r\nDATA\r\nhi\r\n.\r\nQUIT\r\n"});
        LineReader reader(src, mail::kMaxWireLineOctets);
        StringSink wire;
        CollectingSink sink;
        sink.status =
            (variant == 0) ? SinkStatus::TransientFail : SinkStatus::PermanentFail;
        AcceptAllVerifier v;
        Session s(reader, wire, sink, v, SessionConfig{});
        s.run();

        std::vector<std::string> lines = splitCrlf(wire.bytes());
        // 220 + EHLO(4) + MAIL + RCPT + DATA(354) + 投递结果 = index 8
        if (variant == 0) {
            CHECK(startsWith(lines[8], "451"));
        } else {
            CHECK(startsWith(lines[8], "554"));
        }
    }
}

// ============================ AUTH（RFC 4954）============================
//
// 共用固件：mkdtemp 建的临时目录里放一份单用户的用户档，据此建 UserStore。口令哈希
// 在 main 里只算一次（每次 Argon2 约 35 ms、64 MiB），全部 AUTH 用例复用同一实例。

const std::string kAuthUser = "alice";
const std::string kAuthPassword = "alice-secret";

std::string g_authDir;
std::unique_ptr<UserStore> g_authStore;

// 启用 AUTH 的会话配置。userStore 借用全局实例，其寿命覆盖整个 main。
SessionConfig authConfig() {
    SessionConfig cfg;
    cfg.allowPlaintextAuth = true;
    cfg.userStore = g_authStore.get();
    return cfg;
}

// 构造 SASL PLAIN 的载荷：[authzid] NUL authcid NUL passwd。authzid 留空（常态）。
// 用 push_back('\0') 而非字面量拼接：嵌入 NUL 必须按长度承载，不能经手 C 字符串。
std::string plainPayload(std::string_view user, std::string_view pass) {
    std::string s;
    s.push_back('\0');
    s.append(user);
    s.push_back('\0');
    s.append(pass);
    return s;
}

// 递归删除 path。尽力而为，忽略中途错误（照抄 auth_user_store_test.cpp）。
void removeRecursive(const std::string& path) {
    struct stat st{};
    if (::lstat(path.c_str(), &st) != 0) {
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        DIR* d = ::opendir(path.c_str());
        if (d) {
            while (dirent* e = ::readdir(d)) {
                std::string name = e->d_name;
                if (name == "." || name == "..") {
                    continue;
                }
                removeRecursive(path + "/" + name);
            }
            ::closedir(d);
        }
        ::rmdir(path.c_str());
    } else {
        ::unlink(path.c_str());
    }
}

// 跑一段脚本，返回线路上的全部字节。AUTH 用例的公共驱动。
std::string runScript(const std::string& script, const SessionConfig& cfg) {
    ChunkSource src({script});
    LineReader reader(src, mail::kMaxWireLineOctets);
    StringSink wire;
    CollectingSink sink;
    AcceptAllVerifier v;
    Session s(reader, wire, sink, v, cfg);
    s.run();
    return wire.bytes();
}

// ---- AUTH 1：启用时 EHLO 宣告 AUTH 能力，且排在末行 ----
void testEhloAdvertisesAuthWhenEnabled() {
    std::vector<std::string> lines =
        splitCrlf(runScript("EHLO x\r\nQUIT\r\n", authConfig()));
    CHECK(lines.size() >= 7);
    CHECK(lines[4] == "250-8BITMIME");           // 不再是末行，分隔符变连字符
    CHECK(lines[5] == "250 AUTH PLAIN LOGIN");   // 末行
}

// ---- AUTH 2：默认配置下 EHLO 完全不提 AUTH（与本里程碑之前逐字节一致）----
void testEhloOmitsAuthByDefault() {
    std::vector<std::string> lines =
        splitCrlf(runScript("EHLO x\r\nQUIT\r\n", SessionConfig{}));
    CHECK(lines[4] == "250 8BITMIME");  // 仍是末行
    for (const std::string& l : lines) {
        CHECK(l.find("AUTH") == std::string::npos);
    }
}

// ---- AUTH 3：AUTH PLAIN 带初始响应，一轮完成 → 235 ----
void testAuthPlainInitialResponseSucceeds() {
    std::string ir =
        mail::base64Encode(plainPayload(kAuthUser, kAuthPassword));
    std::vector<std::string> lines = splitCrlf(
        runScript("EHLO x\r\nAUTH PLAIN " + ir + "\r\nQUIT\r\n", authConfig()));
    CHECK(countPrefix(lines, "235") == 1);
    CHECK(countPrefix(lines, "334") == 0);  // 有初始响应就不该有挑战
}

// ---- AUTH 4：AUTH PLAIN 无初始响应 → 空挑战必须是 "334 \r\n"（带尾随空格）----
void testAuthPlainChallengeSucceeds() {
    std::string resp =
        mail::base64Encode(plainPayload(kAuthUser, kAuthPassword));
    std::string wire =
        runScript("EHLO x\r\nAUTH PLAIN\r\n" + resp + "\r\nQUIT\r\n", authConfig());
    // 逐字节核对：裸 "334\r\n" 不合规，客户端会解析失败。
    CHECK(wire.find("334 \r\n") != std::string::npos);

    std::vector<std::string> lines = splitCrlf(wire);
    // EHLO 启用 AUTH 后占 lines[1..5]（末行 250 AUTH PLAIN LOGIN），故空挑战在 lines[6]。
    CHECK(lines[6] == "334 ");
    CHECK(countPrefix(lines, "235") == 1);
}

// ---- AUTH 5："AUTH PLAIN =" 是空初始响应，不是"缺省"：不得发挑战，直接 535 ----
void testAuthPlainEqualsIsEmptyInitialResponse() {
    std::vector<std::string> lines =
        splitCrlf(runScript("EHLO x\r\nAUTH PLAIN =\r\nQUIT\r\n", authConfig()));
    CHECK(countPrefix(lines, "535") == 1);
    CHECK(countPrefix(lines, "334") == 0);  // 关键：'=' 与"缺省"不可混同
}

// ---- AUTH 6：LOGIN 两轮挑战，base64 逐字节精确 ----
void testAuthLoginTwoChallenges() {
    std::string u = mail::base64Encode(kAuthUser);
    std::string p = mail::base64Encode(kAuthPassword);
    std::vector<std::string> lines = splitCrlf(runScript(
        "EHLO x\r\nAUTH LOGIN\r\n" + u + "\r\n" + p + "\r\nQUIT\r\n",
        authConfig()));
    // EHLO 占 lines[1..5]，两轮挑战随后落在 lines[6]、lines[7]。
    CHECK(lines[6] == "334 VXNlcm5hbWU6");  // "Username:"
    CHECK(lines[7] == "334 UGFzc3dvcmQ6");  // "Password:"
    CHECK(startsWith(lines[8], "235"));
}

// ---- AUTH 7："*" 取消交换 → 501，会话存活 ----
void testAuthCancelReturns501() {
    std::vector<std::string> lines = splitCrlf(
        runScript("EHLO x\r\nAUTH PLAIN\r\n*\r\nNOOP\r\nQUIT\r\n", authConfig()));
    CHECK(countPrefix(lines, "501") == 1);
    // EHLO(1..5) + 334(6) + 501(7) + NOOP 的 250(8)。
    CHECK(startsWith(lines[8], "250 "));   // 取消后的 NOOP 仍正常
    CHECK(startsWith(lines.back(), "221 "));
}

// ---- AUTH 8：初始响应 base64 非法 → 501，**不是** 535 ----
void testAuthBadBase64Returns501() {
    std::vector<std::string> lines = splitCrlf(
        runScript("EHLO x\r\nAUTH PLAIN !!!!\r\nQUIT\r\n", authConfig()));
    CHECK(countPrefix(lines, "501") == 1);
    CHECK(countPrefix(lines, "535") == 0);  // 语法错不得谎报成凭据错
}

// ---- AUTH 9：超长续行（2100 字节，超过分帧上限 2048）→ 500，会话存活 ----
void testAuthOverlongContinuationReturns500() {
    std::string overlong(2100, 'A');
    std::vector<std::string> lines = splitCrlf(runScript(
        "EHLO x\r\nAUTH PLAIN\r\n" + overlong + "\r\nNOOP\r\nQUIT\r\n",
        authConfig()));
    CHECK(countPrefix(lines, "500") == 1);
    // EHLO(1..5) + 334(6) + 500(7) + NOOP 的 250(8)。
    CHECK(startsWith(lines[8], "250 "));   // 超长行后 NOOP 仍正常
    CHECK(startsWith(lines.back(), "221 "));
}

// ---- AUTH 10：1024 字节的 base64 续行（含 CRLF 1026）必须被正常处理 ----
// 这是 kMaxAuthLineOctets 取值的定尺测试：RFC 4616 要求 authzid/authcid/passwd 各支
// 持到 255 字节，最坏 767 字节原文 → base64 后 1024。此行必须走到机制（得到 535 的
// 凭据裁决），绝不能被行长上限拦成 500。
void testAuthContinuationAt1026OctetsAccepted() {
    // 768 字节原文（768 % 3 == 0，故 base64 恰好 1024 字符且无填充）。
    std::string longPassword(768 - 1 - kAuthUser.size() - 1, 'p');
    std::string payload = plainPayload(kAuthUser, longPassword);
    CHECK(payload.size() == 768);
    std::string resp = mail::base64Encode(payload);
    CHECK(resp.size() == 1024);

    std::vector<std::string> lines = splitCrlf(runScript(
        "EHLO x\r\nAUTH PLAIN\r\n" + resp + "\r\nQUIT\r\n", authConfig()));
    CHECK(countPrefix(lines, "500") == 0);  // 关键：不得被行长上限拦下
    CHECK(countPrefix(lines, "535") == 1);  // 口令不对，但确实走到了机制
}

// ---- AUTH 11：已认证后再 AUTH → 503 ----
void testDoubleAuthReturns503() {
    std::string ir =
        mail::base64Encode(plainPayload(kAuthUser, kAuthPassword));
    std::vector<std::string> lines = splitCrlf(runScript(
        "EHLO x\r\nAUTH PLAIN " + ir + "\r\nAUTH PLAIN " + ir + "\r\nQUIT\r\n",
        authConfig()));
    CHECK(countPrefix(lines, "235") == 1);
    CHECK(countPrefix(lines, "503") == 1);
}

// ---- AUTH 12：事务进行中 AUTH → 503（RFC 4954 MUST）----
void testAuthDuringTransactionReturns503() {
    std::string ir =
        mail::base64Encode(plainPayload(kAuthUser, kAuthPassword));
    std::vector<std::string> lines = splitCrlf(runScript(
        "EHLO x\r\nMAIL FROM:<a@b>\r\nAUTH PLAIN " + ir + "\r\nQUIT\r\n",
        authConfig()));
    CHECK(countPrefix(lines, "503") == 1);
    CHECK(countPrefix(lines, "235") == 0);
}

// ---- AUTH 13：EHLO 之前 AUTH → 503 ----
void testAuthBeforeEhloReturns503() {
    std::string ir =
        mail::base64Encode(plainPayload(kAuthUser, kAuthPassword));
    std::vector<std::string> lines = splitCrlf(
        runScript("AUTH PLAIN " + ir + "\r\nQUIT\r\n", authConfig()));
    CHECK(startsWith(lines[1], "503"));  // 紧随 220 之后
    CHECK(countPrefix(lines, "235") == 0);
}

// ---- AUTH 14：默认配置（未启用）下 AUTH → 504，绝不发 334 ----
void testAuthWhenDisallowedReturns504() {
    std::vector<std::string> lines =
        splitCrlf(runScript("EHLO x\r\nAUTH PLAIN\r\nQUIT\r\n", SessionConfig{}));
    CHECK(countPrefix(lines, "504") == 1);
    CHECK(countPrefix(lines, "334") == 0);
}

// ---- AUTH 15：不认识的机制名 → 504 ----
void testUnknownMechanismReturns504() {
    std::vector<std::string> lines = splitCrlf(
        runScript("EHLO x\r\nAUTH CRAM-MD5\r\nQUIT\r\n", authConfig()));
    CHECK(countPrefix(lines, "504") == 1);
    CHECK(countPrefix(lines, "334") == 0);
}

// ---- AUTH 16：认证身份跨越 RSET 与第二次 EHLO 存活 ----
// 二者都只复位事务与 state_。再次 AUTH 得到 503（"已认证"）即证明身份两次都没被清掉。
void testAuthSurvivesRsetAndSecondEhlo() {
    std::string ir =
        mail::base64Encode(plainPayload(kAuthUser, kAuthPassword));
    std::vector<std::string> lines = splitCrlf(runScript(
        "EHLO x\r\nAUTH PLAIN " + ir + "\r\nRSET\r\nEHLO x\r\nAUTH PLAIN " + ir +
        "\r\nQUIT\r\n", authConfig()));
    // 220 + EHLO(5) + 235 + RSET(250) + EHLO(5) + 503 + QUIT(221) = 15 行
    CHECK(lines.size() == 15);
    CHECK(startsWith(lines[6], "235"));   // 首次 AUTH
    CHECK(startsWith(lines[7], "250 "));  // RSET
    CHECK(startsWith(lines[12], "250 "));  // 第二次 EHLO 的末行
    CHECK(startsWith(lines[13], "503"));   // 身份仍在 → 重复 AUTH
    CHECK(countPrefix(lines, "235") == 1);  // Argon2 只跑了一次
}

// ---- AUTH 17：requireAuthForMail 下未认证 MAIL → 530；认证后 → 250 ----
void testRequireAuthForMailReturns530() {
    SessionConfig cfg = authConfig();
    cfg.requireAuthForMail = true;
    std::string ir =
        mail::base64Encode(plainPayload(kAuthUser, kAuthPassword));
    std::vector<std::string> lines = splitCrlf(runScript(
        "EHLO x\r\nMAIL FROM:<a@b>\r\nAUTH PLAIN " + ir +
        "\r\nMAIL FROM:<a@b>\r\nQUIT\r\n", cfg));
    // 220 + EHLO(5) + 530 + 235 + 250 + 221 = 10 行
    CHECK(lines.size() == 10);
    CHECK(startsWith(lines[6], "530"));   // 未认证的 MAIL
    CHECK(startsWith(lines[7], "235"));   // AUTH 成功
    CHECK(startsWith(lines[8], "250 "));  // 认证后的 MAIL 放行
}

// 建立 AUTH 固件：初始化加密库、mkdtemp 建临时目录、把 kAuthPassword 哈希一次写进
// 单用户档、据此建 UserStore。口令哈希在整场测试里只算这一次（Argon2 约 35 ms、
// 64 MiB），全部 AUTH 用例复用 g_authStore。失败返回 false，main 据此直接失败退出。
bool setupAuthFixture() {
    if (!mail::auth::initCrypto()) {
        std::fprintf(stderr, "initCrypto failed\n");
        return false;
    }
    char tmpl[] = "/tmp/smtp_session_auth_XXXXXX";
    if (::mkdtemp(tmpl) == nullptr) {
        std::fprintf(stderr, "mkdtemp failed\n");
        return false;
    }
    g_authDir = tmpl;

    std::string encoded;
    if (!mail::auth::hashPassword(kAuthPassword, encoded)) {
        std::fprintf(stderr, "hashPassword failed\n");
        return false;
    }
    const std::string path = g_authDir + "/users";
    {
        std::ofstream f(path, std::ios::binary);
        f << kAuthUser << ':' << encoded << '\n';
    }

    auto r = UserStore::open(path);
    if (!r) {
        std::fprintf(stderr, "UserStore::open failed\n");
        return false;
    }
    g_authStore = std::move(r).value();
    return true;
}

void teardownAuthFixture() {
    g_authStore.reset();
    if (!g_authDir.empty()) {
        removeRecursive(g_authDir);
    }
}

}  // namespace

int main() {
    testHappyPath();
    testPipelining();
    testOverlongCommandContinues();
    testDataOverlongLinePoisoned();
    testDataLineAtNewFramingBoundary();
    testDataLineJustUnderLimit();
    testMessageTooLargePoisoned();
    testMailSizeExceedsLimit();
    testTooManyRecipients();
    testOutOfSequence();
    testRsetClearsTransaction();
    testEmptyReversePathAccepted();
    testBareLfEndsSession();
    testParseErrorPrecedence();
    testMiscVerbs();
    testHeloSingleLine();
    testRcptRejectPermanent();
    testDeliverFailureCodes();

    if (!setupAuthFixture()) {
        return 1;
    }
    testEhloAdvertisesAuthWhenEnabled();
    testEhloOmitsAuthByDefault();
    testAuthPlainInitialResponseSucceeds();
    testAuthPlainChallengeSucceeds();
    testAuthPlainEqualsIsEmptyInitialResponse();
    testAuthLoginTwoChallenges();
    testAuthCancelReturns501();
    testAuthBadBase64Returns501();
    testAuthOverlongContinuationReturns500();
    testAuthContinuationAt1026OctetsAccepted();
    testDoubleAuthReturns503();
    testAuthDuringTransactionReturns503();
    testAuthBeforeEhloReturns503();
    testAuthWhenDisallowedReturns504();
    testUnknownMechanismReturns504();
    testAuthSurvivesRsetAndSecondEhlo();
    testRequireAuthForMailReturns530();
    teardownAuthFixture();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("all smtp_session tests passed");
    return 0;
}
