// The lite editor stack's plug-in view host. This binary compiles the macOS
// plug-in host sources itself with PULP_VIEW_LITE_HOST=1, the way a lite
// plug-in's ObjC cluster does, so PluginViewHost::create here is the lite
// factory: there is no GPU host to construct.

#include <catch2/catch_test_macros.hpp>

#include <pulp/view/plugin_view_host.hpp>
#include <pulp/view/view.hpp>

#import <AppKit/AppKit.h>

#include <string>

using namespace pulp::view;

namespace {

PluginViewHost::Options options(bool use_gpu) {
    PluginViewHost::Options o;
    o.size = {240, 120};
    o.use_gpu = use_gpu;
    return o;
}

} // namespace

TEST_CASE("a lite editor asked for the GPU host opens the CoreGraphics host and says so once",
          "[view][plugin-host][lite-editor]") {
    [NSApplication sharedApplication];
    View root;
    root.set_requires_gpu_host(true);
    const auto before = detail::lite_host_gpu_request_count();

    auto first = PluginViewHost::create(root, options(true));
    REQUIRE(first);
    CHECK_FALSE(first->is_gpu_backed());
    // The host came from this binary's lite factory: its view is the
    // suffixed CoreGraphics class compiled here, not a library copy.
    id view = (__bridge id)first->native_handle();
    REQUIRE(view != nil);
    CHECK(std::string([NSStringFromClass([view class]) UTF8String]) ==
          "PulpPluginView_LiteHostTest");
    CHECK(detail::lite_host_gpu_request_count() == before + 1);

    auto second = PluginViewHost::create(root, options(true));
    REQUIRE(second);
    CHECK_FALSE(second->is_gpu_backed());
    CHECK(detail::lite_host_gpu_request_count() == before + 2);

    // Only the first request in the process logs.
    CHECK_FALSE(detail::note_lite_host_gpu_request());
}

TEST_CASE("a lite editor binary has no GPU plug-in view class",
          "[view][plugin-host][lite-editor]") {
    // Control: the CoreGraphics view class compiled into this binary exists
    // under this binary's suffix, so the lookup below can find classes.
    CHECK(NSClassFromString(@"PulpPluginView_LiteHostTest") != nil);
    CHECK(NSClassFromString(@"PulpGpuPluginView_LiteHostTest") == nil);
}

TEST_CASE("a lite editor that never asks for the GPU host records nothing",
          "[view][plugin-host][lite-editor]") {
    [NSApplication sharedApplication];
    View root;
    const auto before = detail::lite_host_gpu_request_count();
    auto host = PluginViewHost::create(root, options(false));
    REQUIRE(host);
    CHECK_FALSE(host->is_gpu_backed());
    CHECK(detail::lite_host_gpu_request_count() == before);
}
