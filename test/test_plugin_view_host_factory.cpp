// Non-Apple PluginViewHost factory registration + headless capture proof
// (#3329 Win/Linux parity). On Windows/Linux the platform host registers itself
// via register_platform_plugin_view_host(), so PluginViewHost::create() returns
// a real host without the caller installing a factory. Even with no display
// server (headless CI VM), the host degrades to a capture-only mode whose
// capture_back_buffer_png() still produces a valid frame via Skia raster.
//
// This test is built only on a non-Apple build that has a native platform host
// (PULP_TEST_HAS_PLATFORM_PLUGIN_VIEW_HOST, set from CMake). Apple has built-in
// NSView/UIView hosts and a different capture contract, so it is excluded.

#include <catch2/catch_test_macros.hpp>

#include <pulp/view/plugin_view_host.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>

using namespace pulp::view;

TEST_CASE("register_platform_plugin_view_host installs a factory",
          "[view][plugin-view-host][factory]") {
    register_platform_plugin_view_host();
    REQUIRE(PluginViewHost::has_factory());
}

TEST_CASE("PluginViewHost::create returns a native host with headless capture",
          "[view][plugin-view-host][create]") {
    View root;
    root.set_theme(Theme::dark());
    auto knob = std::make_unique<Knob>();
    knob->set_bounds({8, 8, 48, 48});
    knob->set_value(0.5f);
    root.add_child(std::move(knob));

    PluginViewHost::Options opts;
    opts.size = {64, 64};
    opts.use_gpu = true;  // host falls back to raster capture if GPU/display absent
    auto host = PluginViewHost::create(root, opts);
    REQUIRE(host != nullptr);

    auto size = host->get_size();
    REQUIRE(size.width == 64);
    REQUIRE(size.height == 64);

    // Headless capture must yield a valid PNG even with no display server /
    // no GPU — the deterministic frame the foreign-host embed smoke relies on.
    auto png = host->capture_back_buffer_png();
    REQUIRE_FALSE(png.empty());
    REQUIRE(png.size() > 8);
    REQUIRE(png[0] == 0x89);  // PNG magic
    REQUIRE(png[1] == 'P');
}
