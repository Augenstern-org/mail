// mail::smtp::Session 的整场会话单元测试。完全经由内存伪实现驱动：脚本化分块的
// ByteSource（沿用 line_reader_test.cpp 的 ChunkSource）喂入命令字节，内存 ByteSink
// 收集线路上的应答字节，MessageSink / RecipientVerifier 用可配置伪实现替换。不涉及
// 套接字，也不改动任何产品代码。
//
// 不使用测试框架：一个极小的 CHECK 宏记录失败，任一检查失败时进程返回非零（沿用
// line_reader_test.cpp 的模式）。

#include <cstddef>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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
    LineReader reader(src, mail::kMaxDataWireLineOctets);
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
    LineReader reader(src, mail::kMaxDataWireLineOctets);
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
    std::string longCmd(600, 'A');  // 600 字节，>510 但 <1001（以 Ok 到达）
    ChunkSource src({"EHLO x\r\n", longCmd + "\r\n", "NOOP\r\n", "QUIT\r\n"});
    LineReader reader(src, mail::kMaxDataWireLineOctets);
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

// ---- 场景 4：DATA 内超长行（>1001 线径）→ 毒化 552，终止行仍存活 ----
void testDataOverlongLinePoisoned() {
    std::string longData(1005, 'X');  // >1001，触发 LineReader LineTooLong
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
    LineReader reader(src, mail::kMaxDataWireLineOctets);
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
    LineReader reader(src, mail::kMaxDataWireLineOctets);
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
    LineReader reader(src, mail::kMaxDataWireLineOctets);
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
    LineReader reader(src, mail::kMaxDataWireLineOctets);
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
        LineReader reader(src, mail::kMaxDataWireLineOctets);
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
        LineReader reader(src, mail::kMaxDataWireLineOctets);
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
    LineReader reader(src, mail::kMaxDataWireLineOctets);
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
    LineReader reader(src, mail::kMaxDataWireLineOctets);
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
    LineReader reader(src, mail::kMaxDataWireLineOctets);
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
    LineReader reader(src, mail::kMaxDataWireLineOctets);
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
    LineReader reader(src, mail::kMaxDataWireLineOctets);
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
    LineReader reader(src, mail::kMaxDataWireLineOctets);
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
    LineReader reader(src, mail::kMaxDataWireLineOctets);
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
        LineReader reader(src, mail::kMaxDataWireLineOctets);
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

}  // namespace

int main() {
    testHappyPath();
    testPipelining();
    testOverlongCommandContinues();
    testDataOverlongLinePoisoned();
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

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("all smtp_session tests passed");
    return 0;
}
