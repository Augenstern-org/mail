#pragma once

#include <string>
#include <string_view>

// base64 编解码——纯计算层，零 I/O、零系统调用。
//
// 放在 include/mail/ 顶层（与 limits.hpp、result.hpp、io_status.hpp 同级）而非
// auth/ 之下：docs/architecture.md:60 把 base64 / quoted-printable 归属于 M7 的
// mime::Encoder，若嵌套进 auth 会迫使 M7 反向依赖 auth 模块。

namespace mail {

// 标准 base64 字母表（RFC 4648 §4），带 '=' 填充，不插入换行。
std::string base64Encode(std::string_view data);

// 严格解码：只接受标准字母表 + 正确的 '=' 填充 + 长度为 4 的倍数；拒绝一切空白
//（含 CR/LF）、URL 变体的 '-'/'_'、多余填充、非零填充位。成功返回 true 并整体替换
// out；失败返回 false 且 out 内容未定义。
//
// 不返回 IoStatus：io_status.hpp:5-7 把 IoStatus 的口径限定为“可能部分成功的 I/O
// 操作”，而本函数是结果二元的纯计算。用 bool 避免新增错误枚举。
// out 用 std::string 而非 vector<byte>：解码后的 SASL PLAIN 载荷含嵌入 NUL，
// std::string 按长度承载没有问题，且与全树的字节口径一致。
bool base64Decode(std::string_view text, std::string& out);

}  // namespace mail
