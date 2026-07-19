#pragma once

#include <string_view>
#include <utility>

namespace pulp::timeline::detail {

template <typename Append>
bool append_quoted_json_string(std::string_view value, Append&& append) {
    constexpr char digits[] = "0123456789abcdef";
    if (!append("\""))
        return false;
    for (const auto raw : value) {
        const auto byte = static_cast<unsigned char>(raw);
        switch (raw) {
        case '"':
            if (!append("\\\"")) return false;
            break;
        case '\\':
            if (!append("\\\\")) return false;
            break;
        case '\b':
            if (!append("\\b")) return false;
            break;
        case '\f':
            if (!append("\\f")) return false;
            break;
        case '\n':
            if (!append("\\n")) return false;
            break;
        case '\r':
            if (!append("\\r")) return false;
            break;
        case '\t':
            if (!append("\\t")) return false;
            break;
        default:
            if (byte < 0x20) {
                const char escaped[] = {'\\', 'u', '0', '0', digits[byte >> 4],
                                        digits[byte & 0x0f]};
                if (!append(std::string_view(escaped, sizeof(escaped))))
                    return false;
            } else if (!append(std::string_view(&raw, 1))) {
                return false;
            }
        }
    }
    return append("\"");
}

} // namespace pulp::timeline::detail
