// The per-OS WebView backend answer, linked as the detector's provider by
// every composition that compiles WebView in. CMake links this archive whole,
// so its definition below is always the one the linker chooses.
//
//   * macOS / iOS   -> "wkwebview"   (WebKit.framework ships with the OS).
//   * Windows       -> "webview2"    when WebView2Loader.dll resolves at
//                                    runtime (platform/win/webview2_backend.cpp).
//   * Linux         -> "webkitgtk"   when libwebkit2gtk-4.{1,0} resolves
//                                    (platform/linux/webkitgtk_backend.cpp).
//   * Android       -> "chromium"    (system WebView).
//   * Other         -> "none".

#include <pulp/view/web_view.hpp>

#include <string>

namespace pulp::view::detail {

// Defined below for Apple, Android and other OSes, and by the per-OS probe
// translation unit on Windows and Linux.
std::string os_webview_backend();
bool os_webview_backend_available();

#if defined(__APPLE__)
std::string os_webview_backend() {
    return "wkwebview";
}
bool os_webview_backend_available() {
    return true;
}
#elif defined(__ANDROID__)
std::string os_webview_backend() {
    return "chromium";
}
bool os_webview_backend_available() {
    return true;
}
#elif !defined(_WIN32) && !defined(__linux__)
std::string os_webview_backend() {
    return "none";
}
bool os_webview_backend_available() {
    return false;
}
#endif

namespace {

constexpr WebViewBackendProvider kOsProvider{&os_webview_backend, &os_webview_backend_available};

} // namespace

} // namespace pulp::view::detail

// The strong definition that overrides pulp::view-platform's default (see
// web_view_backend.cpp). It is a link-time choice, so the detector answers
// correctly from the first call, including one made by another translation
// unit's static initializer.
extern "C" const pulp::view::detail::WebViewBackendProvider* pulp_view_webview_backend_provider() {
    return &pulp::view::detail::kOsProvider;
}
