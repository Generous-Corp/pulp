#import <Cocoa/Cocoa.h>

#include <catch2/catch_test_macros.hpp>
#include <pulp/format/editor_host.hpp>
#include <pulp/state/store.hpp>
#include <pulp/vellum/rmlui_editor.hpp>

#include <cstdint>

#include <filesystem>
#include <fstream>
#include <memory>

namespace {
class Context final : public pulp::format::EditorContext {
  public:
    explicit Context(pulp::format::ParamPort& params) : params_(params) {}
    pulp::format::ParamPort& params() override {
        return params_;
    }
    pulp::format::VisualPort* visuals() override {
        return nullptr;
    }
    bool request_resize(const pulp::format::EditorSize&) override {
        return false;
    }
    bool host_action(const pulp::format::HostActionRequest&) override {
        return false;
    }
    pulp::format::EditorHostKind host_kind() const override {
        return pulp::format::EditorHostKind::plugin;
    }

  private:
    pulp::format::ParamPort& params_;
};


struct Fixture {
    pulp::state::StateStore store;
    // The port lists the parameters that exist when it is made, so it is
    // created after the parameter is added.
    std::unique_ptr<pulp::format::StateStoreParamPort> port;
    std::filesystem::path root;

    Fixture() : root(std::filesystem::temp_directory_path() / "pulp-rmlui-editor-test") {
        pulp::state::ParamInfo info;
        info.id = 1;
        info.name = "Gain";
        info.range = {0.0f, 1.0f, 0.0f};
        store.add_parameter(info);
        port = std::make_unique<pulp::format::StateStoreParamPort>(store);
        std::filesystem::create_directories(root);
        std::ofstream(root / "editor.rml")
            << "<rml><head><link type='text/rcss' href='editor.rcss'/></head>"
               "<body data-model='params'><div id='gain' data-class-on='p1 > 0.5' "
               "data-event-click='p1 = 1 - p1'/></body></rml>";
        std::ofstream(root / "editor.rcss")
            << "#gain { display:block; width:20px; height:20px; background-color:#ff0000; }"
               "#gain.on { background-color:#0000ff; }";
    }
};
// The RGB of one pixel of the editor's premultiplied RGBA frame.
std::uint32_t rgb_at(const vellum::rmlui::SoftRenderer& soft, std::uint32_t x, std::uint32_t y) {
    const auto& px = soft.pixels();
    const std::size_t i = (static_cast<std::size_t>(y) * soft.width() + x) * 4;
    return (std::uint32_t(px[i]) << 16) | (std::uint32_t(px[i + 1]) << 8) | px[i + 2];
}
} // namespace

TEST_CASE("RmlUi package editor attaches, repaints and writes a StateStore parameter",
          "[pulp-vellum][rmlui]") {
    @autoreleasepool {
        Fixture fixture;
        Context context(*fixture.port);
        pulp::vellum::rmlui::PackageAssets assets{(fixture.root / "editor.rml").string(),
                                                  (fixture.root / "editor.rcss").string(),
                                                  {fixture.root.string()}};
        auto editor =
            std::make_unique<pulp::vellum::rmlui::Editor<pulp::vellum::rmlui::PackageAssets>>(
                context, assets);
        REQUIRE(editor);
        NSView* parent = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 32, 32)];
        REQUIRE(
            editor->attach({pulp::format::NativeParentKind::cocoa_view, (__bridge void*)parent}));
        editor->idle();
        REQUIRE(parent.subviews.count == 1);
        REQUIRE(editor->set_size({32, 32, 1.0}));
        REQUIRE(editor->renderer() != nullptr);
        editor->idle();
        CHECK(rgb_at(*editor->renderer(), 5, 5) == 0xff0000u);
        fixture.store.set_value(1, 1.0f);
        fixture.store.pump_listeners();
        editor->idle();
        CHECK(fixture.store.get_value(1) == 1.0f);
        CHECK(rgb_at(*editor->renderer(), 5, 5) == 0x0000ffu);
        REQUIRE(editor->process_mouse_move(5, 5) == vellum::rmlui::Status::ok);
        REQUIRE(editor->process_mouse_button_down(0) == vellum::rmlui::Status::ok);
        REQUIRE(editor->process_mouse_button_up(0) == vellum::rmlui::Status::ok);
        editor->idle();
        CHECK(fixture.store.get_value(1) == 0.0f);
        CHECK(rgb_at(*editor->renderer(), 5, 5) == 0xff0000u);
        editor->invalidate();
    }
}
