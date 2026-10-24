// WebView backend detector. This is its only definition: every view
// composition reaches it through pulp::view-platform, so a binary that links
// more than one composition still has one answer.
//
// A composition without WebView support answers "none", which lets callers
// tell "this build excludes WebView" from "this OS has no usable WebView"
// without constructing a WebViewPanel. pulp::view-webview installs a provider
// carrying the per-OS answer (web_view_backend_provider.cpp).

#include <pulp/view/web_view.hpp>

#include <atomic>
#include <string>

namespace pulp::view {

namespace {

// Constant-initialized, so a provider installed from another translation
// unit's static initializer is never overwritten by this one's.
std::atomic<const detail::WebViewBackendProvider*> g_provider{nullptr};

} // namespace

const detail::WebViewBackendProvider*
detail::set_webview_backend_provider(const WebViewBackendProvider* provider) {
    return g_provider.exchange(provider, std::memory_order_acq_rel);
}

std::string detect_webview_backend() {
    const auto* provider = g_provider.load(std::memory_order_acquire);
    return provider && provider->detect ? provider->detect() : std::string("none");
}

bool webview_backend_available() {
    const auto* provider = g_provider.load(std::memory_order_acquire);
    return provider && provider->available ? provider->available() : false;
}

} // namespace pulp::view
