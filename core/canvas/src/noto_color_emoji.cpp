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

#include <mutex>

namespace pulp::canvas {

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

    // The emoji face is materialised through the same process-wide manager the
    // rest of the canvas resolves against — `makeFromData` is all this needs,
    // and sharing the manager keeps the OS switch in one place.
    auto mgr = platform_font_manager();
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
