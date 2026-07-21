#include "mail/base64.hpp"

#include <array>
#include <cstddef>
#include <string>

// base64 编解码的实现。全程按长度处理字节，绝不把输入当 C 字符串，故可正确承载嵌入
// 的 NUL（SASL PLAIN 的载荷正是以 NUL 分隔的）。

namespace mail {
namespace {

// RFC 4648 §4 的标准字母表；索引 0..63 对应六位组的取值。
constexpr char kEncodeTable[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// 逆表：合法字母表字符映射到 0..63，其余一律 -1。'=' 不在此表内，由解码主循环单独
// 处理；空白（含 CR/LF）、URL 变体的 '-' '_' 因此自然落入 -1 而被拒绝。
constexpr std::array<signed char, 256> makeDecodeTable() {
    std::array<signed char, 256> table{};
    for (std::size_t i = 0; i < table.size(); ++i) {
        table[i] = -1;
    }
    for (signed char i = 0; i < 64; ++i) {
        table[static_cast<unsigned char>(kEncodeTable[i])] = i;
    }
    return table;
}

constexpr std::array<signed char, 256> kDecodeTable = makeDecodeTable();

unsigned byteAt(std::string_view data, std::size_t index) {
    return static_cast<unsigned char>(data[index]);
}

}  // namespace

std::string base64Encode(std::string_view data) {
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);

    // 主循环按三字节 -> 四字符推进；不足三字节的尾部单独补 '='。
    std::size_t i = 0;
    for (; i + 3 <= data.size(); i += 3) {
        unsigned v = (byteAt(data, i) << 16) | (byteAt(data, i + 1) << 8) |
                     byteAt(data, i + 2);
        out += kEncodeTable[(v >> 18) & 0x3F];
        out += kEncodeTable[(v >> 12) & 0x3F];
        out += kEncodeTable[(v >> 6) & 0x3F];
        out += kEncodeTable[v & 0x3F];
    }

    const std::size_t rest = data.size() - i;
    if (rest == 1) {
        // 一个字节 -> 两个六位组（第二组的低 4 位补零）+ "=="。
        unsigned v = byteAt(data, i) << 16;
        out += kEncodeTable[(v >> 18) & 0x3F];
        out += kEncodeTable[(v >> 12) & 0x3F];
        out += '=';
        out += '=';
    } else if (rest == 2) {
        // 两个字节 -> 三个六位组（第三组的低 2 位补零）+ "="。
        unsigned v = (byteAt(data, i) << 16) | (byteAt(data, i + 1) << 8);
        out += kEncodeTable[(v >> 18) & 0x3F];
        out += kEncodeTable[(v >> 12) & 0x3F];
        out += kEncodeTable[(v >> 6) & 0x3F];
        out += '=';
    }
    return out;
}

bool base64Decode(std::string_view text, std::string& out) {
    // 严格长度：标准 base64 必然是 4 的倍数；空串合法且解码为空。
    if (text.size() % 4 != 0) {
        return false;
    }
    out.clear();
    if (text.empty()) {
        return true;
    }
    out.reserve(text.size() / 4 * 3);

    const std::size_t lastQuantum = text.size() - 4;
    for (std::size_t i = 0; i < text.size(); i += 4) {
        unsigned sextets[4] = {0, 0, 0, 0};
        std::size_t pads = 0;

        for (std::size_t k = 0; k < 4; ++k) {
            const unsigned char c = static_cast<unsigned char>(text[i + k]);
            if (c == '=') {
                // '=' 只允许出现在最后一个四元组的第 3、4 位。第 3 位是 '=' 时第 4
                // 位必然也是 '='——由下方“填充之后不得再出现字母表字符”一并保证。
                if (i != lastQuantum || k < 2) {
                    return false;
                }
                ++pads;
                continue;
            }
            if (pads != 0) {
                return false;
            }
            const signed char v = kDecodeTable[c];
            if (v < 0) {
                return false;
            }
            sextets[k] = static_cast<unsigned>(v);
        }

        // 非零填充位一律拒绝：两个 '=' 时只有第二个六位组的高 2 位有效，一个 '=' 时
        // 只有第三个六位组的高 4 位有效，余下的位必须为零。
        if (pads == 2 && (sextets[1] & 0x0F) != 0) {
            return false;
        }
        if (pads == 1 && (sextets[2] & 0x03) != 0) {
            return false;
        }

        const unsigned v = (sextets[0] << 18) | (sextets[1] << 12) |
                           (sextets[2] << 6) | sextets[3];
        out += static_cast<char>((v >> 16) & 0xFF);
        if (pads < 2) {
            out += static_cast<char>((v >> 8) & 0xFF);
        }
        if (pads == 0) {
            out += static_cast<char>(v & 0xFF);
        }
    }
    return true;
}

}  // namespace mail
