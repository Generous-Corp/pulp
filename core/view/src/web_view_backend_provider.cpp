// The per-OS WebView backend answer, installed as the detector's provider by
// every composition that compiles WebView in. CMake links this archive whole,
// so the registrar below survives dead-stripping even when nothing in the
// binary references it.
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

const WebViewBackendProvider kOsProvider{&os_webview_backend, &os_webview_backend_available};

struct Registrar {
    Registrar() {
        set_webview_backend_provider(&kOsProvider);
    }
};

const Registrar kRegistrar;

} // namespace

} // namespace pulp::view::detail
