// #299: Non-Apple view hosts route through host-registered
// callbacks (ScreenshotProvider, WindowHost::Factory,
// PluginViewHost::Factory). Without a registration, the APIs return
// explicit "unsupported" (empty vector / nullptr) and the has_*()
// probes let callers tell that state apart from genuine failure.
//
// The registration APIs are public on every platform; on Apple
// platforms the native impl takes precedence so installing a
// factory is a no-op — but the registration itself must remain
// safe.

#include <catch2/catch_test_macros.hpp>
#include <pulp/view/plugin_view_host.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/window_host.hpp>

using namespace pulp::view;

TEST_CASE("View host bridges: registration APIs are safe on all platforms",
          "[view][hosts][issue-299]") {
    clear_screenshot_provider();
    WindowHost::clear_factory();
    PluginViewHost::clear_factory();

    REQUIRE_FALSE(has_screenshot_provider());
    REQUIRE_FALSE(WindowHost::has_factory());
    REQUIRE_FALSE(PluginViewHost::has_factory());
}

#if !defined(__APPLE__)

namespace {

class StubWindowHost final : public WindowHost {
public:
    void show() override {}
    void hide() override {}
    bool is_visible() const override { return false; }
    void repaint() override {}
    void set_close_callback(std::function<void()>) override {}
    void run_event_loop() override {}
};

class StubPluginViewHost final : public PluginViewHost {
public:
    NativeViewHandle native_handle() override { return nullptr; }
    void attach_to_parent(NativeViewHandle) override {}
    void detach() override {}
    void repaint() override {}
    void set_size(uint32_t width, uint32_t height) override { size_ = {width, height}; }
    Size get_size() const override { return size_; }

private:
    Size size_{};
};

} // namespace

TEST_CASE("Non-Apple screenshot: no provider -> empty bytes + false file",
          "[view][hosts][issue-299]") {
    clear_screenshot_provider();
    View root;
    REQUIRE(render_to_png(root, 64, 64).empty());
    REQUIRE_FALSE(render_to_file(root, 64, 64, "/tmp/should-not-exist.png"));
}

TEST_CASE("Non-Apple screenshot: provider routes through and carries data",
          "[view][hosts][issue-299]") {
    int calls = 0;
    set_screenshot_provider([&](View&, uint32_t w, uint32_t h, float, ScreenshotBackend) {
        calls++;
        std::vector<uint8_t> fake(w * h, 0x42);
        return fake;
    });

    View root;
    auto bytes = render_to_png(root, 10, 10);
    REQUIRE(bytes.size() == 100);
    REQUIRE(bytes[0] == 0x42);
    REQUIRE(calls == 1);

    clear_screenshot_provider();
    REQUIRE_FALSE(has_screenshot_provider());
    REQUIRE(render_to_png(root, 10, 10).empty());
}

TEST_CASE("Non-Apple WindowHost::create: no factory -> nullptr",
          "[view][hosts][issue-299]") {
    WindowHost::clear_factory();
    View root;
    WindowOptions opts;
    auto win = WindowHost::create(root, opts);
    REQUIRE(win == nullptr);
}

TEST_CASE("Non-Apple PluginViewHost::create: no factory -> nullptr",
          "[view][hosts][issue-299]") {
    PluginViewHost::clear_factory();
    View root;
    PluginViewHost::Size size;
    REQUIRE(PluginViewHost::create(root, size) == nullptr);
    PluginViewHost::Options opts;
    REQUIRE(PluginViewHost::create(root, opts) == nullptr);
}

TEST_CASE("Non-Apple host factories propagate root host references",
          "[view][hosts][issue-651]") {
    WindowHost::clear_factory();
    PluginViewHost::clear_factory();

    View window_root;
    WindowHost::set_factory([](View&, const WindowOptions&) {
        return std::make_unique<StubWindowHost>();
    });
    auto window_host = WindowHost::create(window_root, WindowOptions{});
    REQUIRE(window_host != nullptr);
    REQUIRE(window_root.window_host() == window_host.get());

    View plugin_root;
    PluginViewHost::set_factory([](View&, const PluginViewHost::Options&) {
        return std::make_unique<StubPluginViewHost>();
    });
    auto plugin_host = PluginViewHost::create(plugin_root, PluginViewHost::Size{640, 360});
    REQUIRE(plugin_host != nullptr);
    REQUIRE(plugin_root.plugin_view_host() == plugin_host.get());

    auto child = reinterpret_cast<NativeViewHandle>(0x1);
    REQUIRE_FALSE(plugin_host->attach_native_child_view(child, 1.0f, 2.0f, 3.0f, 4.0f));
    REQUIRE_FALSE(plugin_host->set_native_child_view_bounds(child, 5.0f, 6.0f, 7.0f, 8.0f));
    plugin_host->detach_native_child_view(child);

    PluginViewHost::clear_factory();
    WindowHost::clear_factory();
}

// #313 Codex P2: providers must be invoked OUTSIDE the registration
// mutex so they can safely re-enter the bridge API (check state,
// take long actions, etc.). If the mutex were still held, calling
// set_screenshot_provider from inside a running provider would
// deadlock. This test would hang before the fix; it returns quickly
// after.
TEST_CASE("Non-Apple screenshot provider can re-enter the bridge API",
          "[view][hosts][issue-313]") {
    clear_screenshot_provider();

    bool reentered = false;
    set_screenshot_provider([&](View&, uint32_t, uint32_t, float, ScreenshotBackend) {
        // If the old code held g_provider_mu during this callback, the
        // has_screenshot_provider() call below would deadlock on a
        // recursive acquire (or, with std::mutex on some platforms,
        // UB). The fix copies the provider out before release.
        REQUIRE(has_screenshot_provider());
        reentered = true;
        return std::vector<uint8_t>{};
    });

    View root;
    (void)render_to_png(root, 1, 1);
    REQUIRE(reentered);
    clear_screenshot_provider();
}

#endif // !defined(__APPLE__)
