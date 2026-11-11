#pragma once

// The Vellum SDK's mirrored view and canvas units, in whichever spelling the
// installed SDK carries: <pulp/...> headers in namespace pulp (SDK 0.1.10 and
// older) or <vellum/...> headers in namespace vellum (later SDKs). The
// Vellum-side UI includes this header and names the units through `units::`,
// so it builds against either.
//
// Remove once the pinned Vellum SDK is >= 0.2.0 (renamed mirror only), and
// spell the units vellum:: directly.

#if __has_include(<vellum/canvas/canvas.hpp>)
#include <vellum/canvas/canvas.hpp>
#include <vellum/view/screenshot.hpp>
#include <vellum/view/text_editor.hpp>
#include <vellum/view/view.hpp>
#include <vellum/view/widgets.hpp>
namespace slice_vellum_ui {
namespace units = ::vellum;
inline constexpr bool units_spelled_vellum = true;
} // namespace slice_vellum_ui
#else
#include <pulp/canvas/canvas.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>
namespace slice_vellum_ui {
namespace units = ::pulp;
inline constexpr bool units_spelled_vellum = false;
} // namespace slice_vellum_ui
#endif
