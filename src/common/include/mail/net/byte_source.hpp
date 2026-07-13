#pragma once

#include <cstddef>
#include <span>

#include "mail/io_status.hpp"

// 抽象的面向字节的输入。LineReader 消费 ByteSource 而非直接消费套接字，从而可以对
// 一个内存伪实现做单元测试。

namespace mail::net {

class ByteSource {
public:
    virtual ~ByteSource() = default;

    // 最多读取 buf.size() 个字节到 buf。
    //
    //   - 成功时返回 IoStatus::Ok，并把 outBytes 设为读到的字节数（1..buf.size()）。
    //   - 对端有序关闭 / EOF 时返回 IoStatus::Closed，outBytes == 0。
    //   - 非阻塞源遇到 would-block 时返回 IoStatus::WouldBlock，outBytes == 0。
    //   - 出错时返回 IoStatus::Error，outBytes == 0。
    //
    // 实现必须在每条路径上都设置 outBytes。
    virtual IoStatus readSome(std::span<std::byte> buf, std::size_t& outBytes) = 0;
};

}  // namespace mail::net
