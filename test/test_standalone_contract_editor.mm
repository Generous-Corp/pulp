// test_standalone_contract_editor.mm — the standalone app opening a
// processor's editor through the editor-host contract (headless).
//
// Every run is a screenshot-only launch: the window is never shown and no
// audio device is created (StandaloneConfig::screenshot_path with headless).
// The editor records every contract call and reports a fixed one-pixel PNG as
// its captured frame.

#include <catch2/catch_test_macros.hpp>

#import <AppKit/AppKit.h>

#include "support/recording_contract_editor.hpp"

#include <pulp/format/processor.hpp>
#include <pulp/format/standalone.hpp>
#include <pulp/state/store.hpp>

#include <cmath>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

using pulp::test_support::contract_config;
using pulp::test_support::contract_log;
using pulp::test_support::EditorEnvironment;
using pulp::test_support::RecordingContractFactory;
using pulp::test_support::reset_contract_recording;

namespace {

constexpr pulp::state::ParamID kGain = 1;

class ContractProcessor final : public pulp::format::Processor {
  public:
    pulp::format::PluginDescriptor descriptor() const override {
        pulp::format::PluginDescriptor d;
        d.name = "Contract Standalone";
        d.manufacturer = "Pulp";
        d.bundle_id = "dev.pulp.test.standalone-contract-editor";
        d.version = "1.0.0";
        d.input_buses = {{"Audio In", 2}};
        d.output_buses = {{"Audio Out", 2}};
        return d;
    }
    void define_parameters(pulp::state::StateStore& store) override {
        pulp::state::ParamInfo info;
        info.id = kGain;
        info.name = "Gain";
        info.range = {0.0f, 1.0f, 0.5f};
        store.add_parameter(info);
    }
    void prepare(const pulp::format::PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>& out, const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {
        out.clear();
    }
    pulp::format::EditorFactory* editor_factory() override {
        return &factory_;
    }

  private:
    RecordingContractFactory factory_;
};

std::unique_ptr<pulp::format::Processor> create_contract_processor() {
    return std::make_unique<ContractProcessor>();
}

std::string temp_png(const char* name) {
    const auto dir = std::filesystem::temp_directory_path();
    return (dir / (std::string(name) + "-" + std::to_string(::getpid()) + ".png")).string();
}

std::vector<unsigned char> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

pulp::format::StandaloneConfig headless_config(const std::string& png) {
    pulp::format::StandaloneConfig config;
    config.headless = true;
    config.screenshot_path = png;
    config.screenshot_frame_delay = 3;
    config.persist_settings = false;
    config.show_settings_tab = false;
    return config;
}

} // namespace

TEST_CASE("the standalone app hosts a contract editor headless and captures its frame",
          "[standalone][editor-host][standalone-contract]") {
    EditorEnvironment env;
    reset_contract_recording();
    const auto png = temp_png("pulp-standalone-contract");
    std::filesystem::remove(png);

    pulp::format::StandaloneApp app(create_contract_processor);
    auto config = headless_config(png);
    // Space is the standalone's transport key; "a" is the editor's.
    config.test_key_sequence = "space,a";
    config.test_key_frame_delay = 2;
    app.set_config(config);
    REQUIRE(app.run_with_editor(false));

    // A screenshot-only launch: no audio device was created.
    REQUIRE(app.audio_skipped_for_capture());

    // The editor ran in the window, was ticked, captured, then detached.
    REQUIRE(contract_log().saw("attach:cocoa_view"));
    REQUIRE(contract_log().idles > 0);
    REQUIRE(contract_log().saw("capture"));
    REQUIRE(contract_log().saw("detach"));
    REQUIRE_FALSE(contract_log().saw("invalidate"));
    REQUIRE(contract_log().live == 0);

    // The captured frame is what the editor produced.
    REQUIRE(read_file(png) ==
            std::vector<unsigned char>(pulp::test_support::recorded_frame_png().begin(),
                                       pulp::test_support::recorded_frame_png().end()));
    std::filesystem::remove(png);

    // Space was offered, forwarded, and reached the transport; "a" was the
    // editor's and stopped there.
    REQUIRE(contract_log().saw("offer:space"));
    REQUIRE(contract_log().saw("offer:char"));
    // The transport starts playing; one Space stopped it.
    REQUIRE_FALSE(app.test_input_host().commanded_playing());
}

TEST_CASE("a standalone contract editor follows the window's size and scale",
          "[standalone][editor-host][standalone-contract]") {
    EditorEnvironment env;
    reset_contract_recording();
    const auto png = temp_png("pulp-standalone-contract-size");
    pulp::format::StandaloneApp app(create_contract_processor);
    app.set_config(headless_config(png));
    REQUIRE(app.run_with_editor(false));
    // The window took the editor's own size and passed its backing scale.
    REQUIRE(contract_log().last_scale >= 1.0);
    // Without a Space the transport keeps playing (the control for the
    // Space test above).
    REQUIRE(app.test_input_host().commanded_playing());
    std::filesystem::remove(png);
}

TEST_CASE("a headless standalone contract editor without frame capture fails closed",
          "[standalone][editor-host][standalone-contract]") {
    EditorEnvironment env;
    reset_contract_recording();
    contract_config().captures_frames = false;
    const auto png = temp_png("pulp-standalone-contract-nocapture");
    std::filesystem::remove(png);

    pulp::format::StandaloneApp app(create_contract_processor);
    app.set_config(headless_config(png));
    REQUIRE_FALSE(app.run_with_editor(false));
    REQUIRE_FALSE(std::filesystem::exists(png));
    // Closed in order even so.
    REQUIRE(contract_log().saw("detach"));
    REQUIRE(contract_log().live == 0);
    reset_contract_recording();
}

TEST_CASE("a standalone contract editor that will not build or attach fails the run",
          "[standalone][editor-host][standalone-contract]") {
    EditorEnvironment env;
    const auto png = temp_png("pulp-standalone-contract-refused");
    {
        reset_contract_recording();
        contract_config().factory_builds_editor = false;
        pulp::format::StandaloneApp app(create_contract_processor);
        app.set_config(headless_config(png));
        REQUIRE_FALSE(app.run_with_editor(false));
    }
    {
        reset_contract_recording();
        contract_config().attach_succeeds = false;
        pulp::format::StandaloneApp app(create_contract_processor);
        app.set_config(headless_config(png));
        REQUIRE_FALSE(app.run_with_editor(false));
        REQUIRE(contract_log().live == 0);
    }
    {
        reset_contract_recording();
        pulp::format::StandaloneApp app(create_contract_processor);
        auto config = headless_config(png);
        config.test_key_sequence = "not-a-key";
        app.set_config(config);
        REQUIRE_FALSE(app.run_with_editor(false));
    }
    reset_contract_recording();
    REQUIRE_FALSE(std::filesystem::exists(png));
}

namespace {

// Runs the standalone contract window headless, hands the native window to
// `on_window`, and calls `on_frame` once from the window's loop.
bool run_window(double screen_width, double screen_height, std::function<void(NSWindow*)> on_window,
                std::function<void(NSWindow*)> on_frame) {
    const auto png = temp_png("pulp-standalone-contract-window");
    pulp::format::StandaloneApp app(create_contract_processor);
    auto config = headless_config(png);
    config.test_screen_visible_width = screen_width;
    config.test_screen_visible_height = screen_height;
    NSWindow* window = nil;
    bool framed = false;
    config.test_on_editor_window = [&](void* native) {
        window = (NSWindow*)native;
        if (on_window)
            on_window(window);
    };
    config.test_on_editor_frame = [&] {
        if (!framed && window != nil && on_frame) {
            framed = true;
            on_frame(window);
        }
    };
    app.set_config(config);
    const bool ok = app.run_with_editor(false);
    std::filesystem::remove(png);
    return ok;
}

NSSize content_after_drag(NSWindow* window, double frame_width, double frame_height) {
    const NSSize frame = [window.delegate windowWillResize:window
                                                    toSize:NSMakeSize(frame_width, frame_height)];
    return [window contentRectForFrameRect:NSMakeRect(0, 0, frame.width, frame.height)].size;
}

} // namespace

TEST_CASE("a standalone contract window holds the editor's aspect and limits while it is dragged",
          "[standalone][editor-host][standalone-contract][resize-hints]") {
    EditorEnvironment env;
    reset_contract_recording();
    contract_config().identity_adjust = true; // the window alone enforces the hints
    bool ran = false;
    REQUIRE(run_window(0, 0, nullptr, [&](NSWindow* window) {
        ran = true;
        for (const NSSize drag :
             {NSMakeSize(1000, 400), NSMakeSize(500, 900), NSMakeSize(640, 520)}) {
            const NSSize content = content_after_drag(window, drag.width, drag.height);
            // 4:3 to within a pixel, inside 200x150 .. 800x600.
            REQUIRE(std::abs(content.width - content.height * 4.0 / 3.0) <= 1.0);
            REQUIRE(content.width >= 200);
            REQUIRE(content.width <= 800);
            REQUIRE(content.height <= 600);
        }
        // A programmatic resize (no drag, so no delegate call) is held to the
        // same aspect once its sizes settle.
        [window setContentSize:NSMakeSize(1000, 400)];
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.4]];
        const NSSize set = window.contentView.frame.size;
        REQUIRE(std::abs(set.width - set.height * 4.0 / 3.0) <= 1.0);
        REQUIRE(set.width <= 800);
        const NSSize big = content_after_drag(window, 3000, 3000);
        REQUIRE(big.width == 800);
        REQUIRE(big.height == 600);
    }));
    REQUIRE(ran);
}

