#include <pulp/canvas/bundled_fonts.hpp>

#ifdef PULP_HAS_SKIA

#include "include/core/SkFontMgr.h"

// Platform font manager includes stay in this TU so the main SkiaCanvas
// implementation does not carry platform-port setup for a compatibility shim.
#ifdef __APPLE__
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

namespace pulp::canvas {

// pulp #2163 / font v2 Slice 1.1.a — `platform_font_manager()` lives in
// bundled_fonts.cpp (exported via bundled_fonts.hpp); this compatibility shim
// stays as `get_font_manager()` until the broader caller-migration pass moves
// legacy internal call sites onto the resolver.
sk_sp<SkFontMgr> get_font_manager() {
    return platform_font_manager();
}

} // namespace pulp::canvas

#endif // PULP_HAS_SKIA
