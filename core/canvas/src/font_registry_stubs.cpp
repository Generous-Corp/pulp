// font_registry_stubs.cpp — non-Skia fallback for the public font-registration
// API declared in `pulp/canvas/bundled_fonts.hpp` (pulp #1150).
//
// `bundled_fonts.cpp` is only compiled when `PULP_HAS_SKIA` is on, because it
// pulls in the generated `pulp-bundled-fonts_data.hpp`, the platform font
// manager headers, and `SkTypeface`. Plugin authors should still be able to
// call `pulp::canvas::register_font(...)` from startup code without guarding
// every call site, so this TU compiles unconditionally and provides
// always-fail stubs that the linker picks when Skia is not linked in.
//
// Why a separate file?
//   * Adding `bundled_fonts.cpp` to the always-compiled source list would
//     drag the bundled-fonts header (and thus the pulp_add_binary_data
//     pipeline) into non-GPU builds that have no need for it.
//   * Defining the stubs inside `bundled_fonts.cpp` outside the Skia guard
//     would still leave them un-compiled on non-Skia builds because the
//     CMakeLists only adds bundled_fonts.cpp to `pulp-canvas` when
//     `PULP_HAS_SKIA` is true.
//
// When Skia is on, `bundled_fonts.cpp` provides the real implementations and
// this file is a no-op (the `#ifndef PULP_HAS_SKIA` guard skips the body).

#include <pulp/canvas/bundled_fonts.hpp>

#ifndef PULP_HAS_SKIA

namespace pulp::canvas {

bool register_font(const std::uint8_t*, std::size_t, const std::string&) {
    return false;
}

bool register_font_file(const std::string&, const std::string&) {
    return false;
}

bool is_font_registered(const std::string&) {
    return false;
}

} // namespace pulp::canvas

#endif // !PULP_HAS_SKIA
