// noto_color_emoji_stub.cpp — stub for builds without
// `PULP_BUNDLE_NOTO_COLOR_EMOJI`. Provides the same
// `register_bundled_noto_color_emoji()` symbol so callers don't have to
// conditionally compile against the option.

#include <pulp/canvas/text_font_context.hpp>

#ifdef PULP_HAS_SKIA

namespace pulp::canvas {

bool register_bundled_noto_color_emoji() {
    return false;
}

} // namespace pulp::canvas

#endif // PULP_HAS_SKIA
