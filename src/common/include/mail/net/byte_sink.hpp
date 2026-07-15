#pragma once

#include <string_view>

#include "mail/io_status.hpp"

// 抽象的面向字节的输出，与 ByteSource 对偶。协议层持 ByteSink 而非 Connection，
// 因而可以用一个内存伪实现整场单测，为 v2 IO 解耦铺路。

namespace mail::net {

class ByteSink {
public:
    virtual ~ByteSink() = default;

    // 写完全部字节，否则失败。返回 IoStatus::Ok 表示全部字节送出；其余状态码的
    // 具体语义由实现说明（如对端消失、不可恢复错误）。
    virtual IoStatus writeAll(std::string_view data) = 0;
};

}  // namespace mail::net
