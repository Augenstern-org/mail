// mail::net::LineReader 的单元测试，完全通过一个内存 ByteSource 伪实现驱动（不涉及
// 套接字）。该伪实现保留脚本化的分块边界，便于演练跨读取拆分的 CRLF，以及一次读取
// 中包含多行的情形。
//
// 不使用测试框架：一个极小的 CHECK 宏记录失败，任一检查失败时进程返回非零。

#include <cstddef>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include "mail/io_status.hpp"
#include "mail/net/byte_source.hpp"
#include "mail/net/line_reader.hpp"

using mail::IoStatus;
using mail::net::ByteSource;
using mail::net::LineReader;

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

void testSimpleLine() {
    ChunkSource src({"hello\r\n"});
    LineReader reader(src);
    std::string line;
    CHECK(reader.readLine(line) == IoStatus::Ok);
    CHECK(line == "hello");
    CHECK(reader.buffered() == 0);
}

void testLineAcrossTwoChunks() {
    ChunkSource src({"ab", "cd\r\n"});
    LineReader reader(src);
    std::string line;
    CHECK(reader.readLine(line) == IoStatus::Ok);
    CHECK(line == "abcd");
}

void testCrlfSplitAcrossChunks() {
    // CR 在一个分块到达，LF 在下一个分块。
    ChunkSource src({"split\r", "\n"});
    LineReader reader(src);
    std::string line;
    CHECK(reader.readLine(line) == IoStatus::Ok);
    CHECK(line == "split");
}

void testTwoLinesInOneChunk() {
    ChunkSource src({"one\r\ntwo\r\n"});
    LineReader reader(src);
    std::string line;
    CHECK(reader.readLine(line) == IoStatus::Ok);
    CHECK(line == "one");
    CHECK(reader.buffered() == 5);  // "two\r\n" 仍在缓冲
    CHECK(reader.readLine(line) == IoStatus::Ok);
    CHECK(line == "two");
    CHECK(reader.buffered() == 0);
}

void testReadExactlyFromSurplusAndFreshRead() {
    // "cmd\r\n" 完成一行；"AB" 是多余字节；"CDE" 在稍后的读取到达。
    // readExactly(5) 必须拼接缓冲的多余字节与新读入的字节。
    ChunkSource src({"cmd\r\nAB", "CDE"});
    LineReader reader(src);
    std::string line;
    CHECK(reader.readLine(line) == IoStatus::Ok);
    CHECK(line == "cmd");
    CHECK(reader.buffered() == 2);  // "AB"
    std::string blob;
    CHECK(reader.readExactly(5, blob) == IoStatus::Ok);
    CHECK(blob == "ABCDE");
    CHECK(reader.buffered() == 0);
}

void testReadExactlyTreatsCrlfAsData() {
    ChunkSource src({"a\r\nb"});
    LineReader reader(src);
    std::string blob;
    CHECK(reader.readExactly(4, blob) == IoStatus::Ok);
    CHECK(blob.size() == 4);
    CHECK(blob == std::string("a\r\nb"));
}

void testLineTooLong() {
    // maxLine 8：内容+CRLF 必须容纳于 8，故内容 > 6 即超长。
    ChunkSource src({"AAAAAAAAAAAA"});  // 12 字节，无 CRLF
    LineReader reader(src, 8);
    std::string line;
    CHECK(reader.readLine(line) == IoStatus::LineTooLong);
}

void testMaxLineBoundaryAccepted() {
    // maxLine 8：6 字节内容 + CRLF == 恰好 8，允许。
    ChunkSource src({"ABCDEF\r\n"});
    LineReader reader(src, 8);
    std::string line;
    CHECK(reader.readLine(line) == IoStatus::Ok);
    CHECK(line == "ABCDEF");
}

void testLineTooLongPreservesFollowingLine() {
    // 超长行与其 CRLF、以及下一行都在同一个分块里到达（模拟一次 recv 交付了流水线
    // 化的多条命令）。第一次 readLine 必须只丢弃这条超长行，并保留其后的整行。
    std::string chunk(600, 'A');
    chunk += "\r\nB1 NOOP\r\n";
    ChunkSource src({chunk});
    LineReader reader(src, 512);
    std::string line;
    CHECK(reader.readLine(line) == IoStatus::LineTooLong);
    CHECK(reader.readLine(line) == IoStatus::Ok);
    CHECK(line == "B1 NOOP");
}

void testLineTooLongDiscardModeAcrossChunks() {
    // 超长行的 CRLF 此时尚未到达：第一个分块是 600 个 'A'（无 CRLF），故读取器进入
    // 丢弃模式并返回 LineTooLong；第二个分块补上该行的 CRLF 以及随后的整行，下一次
    // readLine 必须排空被拒绝行后返回后续行。
    std::string chunk1(600, 'A');
    ChunkSource src({chunk1, "\r\nNEXT\r\n"});
    LineReader reader(src, 512);
    std::string line;
    CHECK(reader.readLine(line) == IoStatus::LineTooLong);
    CHECK(reader.readLine(line) == IoStatus::Ok);
    CHECK(line == "NEXT");
}

void testClosedOnEof() {
    ChunkSource src({});  // 无可读数据
    LineReader reader(src);
    std::string line;
    CHECK(reader.readLine(line) == IoStatus::Closed);
}

void testClosedMidLine() {
    // 有数据但无终止 CRLF，随后 EOF：Closed，多余字节保留在缓冲。
    ChunkSource src({"partial"});
    LineReader reader(src);
    std::string line;
    CHECK(reader.readLine(line) == IoStatus::Closed);
    CHECK(reader.buffered() == 7);
}

void testBareLfIsFramingError() {
    ChunkSource src({"oops\n"});
    LineReader reader(src);
    std::string line;
    CHECK(reader.readLine(line) == IoStatus::Error);
}

void testBareCrIsFramingError() {
    ChunkSource src({"oops\rX"});
    LineReader reader(src);
    std::string line;
    CHECK(reader.readLine(line) == IoStatus::Error);
}

}  // namespace

int main() {
    testSimpleLine();
    testLineAcrossTwoChunks();
    testCrlfSplitAcrossChunks();
    testTwoLinesInOneChunk();
    testReadExactlyFromSurplusAndFreshRead();
    testReadExactlyTreatsCrlfAsData();
    testLineTooLong();
    testMaxLineBoundaryAccepted();
    testLineTooLongPreservesFollowingLine();
    testLineTooLongDiscardModeAcrossChunks();
    testClosedOnEof();
    testClosedMidLine();
    testBareLfIsFramingError();
    testBareCrIsFramingError();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("all line_reader tests passed");
    return 0;
}
