// G2 (PAM native-editor port): the examples/webview-plugin example is the
// reference for embedding a native WKWebView editor across every plugin format
// AND standalone. These headless tests pin the two behaviors G2 fixes:
//
//   1. RETENTION — the WebViewPanel (and its JS heap / page state) is owned by
//      the PROCESSOR, not the view tree, so it survives editor close/reopen.
//      ViewBridge destroys the view tree on close (core/format/src/view_bridge.cpp);
//      a view-owned panel would be recreated every cycle. We assert pointer
//      identity across two open/close cycles.
//   2. HOST-AGNOSTIC ATTACH — the editor uses view::NativeViewHost, which falls
//      back from plugin_view_host() to window_host(). In standalone the root is
//      hosted by a WindowHost (core/format/src/standalone.cpp), so the OLD
//      hand-rolled plugin_view_host()-only attach silently no-oped and the
//      WebView never appeared. We assert the widget attaches under a
//      WindowHost-backed root and that headless snapshot capture is still routed.
//
// These run only when PULP_BUILD_WEBVIEW is ON (WebViewPanel::create is compiled
// only then) and SKIP when the native WebView backend is unavailable at runtime.

#include <catch2/catch_test_macros.hpp>

#include <pulp/format/view_bridge.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/native_view_host.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/window_host.hpp>

#include <functional>
#include <memory>

#include "../examples/webview-plugin/webview_plugin.hpp"

using pulp::examples::PulpWebViewPluginProcessor;

namespace {

// Minimal WindowHost that accepts native child views, standing in for the
// standalone host without a real window / AppKit run loop. Mirrors the standalone
// path: root hosted by a WindowHost, native child attached through it.
class FakeStandaloneWindowHost final : public pulp::view::WindowHost {
public:
    void show() override {}
    void hide() override {}
    bool is_visible() const override { return true; }
    void repaint() override {}
    void set_close_callback(std::function<void()>) override {}
    void run_event_loop() override {}

    bool attach_native_child_view(void* child, float x, float y, float w,
                                  float h) override {
        if (!child) return false;
        child_ = child;
        attach_count_++;
        (void)x; (void)y; (void)w; (void)h;
        return true;
    }
    bool set_native_child_view_bounds(void* child, float, float, float,
                                      float) override {
        return child == child_;
    }
    bool set_native_child_view_clip(void* child, bool, float, float, float,
                                    float) override {
        return child == child_;
    }
    void detach_native_child_view(void* child) override {
        if (child == child_) {
            child_ = nullptr;
            detach_count_++;
        }
    }

    int attach_count() const { return attach_count_; }
    int detach_count() const { return detach_count_; }

private:
    void* child_ = nullptr;
    int attach_count_ = 0;
    int detach_count_ = 0;
};

pulp::view::NativeViewHost* find_native_host(pulp::view::View& root) {
    for (size_t i = 0; i < root.child_count(); ++i) {
        if (auto* n = dynamic_cast<pulp::view::NativeViewHost*>(root.child_at(i)))
            return n;
    }
    return nullptr;
}

} // namespace

TEST_CASE("PulpWebViewPlugin reuses its processor-owned WebViewPanel across "
          "editor open/close cycles",
          "[view][webview][example][retention]") {
    pulp::state::StateStore store;
    PulpWebViewPluginProcessor processor;
    processor.define_parameters(store);

    pulp::format::ViewBridge bridge(processor, store);

    REQUIRE(bridge.open());
    bridge.notify_attached();
    pulp::view::WebViewPanel* first = processor.webview_panel_for_test();
    if (first == nullptr) {
        // No native WebView backend in this environment (headless CI without a
        // WKWebView). The retention contract can only be exercised with a real
        // panel; skip rather than pass vacuously.
        bridge.close();
        SKIP("Native WebView backend unavailable: WebViewPanel::create returned "
             "nullptr");
    }

    // Close the editor — ViewBridge destroys the view tree. The panel must NOT
    // be destroyed with it (it lives on the processor).
    bridge.close();
    REQUIRE_FALSE(bridge.is_open());
    REQUIRE(processor.webview_panel_for_test() == first);

    // Reopen — a fresh view tree, but the SAME WKWebView is reused (pointer
    // identity), so its JS heap and page state survive the round trip.
    REQUIRE(bridge.open());
    bridge.notify_attached();
    REQUIRE(processor.webview_panel_for_test() == first);

    // A second cycle keeps the invariant.
    bridge.close();
    REQUIRE(bridge.open());
    bridge.notify_attached();
    REQUIRE(processor.webview_panel_for_test() == first);

    bridge.close();
}

