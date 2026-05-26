// Linux WebKitGTK backend probe for pulp::view::WebViewPanel.
//
// Pulp's WebViewPanel is implemented on top of CHOC's `choc::ui::WebView`,
// which on Linux uses WebKitGTK (`libwebkit2gtk-4.1.so.0`) plus GTK 3.
// CHOC handles instantiation; this translation unit exists so:
//
//   1. The gap-doc audit can confirm WebKitGTK is actually resolvable at
//      runtime on the user's distro (Ubuntu 22.04+, Fedora 38+, etc.
//      ship 4.1 by default; some long-tail distros still carry only the
//      legacy 4.0 ABI).
//   2. `pulp::view::detect_webview_backend()` returns a stable identifier
//      callers can use to surface a clear "WebKitGTK 4.1 not installed"
//      message instead of crashing inside CHOC.
//
// Posture matches `core/events/platform/linux/avahi_backend.cpp` — we
// use `pulp::runtime::DynamicLibrary` to dlopen the SONAMEs at process
// start. The build never links WebKitGTK directly here (the link
// happens inside `pulp-view-core` when `PULP_BUILD_WEBVIEW=ON`); this
// file only probes availability.

#include <pulp/view/web_view.hpp>

#if defined(__linux__) && !defined(__ANDROID__)

#include <pulp/runtime/dynamic_library.hpp>

#include <string>

namespace pulp::view {

namespace {

// Probe-once cache. We try the modern 4.1 ABI first (current stable),
// then fall back to the legacy 4.0 ABI for older LTS distros. Either
// works behind CHOC's WebKitGTK adapter — the gap-doc row only cares
// that *some* WebKitGTK is reachable.
struct WebKitGtkProbe {
    bool available = false;
    const char* soname = nullptr;

    WebKitGtkProbe() {
        static const char* kCandidates[] = {
            "libwebkit2gtk-4.1.so.0",
            "libwebkit2gtk-4.1.so",
            "libwebkit2gtk-4.0.so.37",
            "libwebkit2gtk-4.0.so",
        };
        for (const char* candidate : kCandidates) {
            pulp::runtime::DynamicLibrary lib;
            if (lib.open(candidate)) {
                available = true;
                soname = candidate;
                return;
            }
        }
    }
};

const WebKitGtkProbe& probe() {
    static const WebKitGtkProbe instance;
    return instance;
}

} // namespace

std::string detect_webview_backend() {
    return probe().available ? "webkitgtk" : "none";
}

bool webview_backend_available() {
    return probe().available;
}

} // namespace pulp::view

#endif // __linux__ && !__ANDROID__