TEST_CASE(
    "a standalone contract window leaves an editor with default hints at the size it asked for",
    "[standalone][editor-host][standalone-contract][resize-hints]") {
    EditorEnvironment env;
    reset_contract_recording();
    contract_config().identity_adjust = true;
    contract_config().default_hints = true;
    bool ran = false;
    REQUIRE(run_window(0, 0, nullptr, [&](NSWindow* window) {
        ran = true;
        REQUIRE_FALSE((window.styleMask & NSWindowStyleMaskResizable) != 0);
        // Control for the test above: a fixed-size editor is not snapped.
        const NSSize content = content_after_drag(window, 1000, 400);
        REQUIRE(content.width == 1000);
    }));
    REQUIRE(ran);
}

TEST_CASE("a standalone contract window opens inside a small screen at the editor's aspect",
          "[standalone][editor-host][standalone-contract][resize-hints]") {
    EditorEnvironment env;
    reset_contract_recording();
    NSSize opened{};
    REQUIRE(run_window(300, 250, nullptr, [&](NSWindow* window) {
        opened = window.contentView.frame.size;
        REQUIRE(window.frame.size.width <= 300);
        REQUIRE(window.frame.size.height <= 250);
    }));
    REQUIRE(opened.width < 400);
    REQUIRE(opened.width >= 200);
    REQUIRE(std::abs(opened.width - opened.height * 4.0 / 3.0) <= 1.0);

    // Control: with no injected screen the editor opens at its own size.
    reset_contract_recording();
    NSSize full{};
    REQUIRE(
        run_window(0, 0, nullptr, [&](NSWindow* window) { full = window.contentView.frame.size; }));
    REQUIRE(full.width == 400);
    REQUIRE(full.height == 300);
}

