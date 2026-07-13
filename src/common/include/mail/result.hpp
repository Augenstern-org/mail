#pragma once

#include <new>
#include <type_traits>
#include <utility>

#include "mail/io_status.hpp"

// 一个极简的 值-或-错误 类型，供既可能产出 T、也可能以 IoStatus 加捕获的 errno
// 失败的工厂函数使用。刻意手写，以避免在 C++20 目标下依赖 tl::expected /
// std::expected。
//
// 不变量：
//   - 任一时刻 {值, 错误} 中恰有一个有意义。
//   - has_value() / operator bool 指示是哪一个。
//   - 仅当 has_value() 为 true 时才可调用 value()。
//   - 当 has_value() 为 false 时 error() / error_errno() 才有意义；此时 error()
//     绝不返回 IoStatus::Ok。

namespace mail {

template <class T>
class Result {
public:
    // 成功工厂。
    static Result success(T value) {
        Result r;
        r.has_value_ = true;
        ::new (static_cast<void*>(&r.storage_)) T(std::move(value));
        return r;
    }

    // 失败工厂。`status` 不应为 IoStatus::Ok；`err` 是失败点捕获的 errno（不适用
    // 时为 0）。
    static Result failure(IoStatus status, int err = 0) {
        Result r;
        r.has_value_ = false;
        r.status_ = status;
        r.errno_ = err;
        return r;
    }

    Result(const Result& other) { copyFrom(other); }
    Result(Result&& other) noexcept { moveFrom(other); }

    Result& operator=(const Result& other) {
        if (this != &other) {
            destroy();
            copyFrom(other);
        }
        return *this;
    }

    Result& operator=(Result&& other) noexcept {
        if (this != &other) {
            destroy();
            moveFrom(other);
        }
        return *this;
    }

    ~Result() { destroy(); }

    bool has_value() const noexcept { return has_value_; }
    explicit operator bool() const noexcept { return has_value_; }

    // 前置条件：has_value() == true。
    T& value() & noexcept { return *ptr(); }
    const T& value() const& noexcept { return *ptr(); }
    T&& value() && noexcept { return std::move(*ptr()); }

    // 前置条件：has_value() == false。绝不返回 IoStatus::Ok。
    IoStatus error() const noexcept { return status_; }
    int error_errno() const noexcept { return errno_; }

private:
    Result() = default;

    T* ptr() noexcept { return std::launder(reinterpret_cast<T*>(&storage_)); }
    const T* ptr() const noexcept {
        return std::launder(reinterpret_cast<const T*>(&storage_));
    }

    void destroy() noexcept {
        if (has_value_) {
            ptr()->~T();
        }
    }

    void copyFrom(const Result& other) {
        has_value_ = other.has_value_;
        if (has_value_) {
            ::new (static_cast<void*>(&storage_)) T(*other.ptr());
        } else {
            status_ = other.status_;
            errno_ = other.errno_;
        }
    }

    void moveFrom(Result& other) noexcept {
        has_value_ = other.has_value_;
        if (has_value_) {
            ::new (static_cast<void*>(&storage_)) T(std::move(*other.ptr()));
        } else {
            status_ = other.status_;
            errno_ = other.errno_;
        }
    }

    alignas(T) unsigned char storage_[sizeof(T)];
    bool has_value_ = false;
    IoStatus status_ = IoStatus::Error;
    int errno_ = 0;
};

}  // namespace mail
