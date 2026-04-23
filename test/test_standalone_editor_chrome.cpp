#include <catch2/catch_test_macros.hpp>
#include <pulp/format/detail/standalone_editor_chrome.hpp>

#include <vector>

using namespace pulp::format;
using namespace pulp::format::detail;
using namespace pulp::view;

namespace {

class StubWindowHost final : public WindowHost {
public:
    ContentSize content_size_{640, 360};
    std::function<void()> idle_callback_;
    ResizeCallback resize_callback_;
    int repaint_calls_ = 0;

    void show() override {}
    void hide() override {}
    bool is_visible() const override { return false; }
    void repaint() override { ++repaint_calls_; }
    ContentSize get_content_size() const override { return content_size_; }
    void set_idle_callback(std::function<void()> cb) override {
        idle_callback_ = std::move(cb);
    }
    void set_resize_callback(ResizeCallback cb) override {
        resize_callback_ = std::move(cb);
    }
    void set_close_callback(std::function<void()>) override {}
    void run_event_loop() override {}
};

class DefaultWindowHost final : public WindowHost {
public:
    void show() override {}
    void hide() override {}
    bool is_visible() const override { return false; }
    void repaint() override {}
    void set_close_callback(std::function<void()>) override {}
    void run_event_loop() override {}
};

struct StubScriptedUi {
    int poll_calls_ = 0;
    std::function<void()> repaint_callback_;

    void poll() { ++poll_calls_; }
    void set_repaint_callback(std::function<void()> cb) {
        repaint_callback_ = std::move(cb);
    }
};

struct StubSettingsPoller {
    int poll_calls_ = 0;
    void poll() { ++poll_calls_; }
};

struct StubBridge {
    std::vector<WindowHost::ContentSize> resize_calls_;

    void resize(uint32_t width, uint32_t height) {
        resize_calls_.push_back({width, height});
    }
};

} // namespace

TEST_CASE("Standalone settings callbacks rebind after successful apply",
          "[standalone][chrome]") {
    SettingsPanel panel;
    bool apply_called = false;
    bool rebind_called = false;
    bool signal_changed = false;
    bool file_loaded = false;
    bool transport_called = false;

    auto callbacks = make_standalone_settings_callbacks(
        panel,
        StandaloneSettingsActions{
            .apply_config = [&](const StandaloneConfig&) {
                apply_called = true;
                return true;
            },
            .rebind_after_apply = [&](SettingsPanel& rebound_panel) {
                rebind_called = (&rebound_panel == &panel);
            },
            .on_test_signal_changed = [&](const TestSignalConfig&) {
                signal_changed = true;
            },
            .on_file_load = [&](const std::string& path) {
                file_loaded = (path == "/tmp/test.wav");
            },
            .on_file_transport = [&](bool play, bool loop) {
                transport_called = play && !loop;
            },
        });

    callbacks.on_config_apply(StandaloneConfig{});
    callbacks.on_test_signal_changed(TestSignalConfig{});
    callbacks.on_file_load("/tmp/test.wav");
    callbacks.on_file_transport(true, false);

    REQUIRE(apply_called);
    REQUIRE(rebind_called);
    REQUIRE(signal_changed);
    REQUIRE(file_loaded);
    REQUIRE(transport_called);
}

TEST_CASE("Standalone settings callbacks skip rebind when apply fails",
          "[standalone][chrome]") {
    SettingsPanel panel;
    bool rebind_called = false;

    auto callbacks = make_standalone_settings_callbacks(
        panel,
        StandaloneSettingsActions{
            .apply_config = [](const StandaloneConfig&) { return false; },
            .rebind_after_apply = [&](SettingsPanel&) { rebind_called = true; },
        });

    callbacks.on_config_apply(StandaloneConfig{});
    REQUIRE_FALSE(rebind_called);
}

