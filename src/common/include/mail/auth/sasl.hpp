#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "mail/auth/user_store.hpp"

// SASL 机制对象。职责切分：Session 负责一切线路格式（base64 编解码、334/235/501
// 回码、"*" 取消、行长上限）；机制对象只见**解码后的原始字节**。这使机制可以脱离
// base64 单测，也让"解码失败 → 501"这条 RFC 规则只存在于 Session 一处。
//
// 驱动协议：
//   1. Session 按客户端给出的机制名建对象（makeSasl）。
//   2. 命令行带初始响应 → Session 解码后调用 step(decoded)；
//      命令行不带初始响应 → Session 调用 begin()。二者是不同状态，不可混同：
//      "AUTH PLAIN" 是"给我挑战"，"AUTH PLAIN =" 是"这是我的空初始响应"。
//   3. 返回 Challenge 时，Session 把 outChallenge 编码为 base64 发 "334 <b64>"
//      （空挑战即 "334 "，带尾随空格），读下一行、解码，再调用 step。
//   4. 返回 Done 后，Session 用 result() 取裁决并回 235 / 535 / 454。

namespace mail::auth {

// 机制的下一步动作。与 RcptDecision / SinkStatus 同类的小裁决枚举，非错误枚举。
// 刻意只有两个取值：载荷不符合机制文法与凭据不匹配在线路上都是 535，没有区分的
// 必要，故文法错误也走 Done + result()==Rejected，不单列 Malformed。
enum class SaslStep {
    Challenge,  // 还需一轮：outChallenge 是要发给客户端的（未编码）挑战字节
    Done        // 交换结束：用 result() 取裁决
};

class Sasl {
public:
    virtual ~Sasl() = default;

    // 客户端未提供初始响应时的第一步。PLAIN 产出空挑战、LOGIN 产出 "Username:"；
    // 二者都必然返回 Challenge。
    virtual SaslStep begin(std::string& outChallenge) = 0;

    // 处理一段已解码的客户端响应。response 可为空串（对应线路上的 "="），且是原始
    // 字节、可含嵌入 NUL —— 实现绝不得当作 C 字符串处理（strlen/strtok 会静默截断）。
    virtual SaslStep step(std::string_view response, std::string& outChallenge) = 0;

    // 仅当上一步返回 Done 时有意义。
    virtual AuthResult result() const = 0;

    // 认证成功的用户名；仅当 result() == AuthResult::Ok 时有意义。
    virtual std::string_view authenticatedUser() const = 0;
};

// 工厂：按机制名建对象。名字 ASCII 大小写不敏感。不支持的名字返回 nullptr → Session 回 504。
// 返回的对象**借用** store，不延长其生命周期：调用方须保证 store 存活至对象销毁。
std::unique_ptr<Sasl> makeSasl(std::string_view mechanism, const UserStore& store);

// 本编译单元支持的机制名，空格分隔（"PLAIN LOGIN"），供 EHLO 的 "AUTH " 能力行
// 直接拼接。单一事实来源：将来加机制只改 sasl.cpp，不动 session.cpp。
std::string_view supportedMechanisms();

}  // namespace mail::auth