TEST_CASE("PulpWebViewPlugin editor attaches its WebView under a WindowHost-"
          "backed root",
          "[view][webview][example][standalone]") {
    PulpWebViewPluginProcessor processor;

    auto view = processor.create_view();
    REQUIRE(view != nullptr);
    auto* nvh = find_native_host(*view);
    REQUIRE(nvh != nullptr);

    if (nvh->native_child() == nullptr) {
        SKIP("Native WebView backend unavailable: no native child handle to "
             "embed");
    }

    // Size the root; flex sizes the NativeViewHost to fill it.
    view->set_bounds({0, 0, 720, 440});
    view->layout_children();

    // Standalone: the root is hosted by a WindowHost, NOT a PluginViewHost. The
    // OLD example checked plugin_view_host() only, so this attach never
    // happened. The NativeViewHost's window_host() fallback fixes it.
    FakeStandaloneWindowHost host;
    view->set_window_host(&host);
    nvh->update_native_layout();

    REQUIRE(nvh->is_native_attached());
    REQUIRE(host.attach_count() == 1);
    // Flex grew the child to fill the window.
    const auto frame = nvh->computed_child_frame();
    REQUIRE(frame.width == 720.0f);
    REQUIRE(frame.height == 440.0f);

    // Tearing the host down detaches the native child (but does not destroy the
    // processor-owned WebViewPanel).
    view->set_window_host(nullptr);
    REQUIRE_FALSE(nvh->is_native_attached());
    REQUIRE(host.detach_count() == 1);
    REQUIRE(processor.webview_panel_for_test() != nullptr);
}

TEST_CASE("PulpWebViewPlugin editor routes headless snapshot capture through "
          "its native overlay",
          "[view][webview][example][snapshot]") {
    PulpWebViewPluginProcessor processor;

    auto view = processor.create_view();
    REQUIRE(view != nullptr);
    auto* nvh = find_native_host(*view);
    REQUIRE(nvh != nullptr);

    if (nvh->native_child() == nullptr) {
        SKIP("Native WebView backend unavailable: no native child handle to "
             "snapshot");
    }

    // The subtree is flagged as containing a native overlay, so the smart-capture
    // path composites the WebView snapshot instead of rastering a blank region.
    REQUIRE(nvh->contains_native_overlay());

    // capture_native_overlay_png must delegate to the processor-owned panel's
    // snapshot_png() — the same route the smart-capture path uses. We assert the
    // route by equality rather than a literal non-empty check: WKWebView's
    // takeSnapshot yields ZERO bytes headlessly (no on-screen window, nothing
    // painted — verified: is_ready()==true yet snapshot is empty), so a
    // non-empty assertion on the real panel would be false here. The non-empty
    // FORWARDING guarantee (bytes in → bytes out) is pinned deterministically at
    // the widget level in test_native_view_host.cpp ("NativeViewHost forwards
    // headless capture to its snapshot callback"); this test pins that the
    // EXAMPLE wires that route to the processor-owned panel.
    auto* panel = processor.webview_panel_for_test();
    REQUIRE(panel != nullptr);
    const auto direct = panel->snapshot_png();
    const auto routed = nvh->capture_native_overlay_png(720, 440);
    REQUIRE(routed == direct);
}
