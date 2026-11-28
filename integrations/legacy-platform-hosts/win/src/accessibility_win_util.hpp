#pragma once

#include <pulp/platform/win32_sane.hpp>
#include <string>

namespace pulp::view::uia_detail {

inline BSTR make_bstr(const std::string& text) {
    if (text.empty()) return SysAllocString(L"");
    const int length = MultiByteToWideChar(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return SysAllocString(L"");
    BSTR result = SysAllocStringLen(nullptr, static_cast<UINT>(length));
    if (!result) return nullptr;
    MultiByteToWideChar(CP_UTF8, 0, text.data(),
                        static_cast<int>(text.size()), result, length);
    return result;
}

} // namespace pulp::view::uia_detail