TEST_CASE("standalone acknowledges editor requests and rejects refused sizes",
          "[standalone][editor-host][resize-ack]") {
    EditorEnvironment env;
    reset_contract_recording();
    contract_config().default_hints = true;
    pulp::test_support::ScopedManualSnapTimer settle;
    bool ran = false;
    REQUIRE(run_window(0, 0, nullptr, [&](NSWindow* window) {
        ran = true;
        REQUIRE(contract_log().context->request_resize({640, 480, 1.0}));
        REQUIRE(contract_log().saw("set_size:640x480"));
        REQUIRE(window.contentView.frame.size.width == 640);
        REQUIRE(window.contentView.frame.size.height == 480);
        REQUIRE_FALSE(settle.armed());
        // RecordingContractEditor refuses sizes below 200x150.
        REQUIRE_FALSE(contract_log().context->request_resize({100, 100, 1.0}));
        REQUIRE(window.contentView.frame.size.width == 640);
        REQUIRE(window.contentView.frame.size.height == 480);
    }));
    REQUIRE(ran);
}

TEST_CASE("standalone editor requests supersede a pending host correction",
          "[standalone][editor-host][resize-ack]") {
    EditorEnvironment env;
    reset_contract_recording();
    contract_config().identity_adjust = true;
    pulp::test_support::ScopedManualSnapTimer settle;
    bool ran = false;
    REQUIRE(run_window(0, 0, nullptr, [&](NSWindow* window) {
        ran = true;
        [window setContentSize:NSMakeSize(900, 500)];
        REQUIRE(settle.armed());
        REQUIRE(contract_log().context->request_resize({640, 480, 1.0}));
        REQUIRE_FALSE(settle.armed());
        settle.fire();
        REQUIRE(window.contentView.frame.size.width == 640);
        REQUIRE(window.contentView.frame.size.height == 480);
        REQUIRE(contract_log().saw("set_size:640x480"));
    }));
    REQUIRE(ran);
}