TEST_CASE("Standalone editor chrome keeps the editor root when settings are hidden",
          "[standalone][chrome]") {
    auto editor_root = std::make_unique<View>();
    auto* editor_ptr = editor_root.get();
    StandaloneConfig config;
    config.show_settings_tab = false;

    auto chrome = make_standalone_editor_chrome(
        std::move(editor_root), config, nullptr, nullptr, nullptr, {});

    REQUIRE(&chrome.window_root() == editor_ptr);
    REQUIRE(chrome.settings_panel() == nullptr);
    REQUIRE(chrome.tab_panel() == nullptr);
    REQUIRE(chrome.extra_window_height() == 0.0f);
    REQUIRE(chrome.chrome_label() == std::string_view("editor-only"));
}

TEST_CASE("Standalone editor chrome wraps editor and settings in a tab panel",
          "[standalone][chrome]") {
    auto editor_root = std::make_unique<View>();
    StandaloneConfig config;
    config.show_settings_tab = true;

    auto chrome = make_standalone_editor_chrome(
        std::move(editor_root), config, nullptr, nullptr, nullptr, {});

    REQUIRE(chrome.tab_panel() != nullptr);
    REQUIRE(&chrome.window_root() == chrome.tab_panel());
    REQUIRE(chrome.settings_panel() != nullptr);
    REQUIRE(chrome.tab_panel()->tab_count() == 2);
    REQUIRE(chrome.extra_window_height() == 32.0f);
    REQUIRE(chrome.chrome_label() == std::string_view("tabs"));
}

TEST_CASE("Standalone idle callback polls scripted UI before settings",
          "[standalone][chrome]") {
    StubWindowHost window;
    std::vector<std::string> calls;

    install_standalone_idle_callback(
        window,
        [&] { calls.push_back("scripted"); },
        [&] { calls.push_back("settings"); });

    REQUIRE(window.idle_callback_ != nullptr);
    window.idle_callback_();
    REQUIRE(calls == std::vector<std::string>{"scripted", "settings"});
}

TEST_CASE("Standalone idle callback still polls settings without scripted UI",
          "[standalone][chrome]") {
    StubWindowHost window;
    bool settings_polled = false;

    install_standalone_idle_callback(window, {}, [&] { settings_polled = true; });

    REQUIRE(window.idle_callback_ != nullptr);
    window.idle_callback_();
    REQUIRE(settings_polled);
}

TEST_CASE("Standalone repaint helper routes scripted UI repaint through the window",
          "[standalone][chrome]") {
    StubWindowHost window;
    StubScriptedUi scripted_ui;

    install_scripted_ui_repaint_callback(&scripted_ui, window);

    REQUIRE(scripted_ui.repaint_callback_ != nullptr);
    scripted_ui.repaint_callback_();
    REQUIRE(window.repaint_calls_ == 1);
}

TEST_CASE("Standalone pointer idle helper polls scripted UI and settings",
          "[standalone][chrome]") {
    StubWindowHost window;
    StubScriptedUi scripted_ui;
    StubSettingsPoller settings;

    install_standalone_idle_callback(window, &scripted_ui, &settings);

    REQUIRE(window.idle_callback_ != nullptr);
    window.idle_callback_();
    REQUIRE(scripted_ui.poll_calls_ == 1);
    REQUIRE(settings.poll_calls_ == 1);
}

TEST_CASE("Standalone editor content size subtracts chrome height",
          "[standalone][chrome]") {
    auto editor_root = std::make_unique<View>();
    auto tabs = make_standalone_editor_chrome(
        std::move(editor_root), StandaloneConfig{}, nullptr, nullptr, nullptr, {});

    auto size = standalone_editor_content_size({640, 392}, tabs);
    REQUIRE(size.width == 640);
    REQUIRE(size.height == 360);

    auto editor_only = make_standalone_editor_chrome(
        std::make_unique<View>(),
        StandaloneConfig{.show_settings_tab = false},
        nullptr,
        nullptr,
        nullptr,
        {});
    size = standalone_editor_content_size({640, 360}, editor_only);
    REQUIRE(size.width == 640);
    REQUIRE(size.height == 360);
}

