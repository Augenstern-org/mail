#pragma once

// net I/O 层共享的状态码。
//
// 可能部分成功的 I/O 操作（readSome、writeAll、readLine、readExactly）按值返回
// IoStatus，并通过出参传出所产生的数据。既可能产出对象、也可能失败的工厂函数使用
// mail::Result<T>（见 mail/result.hpp），它携带一个 IoStatus 加上捕获的 errno。

namespace mail {

enum class IoStatus {
    Ok,          // 操作成功完成
    Closed,      // 对端有序关闭 / EOF（具体语义见各操作）
    WouldBlock,  // 非阻塞 fd 暂无数据 / 无法推进（EAGAIN/EWOULDBLOCK）
    Interrupted, // 被信号打断（EINTR）且未在内部重试
    LineTooLong, // 累积的行超过了配置的上限
    Timeout,     // 已设置接收超时（SO_RCVTIMEO）的 fd 在等待期内未等到数据
    Error,       // 不可恢复错误；如有捕获的 errno 可据其排查
};

// 面向日志 / 诊断的可读名称。永不返回 nullptr。
inline const char* toString(IoStatus s) noexcept {
    switch (s) {
        case IoStatus::Ok:          return "Ok";
        case IoStatus::Closed:      return "Closed";
        case IoStatus::WouldBlock:  return "WouldBlock";
        case IoStatus::Interrupted: return "Interrupted";
        case IoStatus::LineTooLong: return "LineTooLong";
        case IoStatus::Timeout:     return "Timeout";
        case IoStatus::Error:       return "Error";
    }
    return "Unknown";
}

}  // namespace mail
