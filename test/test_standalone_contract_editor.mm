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

#include <pulp/format/app_updates.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/format/standalone.hpp>
#include <pulp/format/standalone_panel.hpp>
#include <pulp/state/store.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <unistd.h>
#include <utility>
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

TEST_CASE("an editor's open_settings host action reaches the registered settings opener",
          "[standalone][editor-host][standalone-contract][settings]") {
    EditorEnvironment env;
    reset_contract_recording();
    const auto png = temp_png("pulp-standalone-settings-action");
    std::filesystem::remove(png);

    pulp::format::StandaloneApp app(create_contract_processor);
    int opened = 0;
    pulp::format::StandaloneApp* seen_app = nullptr;
    app.set_settings_window_opener([&](pulp::format::StandaloneApp& a) {
        ++opened;
        seen_app = &a;
        return true;
    });

    auto config = headless_config(png);
    bool requested = false;
    bool accepted = false;
    config.test_on_editor_frame = [&] {
        if (requested || contract_log().context == nullptr)
            return;
        requested = true;
        pulp::format::HostActionRequest request;
        request.action = pulp::format::HostAction::open_settings;
        accepted = contract_log().context->host_action(request);
    };
    app.set_config(config);
    REQUIRE(app.run_with_editor(false));

    REQUIRE(requested);
    REQUIRE(accepted);
    REQUIRE(opened == 1);
    REQUIRE(seen_app == &app);
    std::filesystem::remove(png);
}

TEST_CASE("open_settings_window uses the registered opener and reports its result",
          "[standalone][settings]") {
    pulp::format::StandaloneApp app(create_contract_processor);
    bool answer = false;
    int calls = 0;
    app.set_settings_window_opener([&](pulp::format::StandaloneApp&) {
        ++calls;
        return answer;
    });
    REQUIRE_FALSE(app.open_settings_window());
    answer = true;
    REQUIRE(app.open_settings_window());
    REQUIRE(calls == 2);
}

namespace {

struct ShortcutPanelRecord {
    int toggles = 0;
    void* window_while_running = nullptr;
    bool window_cleared = false;
};
ShortcutPanelRecord shortcut_record;

class ShortcutPanel final : public pulp::format::StandalonePanel {
  public:
    std::string menu_title() const override {
        return "Shortcut Panel";
    }
    char shortcut() const override {
        return 'k';
    }
    bool toggle() override {
        ++shortcut_record.toggles;
        return true;
    }
    bool is_visible() const override {
        return false;
    }
    void set_primary_window(void* native_window) override {
        if (native_window != nullptr)
            shortcut_record.window_while_running = native_window;
        else if (shortcut_record.window_while_running != nullptr)
            shortcut_record.window_cleared = true;
    }
    void drain_midi_into(pulp::midi::MidiBuffer&, int) noexcept override {}
    void shutdown() noexcept override {}
};

std::unique_ptr<pulp::format::StandalonePanel>
make_shortcut_panel(const pulp::format::StandalonePanelContext&) {
    return std::make_unique<ShortcutPanel>();
}

} // namespace

TEST_CASE("a standalone panel toggles on its Command shortcut from the editor window",
          "[standalone][editor-host][standalone-contract][standalone-panel]") {
    EditorEnvironment env;
    reset_contract_recording();
    pulp::format::detail::reset_standalone_panel_factories_for_testing();
    REQUIRE(pulp::format::detail::install_standalone_panel_factory(&make_shortcut_panel));
    shortcut_record = {};
    const auto png = temp_png("pulp-standalone-contract-panel");
    std::filesystem::remove(png);

    pulp::format::StandaloneApp app(create_contract_processor);
    auto config = headless_config(png);
    // Cmd+K toggles the panel twice; Shift+Cmd+K and a bare "k" do not, and
    // the bare "k" is the editor's.
    config.test_key_sequence = "cmd+k,cmd+shift+k,k,cmd+k";
    config.test_key_frame_delay = 2;
    app.set_config(config);
    const bool ran = app.run_with_editor(false);
    pulp::format::detail::reset_standalone_panel_factories_for_testing();
    std::filesystem::remove(png);
    REQUIRE(ran);

    CHECK(shortcut_record.toggles == 2);
    CHECK(contract_log().saw("offer:char"));
    // The panel was handed the editor window, and let go of it at close.
    CHECK(shortcut_record.window_while_running != nullptr);
    CHECK(shortcut_record.window_cleared);
}

