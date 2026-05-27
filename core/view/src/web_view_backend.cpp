// Cross-platform WebView backend detector — always compiled into
// pulp-view-core regardless of PULP_BUILD_WEBVIEW so callers can
// distinguish "this build doesn't include WebView support" from
// "this OS has no usable WebView backend installed" without first
// trying to instantiate a WebViewPanel.
//
// The detection layer is split per-platform so each OS surfaces a
// canonical identifier:
//
//   * macOS / iOS   → "wkwebview"   (always — WebKit.framework ships
//                                    with the OS).
//   * Windows       → "webview2"    (when WebView2Loader.dll resolves
//                                    at runtime; "none" otherwise).
//                     Implemented in platform/win/webview2_backend.cpp.
//   * Linux         → "webkitgtk"   (when libwebkit2gtk-4.{1,0}.so
//                                    resolves; "none" otherwise).
//                     Implemented in platform/linux/webkitgtk_backend.cpp.
//   * Android       → "chromium"    (always — system WebView).
//   * Other         → "none".
//
// On Windows + Linux the per-platform TUs own the symbol so we get the
// runtime probe. On every other supported OS this file owns it directly
// so the build still links.

#include <pulp/view/web_view.hpp>

#if defined(__APPLE__)

namespace pulp::view {
// When PULP_BUILD_WEBVIEW is OFF the WebView implementation file
// (web_view.cpp) is excluded from the build, so reporting "wkwebview"
// would lie about a build that physically cannot construct a
// WebViewPanel. Honor the documented "none if disabled" contract
// (Codex PR #3016 P2).
#if defined(PULP_BUILD_WEBVIEW) && !PULP_BUILD_WEBVIEW
std::string detect_webview_backend() { return "none"; }
bool webview_backend_available() { return false; }
#else
std::string detect_webview_backend() { return "wkwebview"; }
bool webview_backend_available() { return true; }
#endif
} // namespace pulp::view

#elif defined(__ANDROID__)

namespace pulp::view {
#if defined(PULP_BUILD_WEBVIEW) && !PULP_BUILD_WEBVIEW
std::string detect_webview_backend() { return "none"; }
bool webview_backend_available() { return false; }
#else
std::string detect_webview_backend() { return "chromium"; }
bool webview_backend_available() { return true; }
#endif
} // namespace pulp::view

#elif !defined(_WIN32) && !defined(__linux__)

// Catch-all fallback for OSes that are neither Apple, nor Android, nor
// Windows, nor Linux (e.g. FreeBSD, OpenBSD, future ports). The
// Windows + Linux symbols live in the per-platform TUs alongside their
// runtime probes; we deliberately don't define them here so the build
// hard-fails if the per-platform TU is missing from CMakeLists.txt.
namespace pulp::view {
std::string detect_webview_backend() { return "none"; }
bool webview_backend_available() { return false; }
} // namespace pulp::view

#endif
