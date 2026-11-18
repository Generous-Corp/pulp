// SPDX-License-Identifier: MIT
#pragma once

// Portable UTF-8 <-> UTF-16 conversion and COM apartment policy for the
// Windows platform backends. Nothing here includes a Windows header, so the
// string handling and the apartment decision are exercised by the ordinary
// test suite on every host. Invalid UTF-8 and unpaired surrogates become
// U+FFFD, matching the replacement behaviour of the Win32 converters.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace pulp::platform::win_text {

namespace detail {

inline void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

}  // namespace detail

// UTF-16 code units -> UTF-8. The result has exactly the converted length: no
// terminator is stored and nothing is written past it.
inline std::string utf16_to_utf8(const char16_t* units, std::size_t count) {
    std::string out;
    if (units == nullptr) return out;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint32_t u = units[i];
        if (u >= 0xD800 && u <= 0xDBFF) {
            if (i + 1 < count && units[i + 1] >= 0xDC00 && units[i + 1] <= 0xDFFF) {
                const std::uint32_t lo = units[i + 1];
                detail::append_utf8(out, 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00));
                ++i;
            } else {
                detail::append_utf8(out, 0xFFFD);
            }
        } else if (u >= 0xDC00 && u <= 0xDFFF) {
            detail::append_utf8(out, 0xFFFD);
        } else {
            detail::append_utf8(out, u);
        }
    }
    return out;
}

// NUL-terminated variant; the terminator is not part of the result.
inline std::string utf16_to_utf8(const char16_t* terminated) {
    if (terminated == nullptr) return {};
    std::size_t n = 0;
    while (terminated[n] != u'\0') ++n;
    return utf16_to_utf8(terminated, n);
}

inline std::u16string utf8_to_utf16(std::string_view s) {
    std::u16string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        const auto b0 = static_cast<unsigned char>(s[i]);
        std::uint32_t cp = 0xFFFD;
        std::size_t len = 1;
        std::uint32_t min = 0;
        if (b0 < 0x80) {
            cp = b0;
        } else if (b0 >= 0xC2 && b0 <= 0xDF) {
            len = 2; cp = b0 & 0x1F; min = 0x80;
        } else if (b0 >= 0xE0 && b0 <= 0xEF) {
            len = 3; cp = b0 & 0x0F; min = 0x800;
        } else if (b0 >= 0xF0 && b0 <= 0xF4) {
            len = 4; cp = b0 & 0x07; min = 0x10000;
        }
        if (len > 1) {
            bool good = i + len <= s.size();
            for (std::size_t k = 1; good && k < len; ++k) {
                const auto b = static_cast<unsigned char>(s[i + k]);
                if ((b & 0xC0) != 0x80) good = false;
                else cp = (cp << 6) | (b & 0x3F);
            }
            if (!good || cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
                cp = 0xFFFD;
                len = 1;
            }
        }
        i += len;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
    }
    return out;
}

// COM / OLE apartment initialization results (HRESULT values, spelled out so
// this header needs no Windows include).
inline constexpr std::int32_t kComChangedMode = static_cast<std::int32_t>(0x80010106u);

// IFileDialog::Show and DoDragDrop need the calling thread to be in a
// single-threaded apartment. S_OK and S_FALSE both mean this call initialized
// (or re-entered) an STA. RPC_E_CHANGED_MODE means the thread already
// belongs to a multi-threaded apartment, so the operation must be refused
// rather than attempted in the wrong apartment.
inline constexpr bool sta_init_usable(std::int32_t hr) noexcept {
    return hr >= 0;
}

// wchar_t adapters: a Windows wchar_t is one UTF-16 code unit.
inline std::wstring to_wide(std::string_view s) {
    const std::u16string u = utf8_to_utf16(s);
    return std::wstring(u.begin(), u.end());
}

inline std::string from_wide(const wchar_t* terminated) {
    if (terminated == nullptr) return {};
    std::u16string u;
    for (std::size_t i = 0; terminated[i] != L'\0'; ++i)
        u.push_back(static_cast<char16_t>(terminated[i]));
    return utf16_to_utf8(u.data(), u.size());
}

}  // namespace pulp::platform::win_text