TEST_CASE("WindowHost default content-size and resize callback are no-ops",
          "[standalone][chrome]") {
    DefaultWindowHost window;

    const auto size = window.get_content_size();
    REQUIRE(size.width == 0);
    REQUIRE(size.height == 0);

    bool resize_fired = false;
    window.set_resize_callback([&](uint32_t, uint32_t) {
        resize_fired = true;
    });
    REQUIRE_FALSE(resize_fired);
}

TEST_CASE("Standalone host sync installs a resize callback and applies initial size",
          "[standalone][chrome]") {
    StubWindowHost window;
    window.content_size_ = {800, 432};
    auto chrome = make_standalone_editor_chrome(
        std::make_unique<View>(), StandaloneConfig{}, nullptr, nullptr, nullptr, {});

    std::vector<WindowHost::ContentSize> seen_sizes;
    sync_standalone_editor_host(
        window,
        chrome,
        [&](uint32_t width, uint32_t height) {
            seen_sizes.push_back({width, height});
        });

    REQUIRE(window.resize_callback_ != nullptr);
    REQUIRE(seen_sizes.size() == 1);
    REQUIRE(seen_sizes[0].width == 800);
    REQUIRE(seen_sizes[0].height == 400);

    window.resize_callback_(900, 532);
    REQUIRE(seen_sizes.size() == 2);
    REQUIRE(seen_sizes[1].width == 900);
    REQUIRE(seen_sizes[1].height == 500);
}

TEST_CASE("Standalone host sync skips zero-sized initial host content",
          "[standalone][chrome]") {
    StubWindowHost window;
    window.content_size_ = {0, 0};
    auto chrome = make_standalone_editor_chrome(
        std::make_unique<View>(), StandaloneConfig{}, nullptr, nullptr, nullptr, {});

    std::vector<WindowHost::ContentSize> seen_sizes;
    sync_standalone_editor_host(
        window,
        chrome,
        [&](uint32_t width, uint32_t height) {
            seen_sizes.push_back({width, height});
        });

    REQUIRE(window.resize_callback_ != nullptr);
    REQUIRE(seen_sizes.empty());

    window.resize_callback_(900, 532);
    REQUIRE(seen_sizes.size() == 1);
    REQUIRE(seen_sizes[0].width == 900);
    REQUIRE(seen_sizes[0].height == 500);
}

TEST_CASE("Standalone bridge attach forwards host sizing through bridge resize",
          "[standalone][chrome]") {
    StubWindowHost window;
    window.content_size_ = {840, 452};
    auto chrome = make_standalone_editor_chrome(
        std::make_unique<View>(), StandaloneConfig{}, nullptr, nullptr, nullptr, {});
    StubBridge bridge;

    attach_standalone_editor_bridge(window, chrome, bridge);

    REQUIRE(window.resize_callback_ != nullptr);
    REQUIRE(bridge.resize_calls_.size() == 1);
    REQUIRE(bridge.resize_calls_[0].width == 840);
    REQUIRE(bridge.resize_calls_[0].height == 420);

    window.resize_callback_(900, 500);
    REQUIRE(bridge.resize_calls_.size() == 2);
    REQUIRE(bridge.resize_calls_[1].width == 900);
    REQUIRE(bridge.resize_calls_[1].height == 468);
}

TEST_CASE("Standalone log helper formats the chrome mode",
          "[standalone][chrome]") {
    auto editor_root = std::make_unique<View>();
    auto chrome = make_standalone_editor_chrome(
        std::move(editor_root), StandaloneConfig{}, nullptr, nullptr, nullptr, {});

    log_standalone_window_open(640, 360, false, false, chrome);
    SUCCEED();
}