TEST_CASE("a standalone contract window opens the plug-in's settings editor in its own window",
          "[standalone][editor-host][standalone-contract][settings]") {
    EditorEnvironment env;
    reset_contract_recording();
    contract_config().factory_supports_settings = true;
    const auto png = temp_png("pulp-standalone-plugin-settings");
    std::filesystem::remove(png);

    pulp::format::StandaloneApp app(create_contract_processor);
    auto config = headless_config(png);
    config.test_open_plugin_settings_frame = 1;
    int max_live = 0;
    config.test_on_editor_frame = [&] { max_live = std::max(max_live, contract_log().live); };
    app.set_config(config);
    REQUIRE(app.run_with_editor(false));

    // A settings-role editor was built, attached beside the main editor and
    // ticked with it, then both detached in order before the run ended.
    REQUIRE(contract_log().settings_created == 1);
    REQUIRE(max_live == 2);
    int attaches = 0;
    int detaches = 0;
    for (const auto& call : contract_log().calls) {
        attaches += call == "attach:cocoa_view";
        detaches += call == "detach";
    }
    REQUIRE(attaches == 2);
    REQUIRE(detaches == 2);
    REQUIRE_FALSE(contract_log().saw("invalidate"));
    REQUIRE(contract_log().live == 0);
    std::filesystem::remove(png);
}

TEST_CASE("a standalone contract window builds no settings editor the factory does not offer",
          "[standalone][editor-host][standalone-contract][settings]") {
    EditorEnvironment env;
    reset_contract_recording();
    const auto png = temp_png("pulp-standalone-no-plugin-settings");
    std::filesystem::remove(png);

    pulp::format::StandaloneApp app(create_contract_processor);
    auto config = headless_config(png);
    config.test_open_plugin_settings_frame = 1;
    app.set_config(config);
    REQUIRE(app.run_with_editor(false));
    REQUIRE(contract_log().settings_created == 0);
    REQUIRE(contract_log().live == 0);
    std::filesystem::remove(png);
}

namespace {

class CountingUpdateService final : public pulp::format::AppUpdateService {
  public:
    int checks = 0;
    pulp::format::AppUpdateStatus status() const override {
        pulp::format::AppUpdateStatus status;
        status.available = true;
        status.can_check_now = true;
        status.app_name = "Contract Standalone";
        return status;
    }
    bool check_for_updates() override {
        ++checks;
        return true;
    }
    bool set_automatic_checks(bool) override {
        return true;
    }
};

using MenuItems = std::vector<std::pair<std::string, std::function<void()>>>;

const std::function<void()>* find_item(const MenuItems& items, const std::string& title) {
    for (const auto& [name, action] : items)
        if (name == title)
            return &action;
    return nullptr;
}

} // namespace

TEST_CASE("the contract window offers Check for Updates and it drives the update service",
          "[standalone][editor-host][standalone-contract][updater]") {
    EditorEnvironment env;
    reset_contract_recording();
    const auto png = temp_png("pulp-standalone-contract-updater");
    std::filesystem::remove(png);

    auto service = std::make_shared<CountingUpdateService>();
    pulp::format::StandaloneApp app(create_contract_processor);
    auto config = headless_config(png);
    config.test_update_service = service;
    MenuItems items;
    bool saw_service = false;
    config.test_on_app_menu = [&](const MenuItems& menu) {
        items = menu;
        saw_service = pulp::format::app_update_status().available;
    };
    app.set_config(config);
    REQUIRE(app.run_with_editor(false));

    // The service was installed before the editor's menu was built.
    CHECK(saw_service);
    const auto* check = find_item(items, "Check for Updates\u2026");
    REQUIRE(check != nullptr);
    REQUIRE(static_cast<bool>(*check));
    // The item runs a check on whatever service is installed; reinstall it,
    // since the app removed it when it stopped.
    pulp::format::set_app_update_service(service);
    (*check)();
    CHECK(service->checks == 1);
    pulp::format::set_app_update_service(nullptr);
    // Quit is always the last item.
    REQUIRE_FALSE(items.empty());
    CHECK(items.back().first == "Quit Contract Standalone");
    std::filesystem::remove(png);
}

TEST_CASE("the contract window offers no Check for Updates when no updater is planned",
          "[standalone][editor-host][standalone-contract][updater]") {
    EditorEnvironment env;
    reset_contract_recording();
    const auto png = temp_png("pulp-standalone-contract-no-updater");
    std::filesystem::remove(png);

    // Headless, with no test service: the plan has no backend.
    pulp::format::StandaloneApp app(create_contract_processor);
    auto config = headless_config(png);
    MenuItems items;
    bool called = false;
    config.test_on_app_menu = [&](const MenuItems& menu) {
        items = menu;
        called = true;
    };
    app.set_config(config);
    REQUIRE(app.run_with_editor(false));
    REQUIRE(called);
    CHECK(find_item(items, "Check for Updates\u2026") == nullptr);
    CHECK_FALSE(pulp::format::app_update_status().available);
    std::filesystem::remove(png);
}
