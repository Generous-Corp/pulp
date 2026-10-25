// system_ui_typeface.hpp — the macOS system UI font, shared by the font
// resolver and the SkParagraph font collection.
#pragma once

#if defined(__APPLE__) && defined(PULP_HAS_SKIA)

#include "include/core/SkFontStyle.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkTypeface.h"

class SkFontMgr;

namespace pulp::canvas::detail {

/// The private family name the system UI font reports to CoreText, and the
/// platform family `system-ui` and `BlinkMacSystemFont` map to.
inline constexpr const char* kSystemUiFamily = ".AppleSystemUIFont";

/// The system UI font at `style`, at its exact weight. A lookup by
/// `kSystemUiFamily` is not reliable: CoreText's name lookup answers nothing
/// for the rest of the process once a weight between the font's named
/// instances has been asked for, and a family's style set only offers the
/// named instances, while the browser interpolates the variable `wght` axis.
/// The width is not applied; callers keep the name lookup for other widths.
sk_sp<SkTypeface> system_ui_typeface(SkFontStyle style);

/// A font manager that answers only `kSystemUiFamily`, through
/// `system_ui_typeface` at the exact requested style, for SkParagraph's font
/// collection, which finds families by name. It answers nothing for a family
/// `registered` serves, so a face registered under that name keeps precedence.
sk_sp<SkFontMgr> make_system_ui_font_manager(sk_sp<SkFontMgr> registered);

} // namespace pulp::canvas::detail

#endif
