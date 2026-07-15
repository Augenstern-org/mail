#include "mail/net/line_reader.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace mail::net {

namespace {
constexpr std::byte kCR{static_cast<unsigned char>('\r')};
constexpr std::byte kLF{static_cast<unsigned char>('\n')};
constexpr std::size_t kFillChunk = 4096;
constexpr std::size_t kNpos = static_cast<std::size_t>(-1);

// 在缓冲 [from, size) 范围内查找第一个 CRLF，返回其 CR 的下标；找不到完整 CRLF时返回 kNpos。
std::size_t findCrlf(const std::vector<std::byte>& buf, std::size_t from) {
    for (std::size_t i = from; i + 1 < buf.size(); ++i) {
        if (buf[i] == kCR && buf[i + 1] == kLF) {
            return i;
        }
    }
    return kNpos;
}
}  // namespace

void LineReader::compact() {
    if (start_ == 0) {
        return;
    }
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(start_));
    scanned_ = (scanned_ >= start_) ? (scanned_ - start_) : 0;
    start_ = 0;
}

IoStatus LineReader::fill(std::size_t& outBytes) {
    std::byte tmp[kFillChunk];
    std::size_t got = 0;
    IoStatus st = source_.readSome(std::span<std::byte>(tmp, kFillChunk), got);
    outBytes = got;
    // 契约加固：源返回 Ok 却读到 0 字节违反 ByteSource 契约（Ok 必须 outBytes>=1）。
    // 若放行，会让 readLine/readExactly 中 "Ok 则 continue" 的循环空转成死循环，
    // 故映射为 Error 使循环终止。
    if (st == IoStatus::Ok && got == 0) {
        return IoStatus::Error;
    }
    if (got > 0) {
        // 每次追加前都无条件回收已消费前缀，使缓冲只保留存活（未消费）字节加上这次新读入的数据块。
        compact();
        buf_.insert(buf_.end(), tmp, tmp + got);
    }
    return st;
}

IoStatus LineReader::readLine(std::string& out) {
    // 若上一次因超长、且当时缓冲中无 CRLF 而进入丢弃模式，
    // 先把这一行被拒绝的超长行排空到其终止 CRLF 之后，再做正常分帧。
    if (discarding_) {
        for (;;) {
            std::size_t crlf = findCrlf(buf_, start_);
            if (crlf != kNpos) {
                start_ = crlf + 2;  // 丢弃到 CRLF 之后
                scanned_ = start_;
                discarding_ = false;
                break;  // 排空完成，转入正常分帧
            }
            // 缓冲中尚无完整 CRLF：丢弃这些被拒绝的字节。但若末尾是单个 CR，
            // 其 LF 可能还未到达——保留该 CR，以免 CRLF 被丢弃边界劈开。
            if (!buf_.empty() && buf_.back() == kCR) {
                start_ = buf_.size() - 1;
            } else {
                start_ = buf_.size();
            }
            scanned_ = start_;

            std::size_t got = 0;
            IoStatus st = fill(got);
            if (st != IoStatus::Ok) {
                // Closed/WouldBlock/Error：丢弃模式保持，下一次 readLine 继续排空。
                return st;
            }
        }
    }

    for (;;) {
        while (scanned_ < buf_.size()) {
            // 一整行是 内容 + CRLF。若内容本身加上两个 CRLF 字节已无法容纳于
            // maxLine_，则该行超长。maxLine_ 计入终止符（RFC 5321 的限值均为
            // "包含 CRLF"），故此处 "+ 2"。
            if (scanned_ - start_ + 2 > maxLine_) {
                // 超长：只丢弃这一行（连同其终止 CRLF），保留其后所有字节，以便下
                // 一次 readLine 正常对后续行分帧。
                std::size_t crlf = findCrlf(buf_, start_);
                if (crlf != kNpos) {
                    start_ = crlf + 2;  // 丢弃到该行 CRLF 之后
                    scanned_ = start_;
                    return IoStatus::LineTooLong;
                }
                // 缓冲中还没有该行的终止 CRLF：丢弃当前缓冲并进入丢弃模式，由后续
                // readLine 继续排空直到 CRLF 到达。若末尾是单个 CR，保留它以免劈开
                // 可能的 CRLF。
                if (!buf_.empty() && buf_.back() == kCR) {
                    start_ = buf_.size() - 1;
                } else {
                    start_ = buf_.size();
                }
                scanned_ = start_;
                discarding_ = true;
                return IoStatus::LineTooLong;
            }

            std::byte b = buf_[scanned_];
            if (b == kCR) {
                if (scanned_ + 1 < buf_.size()) {
                    if (buf_[scanned_ + 1] == kLF) {
                        // 找到 CRLF：行内容为 [start_, scanned_)。
                        out.assign(reinterpret_cast<const char*>(buf_.data()) + start_,
                                   scanned_ - start_);
                        start_ = scanned_ + 2;  // 消费到 LF 之后
                        scanned_ = start_;
                        return IoStatus::Ok;
                    }
                    // CR 之后不是 LF：裸 CR 分帧违规。
                    return IoStatus::Error;
                }
                // 缓冲末尾的 CR：需再读一个字节才能判断。让 scanned_ 停在该 CR 上，
                // fill 之后重新检查。
                break;
            }
            if (b == kLF) {
                // LF 前面不紧邻 CR（若前有 CR 会在上面被当作 CRLF 消费）：裸 LF 分帧违规。
                return IoStatus::Error;
            }
            ++scanned_;
        }

        std::size_t got = 0;
        IoStatus st = fill(got);
        if (st == IoStatus::Ok) {
            continue;  // 连同新读入字节重新扫描
        }
        // Closed（EOF；残行留在缓冲，out 不动）、WouldBlock、Error、Interrupted：
        // 原样上报。
        return st;
    }
}

IoStatus LineReader::readExactly(std::size_t n, std::string& out) {
    out.clear();
    if (n == 0) {
        return IoStatus::Ok;
    }

    while (buffered() < n) {
        std::size_t got = 0;
        IoStatus st = fill(got);
        if (st == IoStatus::Ok) {
            continue;
        }
        // Closed 时，已收集到的字节仍留在缓冲，故稍后重试不会丢数据。WouldBlock/
        // Error 在缓冲原封不动的情况下上报。
        return st;
    }

    out.assign(reinterpret_cast<const char*>(buf_.data()) + start_, n);
    start_ += n;
    // 这些字节按原始数据消费，未作为行扫描过；把行扫描起点重置到缓冲新前端。
    scanned_ = start_;
    return IoStatus::Ok;
}

}  // namespace mail::net
