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

TEST_CASE("Non-Apple screenshot: no provider → empty bytes + false file",
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

TEST_CASE("Non-Apple WindowHost::create: no factory → nullptr",
          "[view][hosts][issue-299]") {
    WindowHost::clear_factory();
    View root;
    WindowOptions opts;
    auto win = WindowHost::create(root, opts);
    REQUIRE(win == nullptr);
}

TEST_CASE("Non-Apple PluginViewHost::create: no factory → nullptr",
          "[view][hosts][issue-299]") {
    PluginViewHost::clear_factory();
    View root;
    PluginViewHost::Size size;
    REQUIRE(PluginViewHost::create(root, size) == nullptr);
    PluginViewHost::Options opts;
    REQUIRE(PluginViewHost::create(root, opts) == nullptr);
}

#endif // !defined(__APPLE__)
