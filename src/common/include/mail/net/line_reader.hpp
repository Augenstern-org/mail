#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "mail/io_status.hpp"
#include "mail/limits.hpp"
#include "mail/net/byte_source.hpp"

namespace mail::net {

// 基于 ByteSource 的带缓冲、按 CRLF 分帧的读取器。这是协议热点路径：SMTP 与
// IMAP 都按 CRLF 分帧命令，而 IMAP literal 需要一次精确字节数的读取，且要与行读
// 取共享同一份缓冲。
//
// 单一可增长缓冲同时支撑 readLine() 与 readExactly()：从源读入但尚未消费的字节会
// 保留给下一次调用，无论产生它的是哪个调用。正是这一点让 IMAP literal
// （readExactly）能取到与宣告它的命令行处于同一个数据包里的多余字节。
//
// 行分帧策略（有意为之的单一行为）：
//
//   严格按两字节序列 CR LF 分帧。返回的行会去掉尾部 CRLF。裸 LF（前面不紧邻 CR
//   的 LF）与裸 CR（后面不紧邻 LF 的 CR）都被视为协议分帧错误：readLine 返回
//   IoStatus::Error。LineReader 不会把裸 LF 当作行终止符默默接受，也不做行尾转
//   换。理由：本层服务的邮件协议均强制要求 CRLF，默默接受裸 LF 会引入 SMTP
//   smuggling 的歧义；是否更宽松由上层协议自行决定。
//
//   分帧错误（裸 CR / 裸 LF 产生的 IoStatus::Error）是终止性的：读取器不会自我
//   复位，调用方必须关闭该连接。
//
//   （在 readExactly 内部，CR 与 LF 都是普通数据字节，不受任何分帧处理。）
class LineReader {
public:
    explicit LineReader(ByteSource& source,
                        std::size_t maxLine = kDefaultMaxLineOctets)
        : source_(source), maxLine_(maxLine) {}

    LineReader(const LineReader&) = delete;
    LineReader& operator=(const LineReader&) = delete;

    // 读取一整行 CRLF 结尾的逻辑行，去掉尾部 CRLF 后放入 out。会跨任意多次
    // readSome() 累积，并把多余字节缓存给下一次调用。
    //
    // 返回值：
    //   Ok          - 成功产出一整行（out 被整体替换）。
    //   LineTooLong - 累积字节在找到 CRLF 前达到 maxLine；仅丢弃这一行超长数据
    //                 （连同其终止 CRLF），保留其后的所有字节，以便调用方报错后
    //                 下一次 readLine 正常对后续行分帧。若缓冲中此时尚无 CRLF，
    //                 则进入丢弃模式，后续 readLine 继续排空这一被拒绝的行，直到
    //                 其 CRLF 到达。
    //   Error       - 遇到裸 CR 或裸 LF（分帧违规，终止性），或底层源返回 Error。
    //   Closed      - 源到达 EOF。若行读到一半仍有缓冲字节，则保留在缓冲中（不返回
    //                 该残行）；out 不被修改。
    //   WouldBlock  - 非阻塞源此刻无法推进；已缓冲字节保留。
    IoStatus readLine(std::string& out);

    // 精确读取 n 个字节放入 out。先取缓冲中的多余字节，再从源读取更多。没有内部
    // 上限：调用方负责在调用前用策略上限（如 kMaxNonSyncLiteralOctets）校验 n。
    //
    // 返回值：
    //   Ok         - 精确产出 n 个字节（out 被整体替换）。
    //   Closed     - 在凑够 n 个字节前到达 EOF；已为本次调用读入的字节仍留在缓冲
    //                中，故重试不会静默丢数据。
    //   WouldBlock - 非阻塞源暂时无法提供足够字节。
    //   Error      - 底层源返回 Error。
    //
    // n == 0 时返回 Ok 并清空 out。
    //
    // readExactly 不受 readLine 丢弃状态（discarding_）的影响：discarding_ 只作用
    // 于 readLine。
    IoStatus readExactly(std::size_t n, std::string& out);

    // 当前内部缓冲中持有的字节数（已从源读入但尚未被 readLine/readExactly 消费）。
    std::size_t buffered() const noexcept { return buf_.size() - start_; }

private:
    // 从源拉取一个数据块进缓冲。返回源的状态。
    IoStatus fill(std::size_t& outBytes);

    // 丢弃已消费的前缀 [0, start_)，避免它在多次小读取中无界增长。
    void compact();

    ByteSource& source_;
    std::size_t maxLine_;

    std::vector<std::byte> buf_;  // 原始字节；不以 NUL 结尾
    std::size_t start_ = 0;       // buf_ 中第一个未消费字节的下标
    std::size_t scanned_ = 0;     // readLine：已扫描过 CR/LF 的字节数
    bool discarding_ = false;     // readLine：正在排空一行被拒绝的超长行直到其 CRLF
};

}  // namespace mail::net
