// WebView backend detector. This is its only definition: every view
// composition reaches it through pulp::view-platform, so a binary that links
// more than one composition still has one answer.
//
// A composition without WebView support answers "none", which lets callers
// tell "this build excludes WebView" from "this OS has no usable WebView"
// without constructing a WebViewPanel. pulp::view-webview supplies the per-OS
// answer (web_view_backend_provider.cpp) as a link-time definition, so the
// answer does not depend on static-initialization order: a consumer's own
// static initializer that calls the detector already sees it.

#include <pulp/view/web_view.hpp>

#include <atomic>
#include <string>

// The linked provider. pulp::view-webview defines it (strongly); this
// translation unit's default, which answers "no provider", is used only when
// no composition links WebView in. MSVC has no weak definitions, so the
// default is named and selected with /alternatename instead. C linkage keeps
// the symbol name undecorated on every 64-bit target.
extern "C" const pulp::view::detail::WebViewBackendProvider* pulp_view_webview_backend_provider();

#if defined(_MSC_VER)
extern "C" const pulp::view::detail::WebViewBackendProvider*
pulp_view_webview_backend_provider_none() {
    return nullptr;
}
#pragma comment(                                                                                   \
    linker,                                                                                        \
    "/alternatename:pulp_view_webview_backend_provider=pulp_view_webview_backend_provider_none")
#else
extern "C" __attribute__((weak))
const pulp::view::detail::WebViewBackendProvider* pulp_view_webview_backend_provider() {
    return nullptr;
}
#endif

namespace pulp::view {

namespace {

// An override installed by set_webview_backend_provider (tests). Constant-
// initialized, so it is valid before any dynamic initializer runs.
std::atomic<const detail::WebViewBackendProvider*> g_override{nullptr};

const detail::WebViewBackendProvider* active_provider() {
    if (const auto* provider = g_override.load(std::memory_order_acquire))
        return provider;
    return pulp_view_webview_backend_provider();
}

} // namespace

const detail::WebViewBackendProvider*
detail::set_webview_backend_provider(const WebViewBackendProvider* provider) {
    return g_override.exchange(provider, std::memory_order_acq_rel);
}

std::string detect_webview_backend() {
    const auto* provider = active_provider();
    return provider && provider->detect ? provider->detect() : std::string("none");
}

bool webview_backend_available() {
    const auto* provider = active_provider();
    return provider && provider->available ? provider->available() : false;
}

} // namespace pulp::view
