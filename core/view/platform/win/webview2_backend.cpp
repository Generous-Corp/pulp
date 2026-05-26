// Windows WebView2 backend probe for pulp::view::WebViewPanel.
//
// Pulp's WebViewPanel is implemented on top of CHOC's `choc::ui::WebView`,
// which on Windows talks to Microsoft Edge WebView2 (via the
// `Microsoft.Web.WebView2.Loader` COM shim distributed with the Edge
// runtime). CHOC handles the heavy lifting — this translation unit
// exists so the gap-doc audit can confirm:
//
//   1. The WebView2 runtime is locatable at process start (so the build
//      isn't silently emitting CHOC code that will fail to instantiate),
//      and
//   2. `pulp::view::detect_webview_backend()` returns a stable identifier
//      callers can use to surface a clear error if the WebView2 runtime
//      is missing on the user's machine.
//
// We deliberately use runtime LoadLibrary against `WebView2Loader.dll`
// rather than linking against the WebView2 SDK at build time:
//
//   * The build must succeed on Windows boxes that haven't installed
//     the WebView2 Evergreen runtime, including CI agents that only
//     compile core/view as part of the cross-platform smoke.
//   * The WebView2 loader DLL is distributed by Microsoft's Edge update
//     channel, not as part of the Windows SDK, so vendor-pinned headers
//     would add a needless build dependency.
//
// What's *not* in this file (deferred follow-up):
//   * Full WebView2 environment setup (`CreateCoreWebView2Environment`,
//     `ICoreWebView2Controller2`, the per-user data folder convention).
//     CHOC already takes that path inside `choc::ui::WebView`; this
//     module just confirms the loader DLL is reachable.
//   * Custom-scheme / `WebResourceRequested` interception. CHOC's
//     `customSchemeURI` + `fetchResource` already cover the
//     `ResourceProvider`-style intercept use case for the gap-doc row.

#include <pulp/view/web_view.hpp>

#if defined(_WIN32)

#include <pulp/runtime/dynamic_library.hpp>

#include <string>

namespace pulp::view {

namespace {

// Probe-once cache so repeated `detect_webview_backend()` calls don't
// reopen the loader DLL.
struct WebView2Probe {
    bool available = false;

    WebView2Probe() {
        pulp::runtime::DynamicLibrary loader;
        if (loader.open("WebView2Loader.dll")) {
            available = true;
            return;
        }
        // Older / x86 deployments sometimes ship the loader under the
        // architecture-suffixed name; try that as a fallback so we don't
        // false-negative on packaged WebView2 runtimes.
        if (loader.open("WebView2Loader.dll.x64") ||
            loader.open("WebView2Loader.dll.x86")) {
            available = true;
        }
    }
};

const WebView2Probe& probe() {
    static const WebView2Probe instance;
    return instance;
}

} // namespace

std::string detect_webview_backend() {
    return probe().available ? "webview2" : "none";
}

bool webview_backend_available() {
    return probe().available;
}

} // namespace pulp::view

#endif // _WIN32
