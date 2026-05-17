// noto_color_emoji.cpp — register the embedded Noto Color Emoji typeface
// with the shared `TextFontContext`.
//
// This TU is only compiled when `PULP_BUNDLE_NOTO_COLOR_EMOJI` is ON. The
// font bytes are linked in via `pulp-bundled-noto-color-emoji`, a
// dedicated static library produced by `pulp_add_binary_data` in
// core/canvas/CMakeLists.txt. Keeping the bytes in their own library lets
// macOS / Windows release builds drop the ~5 MB payload by configuring
// `-DPULP_BUNDLE_NOTO_COLOR_EMOJI=OFF`.
//
// When the option is OFF, a sibling translation unit
// (`noto_color_emoji_stub.cpp`) provides the same symbol with a `false`
// return so callers can invoke `register_bundled_noto_color_emoji()`
// unconditionally.

#include <pulp/canvas/bundled_fonts.hpp>
#include <pulp/canvas/text_font_context.hpp>

#ifdef PULP_HAS_SKIA

#include "include/core/SkData.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkString.h"
#include "include/core/SkTypeface.h"

#include "pulp-bundled-noto-color-emoji_data.hpp"

#if defined(__APPLE__)
#include "include/ports/SkFontMgr_mac_ct.h"
#elif defined(_WIN32)
#include "include/ports/SkTypeface_win.h"
#elif defined(__ANDROID__)
#include "include/ports/SkFontMgr_android.h"
#include "include/ports/SkFontScanner_FreeType.h"
#elif defined(__linux__)
#include "include/ports/SkFontMgr_fontconfig.h"
#include "include/ports/SkFontScanner_FreeType.h"
#endif

#include <mutex>

namespace pulp::canvas {

namespace {

sk_sp<SkFontMgr> noto_font_mgr() {
#if defined(__APPLE__)
    return SkFontMgr_New_CoreText(nullptr);
#elif defined(_WIN32)
    return SkFontMgr_New_DirectWrite();
#elif defined(__ANDROID__)
    return SkFontMgr_New_Android(nullptr, SkFontScanner_Make_FreeType());
#elif defined(__linux__)
    return SkFontMgr_New_FontConfig(nullptr, SkFontScanner_Make_FreeType());
#else
    return nullptr;
#endif
}

} // namespace

bool register_bundled_noto_color_emoji() {
    static std::mutex once_mutex;
    static bool tried = false;
    static bool registered_ok = false;

    std::lock_guard<std::mutex> guard(once_mutex);
    if (tried) return registered_ok;
    tried = true;

    auto* data_ptr = pulp_bundled_noto_color_emoji::NotoColorEmoji_ttf;
    std::size_t data_size = pulp_bundled_noto_color_emoji::NotoColorEmoji_ttf_size;
    if (!data_ptr || data_size == 0) return false;

    auto mgr = noto_font_mgr();
    if (!mgr) return false;

    auto sk_data = SkData::MakeWithoutCopy(data_ptr, data_size);
    sk_sp<SkTypeface> face = mgr->makeFromData(std::move(sk_data));
    if (!face) return false;

    if (!register_emoji_fallback(std::move(face))) return false;
    registered_ok = true;
    return true;
}

} // namespace pulp::canvas

#endif // PULP_HAS_SKIA
