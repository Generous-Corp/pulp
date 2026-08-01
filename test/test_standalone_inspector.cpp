#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <pulp/format/detail/standalone_inspector.hpp>
#include <pulp/format/standalone.hpp>
#include <pulp/format/view_bridge.hpp>
#include <pulp/view/scripted_ui.hpp>
#include <pulp/view/window_host.hpp>
#if PULP_TEST_STANDALONE_INSPECTOR
#include <pulp/events/main_thread_dispatcher.hpp>
#include <pulp/inspect/client.hpp>
#include <pulp/inspect/discovery.hpp>
#include <pulp/inspect/inspector_overlay.hpp>
#endif

#include <choc/text/choc_JSON.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace pulp::format;
using namespace pulp::format::detail;
using namespace pulp::view;

namespace {

struct ScopedEnv {
    explicit ScopedEnv(std::string name) : name_(std::move(name)) {
        if (const char* previous = std::getenv(name_.c_str())) {
            previous_ = previous;
            had_previous_ = true;
        }
    }
    ~ScopedEnv() {
        if (had_previous_)
            set(previous_);
        else
            unset();
    }
    void set(const std::string& value) {
#if defined(_WIN32)
        _putenv_s(name_.c_str(), value.c_str());
#else
        ::setenv(name_.c_str(), value.c_str(), 1);
#endif
    }
    void unset() {
#if defined(_WIN32)
        _putenv_s(name_.c_str(), "");
#else
        ::unsetenv(name_.c_str());
#endif
    }

private:
    std::string name_;
    std::string previous_;
    bool had_previous_ = false;
};

class TestProcessor : public Processor {
public:
    PluginDescriptor descriptor() const override { return {}; }
    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>&,
                 const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const ProcessContext&) override {}
};

std::unique_ptr<Processor> null_processor_factory() { return {}; }

class StubWindowHost final : public WindowHost {
public:
    int repaint_calls = 0;
    std::vector<std::uint8_t> capture_bytes;
    bool capture_supported = true;
    bool blocking_event_loop = true;

    void show() override {}
    void hide() override {}
    bool is_visible() const override { return false; }
    void repaint() override { ++repaint_calls; }
    std::vector<std::uint8_t> capture_png() override { return capture_bytes; }
    bool supports_compositor_capture() const override { return capture_supported; }
    bool event_loop_blocks_until_close() const override { return blocking_event_loop; }
    void set_close_callback(std::function<void()>) override {}
    void run_event_loop() override {}
};

#if PULP_TEST_STANDALONE_INSPECTOR
std::vector<std::uint8_t> inspector_test_png() {
    return {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
        0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
        0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4, 0x89, 0x00, 0x00, 0x00,
        0x0d, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x60, 0x60, 0xf8, 0xff,
        0x1f, 0x00, 0x03, 0x02, 0x01, 0xff, 0xe6, 0x77, 0x0b, 0xae, 0x00, 0x00,
        0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
    };
}

class InspectorProcessor : public TestProcessor {
public:
    InspectorProcessor(pulp::state::StateStore& store,
                       std::filesystem::path script)
        : store_(store), script_(std::move(script)) {}

    PluginDescriptor descriptor() const override {
        PluginDescriptor descriptor;
        descriptor.name = "Inspector Test";
        descriptor.manufacturer = "Pulp";
        descriptor.bundle_id = "com.pulp.test.standalone-inspector";
        descriptor.version = "1.0.0";
        return descriptor;
    }
    int latency_samples() const override { return 128; }
    std::unique_ptr<View> create_view() override {
        auto root = std::make_unique<View>();
        scripted_ = std::make_unique<ScriptedUiSession>(
            *root, store_, ScriptedUiOptions{.script_path = script_});
        std::string error;
        if (!scripted_->load(&error)) {
            scripted_.reset();
            return nullptr;
        }
        return root;
    }
    ScriptedUiSession* active_scripted_ui() override { return scripted_.get(); }
    const ScriptedUiSession* active_scripted_ui() const override {
        return scripted_.get();
    }

private:
    pulp::state::StateStore& store_;
    std::filesystem::path script_;
    std::unique_ptr<ScriptedUiSession> scripted_;
};

class ReloadingInspectorProcessor final : public InspectorProcessor {
public:
    using InspectorProcessor::InspectorProcessor;
    bool supports_editor_reload() const override { return true; }
};

class QueuedMainThreadBackend {
public:
    QueuedMainThreadBackend() : main_thread_(std::this_thread::get_id()) {
        pulp::events::MainThreadDispatcher::Backend backend;
        backend.post = [this](pulp::events::Task task) {
            std::lock_guard lock(mutex_);
            tasks_.push_back(std::move(task));
            return true;
        };
        backend.is_main_thread =
            [this] { return std::this_thread::get_id() == main_thread_; };
        token_ = pulp::events::MainThreadDispatcher::register_backend(
            std::move(backend));
    }
    ~QueuedMainThreadBackend() {
        pulp::events::MainThreadDispatcher::unregister_backend(token_);
    }
    bool valid() const { return token_ != 0; }
    bool pump_one() {
        pulp::events::Task task;
        {
            std::lock_guard lock(mutex_);
            if (tasks_.empty())
                return false;
            task = std::move(tasks_.front());
            tasks_.pop_front();
        }
        task();
        return true;
    }

private:
    std::thread::id main_thread_;
    std::mutex mutex_;
    std::deque<pulp::events::Task> tasks_;
    pulp::events::MainThreadDispatcher::Token token_ = 0;
};
#endif

} // namespace

#if PULP_TEST_STANDALONE_INSPECTOR
TEST_CASE("Standalone inspector off mode publishes no endpoint or artifact",
          "[standalone][inspect][negative]") {
    const auto runtime_dir = std::filesystem::temp_directory_path()
        / "pulp-standalone-inspector-off-test";
    std::error_code error;
    std::filesystem::remove_all(runtime_dir, error);
    ScopedEnv runtime_env("PULP_INSPECTOR_RUNTIME_DIR");
    runtime_env.set(runtime_dir.string());

    StandaloneApp app(null_processor_factory);
    TestProcessor processor;
    pulp::state::StateStore store;
    ViewBridge bridge(processor, store);
    View root;
    StubWindowHost window;
    auto runtime = StandaloneInspectorRuntime::create(
        app, processor, bridge, root, window, "off", {});
    REQUIRE(runtime != nullptr);
    runtime->pump();
    REQUIRE_FALSE(std::filesystem::exists(runtime_dir));
}

TEST_CASE("Standalone inspector rejects capture when the host cannot provide it",
          "[standalone][inspect][capabilities][negative]") {
    StandaloneApp app(null_processor_factory);
    TestProcessor processor;
    pulp::state::StateStore store;
    ViewBridge bridge(processor, store);
    View root;
    StubWindowHost window;
    window.capture_supported = false;
    auto runtime = StandaloneInspectorRuntime::create(
        app, processor, bridge, root, window, "custom",
        {"session.describe", "capture.image"});
    REQUIRE(runtime == nullptr);
}

TEST_CASE("Standalone inspector rejects a non-blocking window event loop",
          "[standalone][inspect][lifecycle][negative]") {
    StandaloneApp app(null_processor_factory);
    TestProcessor processor;
    pulp::state::StateStore store;
    ViewBridge bridge(processor, store);
    View root;
    StubWindowHost window;
    window.blocking_event_loop = false;
    auto runtime = StandaloneInspectorRuntime::create(
        app, processor, bridge, root, window, "develop", {});
    REQUIRE(runtime == nullptr);
}

TEST_CASE("Standalone inspector failed startup detaches borrowed UI hooks",
          "[standalone][inspect][negative]") {
    const auto suffix = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto temp = std::filesystem::temp_directory_path()
        / ("pulp-standalone-inspector-failed-start-" + suffix);
    const auto runtime_path = temp / "not-a-directory";
    const auto script = temp / "ui.js";
    std::filesystem::create_directories(temp);
    {
        std::ofstream blocker(runtime_path);
        blocker << "file blocks discovery directory creation";
        std::ofstream source(script);
        source << "console.log('before-failure'); createLabel('v', 'one', '');";
    }
    ScopedEnv runtime_env("PULP_INSPECTOR_RUNTIME_DIR");
    runtime_env.set(runtime_path.string());

    StandaloneApp app(null_processor_factory);
    auto processor = std::make_unique<InspectorProcessor>(app.state(), script);
    ViewBridge bridge(*processor, app.state());
    REQUIRE(bridge.open());
    std::vector<std::string> primary_logs;
    processor->active_scripted_ui()->set_log_callback(
        [&](std::string_view, std::string_view message) {
            primary_logs.emplace_back(message);
        });
    StubWindowHost window;
    auto runtime = StandaloneInspectorRuntime::create(
        app, *processor, bridge, *bridge.view(), window, "develop", {});
    REQUIRE(runtime != nullptr);
    runtime->pump();
    REQUIRE(runtime->startup_failed());

    {
        std::ofstream source(script);
        source << "console.warn('after-failure'); createLabel('v', 'two', '');";
    }
    std::string reload_error;
    REQUIRE(processor->active_scripted_ui()->reload(&reload_error));
    REQUIRE(primary_logs == std::vector<std::string>{"after-failure"});
    bridge.close();
    std::error_code cleanup_error;
    std::filesystem::remove_all(temp, cleanup_error);
}

TEST_CASE("Standalone inspector fails closed for processor-level editor replacement",
          "[standalone][inspect][negative]") {
    const auto suffix = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto temp = std::filesystem::temp_directory_path()
        / ("pulp-standalone-inspector-editor-reload-" + suffix);
    const auto runtime_dir = temp / "runtime";
    const auto script = temp / "ui.js";
    std::filesystem::create_directories(temp);
    {
        std::ofstream source(script);
        source << "createLabel('v', 'one', '');";
    }
    ScopedEnv runtime_env("PULP_INSPECTOR_RUNTIME_DIR");
    runtime_env.set(runtime_dir.string());

    StandaloneApp app(null_processor_factory);
    ReloadingInspectorProcessor processor(app.state(), script);
    ViewBridge bridge(processor, app.state());
    REQUIRE(bridge.open());
    StubWindowHost window;
    auto runtime = StandaloneInspectorRuntime::create(
        app, processor, bridge, *bridge.view(), window, "develop", {});
    REQUIRE(runtime == nullptr);
    REQUIRE_FALSE(std::filesystem::exists(runtime_dir));
    bridge.close();
    std::error_code cleanup_error;
    std::filesystem::remove_all(temp, cleanup_error);
}

TEST_CASE("Standalone inspector composition root serves and tears down a live session",
          "[standalone][inspect][integration]") {
    const auto suffix = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto temp = std::filesystem::temp_directory_path()
        / ("pulp-standalone-inspector-integration-" + suffix);
    const auto runtime_dir = temp / "runtime";
    const auto script = temp / "ui.js";
    std::filesystem::create_directories(temp);
    {
        std::ofstream out(script);
        out << "console.log('before-start'); createLabel('v', 'one', '');";
    }
    ScopedEnv runtime_env("PULP_INSPECTOR_RUNTIME_DIR");
    runtime_env.set(runtime_dir.string());

    StandaloneApp app(null_processor_factory);
    app.set_config(StandaloneConfig{.sample_rate = 48'000.0,
                                    .buffer_size = 256,
                                    .output_channels = 2,
                                    .input_channels = 0});
    pulp::state::ParamInfo gain;
    gain.id = 2;
    gain.name = "Gain";
    gain.unit = "dB";
    gain.range = {-18.0f, 18.0f, 0.0f};
    app.state().add_parameter(std::move(gain));
    auto processor = std::make_unique<InspectorProcessor>(app.state(), script);
    ViewBridge bridge(*processor, app.state());
    REQUIRE(bridge.open());
    std::vector<std::string> primary_logs;
    processor->active_scripted_ui()->set_log_callback(
        [&](std::string_view, std::string_view message) {
            primary_logs.emplace_back(message);
        });

    StubWindowHost window;
    window.capture_bytes = inspector_test_png();
    QueuedMainThreadBackend dispatcher;
    REQUIRE(dispatcher.valid());
    auto runtime = StandaloneInspectorRuntime::create(
        app, *processor, bridge, *bridge.view(), window, "develop", {});
    REQUIRE(runtime != nullptr);
    pulp::inspect::InspectorDiscoveryReader reader(runtime_dir);
    REQUIRE(reader.list().empty());
    bridge.view()->set_window_host(&window);
    const auto repaint_calls = window.repaint_calls;
    runtime->pump();
    runtime->pump();
    REQUIRE(bridge.view()->interaction().overlay_queue.size() == 1);
    REQUIRE(window.repaint_calls == repaint_calls + 1);

    const auto records = reader.list();
    REQUIRE(records.size() == 1);
    pulp::inspect::InspectorClient client;
    REQUIRE(client.connect(records.front(), reader));
    const auto request = [&](std::string method, std::string params) {
        auto response = std::async(std::launch::async,
            [&client, method = std::move(method), params = std::move(params)] {
                return client.request(method, params, std::chrono::seconds(1));
            });
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (response.wait_for(std::chrono::milliseconds(0))
                   != std::future_status::ready
               && std::chrono::steady_clock::now() < deadline) {
            if (!dispatcher.pump_one())
                std::this_thread::yield();
        }
        REQUIRE(response.wait_for(std::chrono::milliseconds(0))
                == std::future_status::ready);
        return response.get();
    };

    const auto capabilities = request("Session.getCapabilities", "{}");
    REQUIRE_FALSE(capabilities.is_error);
    REQUIRE(capabilities.params_json.find("state.write") != std::string::npos);
    const auto audio = request("Audio.getConfig", "{}");
    REQUIRE_FALSE(audio.is_error);
    const auto audio_json = choc::json::parse(audio.params_json);
    REQUIRE(audio_json["sample_rate"].getWithDefault<double>(0.0) == 48'000.0);
    REQUIRE(audio_json["latency_samples"].getWithDefault<std::int64_t>(0) == 128);
    const auto screenshot = request("Capture.screenshot", "{}");
    REQUIRE_FALSE(screenshot.is_error);
    const auto screenshot_json = choc::json::parse(screenshot.params_json);
    REQUIRE(screenshot_json["width"].getWithDefault<std::int64_t>(0) == 1);
    REQUIRE(screenshot_json["height"].getWithDefault<std::int64_t>(0) == 1);
    REQUIRE_FALSE(screenshot_json["data"].getString().empty());

    const auto denied = request("State.setParameter", R"({"id":2,"value":3})");
    REQUIRE(denied.is_error);
    REQUIRE(denied.error_code == "controller_lease_required");
    REQUIRE_FALSE(request("Session.acquireController", "{}").is_error);
    REQUIRE_FALSE(request(
        "State.setParameter", R"({"id":2,"value":3})").is_error);
    REQUIRE(app.state().get_value(2) == Catch::Approx(3.0f));

    {
        std::ofstream out(script);
        out << "console.warn('after-start'); createLabel('v', 'two', '');";
    }
    std::string reload_error;
    REQUIRE(processor->active_scripted_ui()->reload(&reload_error));
    REQUIRE(primary_logs == std::vector<std::string>{"after-start"});
    const auto console = request("Console.getMessages", "{}");
    REQUIRE_FALSE(console.is_error);
    REQUIRE(console.params_json.find("after-start") != std::string::npos);

    client.disconnect();
    runtime->stop();
    REQUIRE(reader.list().empty());
    {
        std::ofstream out(script);
        out << "console.error('after-stop'); createLabel('v', 'three', '');";
    }
    REQUIRE(processor->active_scripted_ui()->reload(&reload_error));
    REQUIRE(primary_logs ==
            std::vector<std::string>{"after-start", "after-stop"});
    bridge.view()->set_window_host(nullptr);
    bridge.close();
    processor.reset();
    runtime->stop();
    runtime.reset();
    std::error_code cleanup_error;
    std::filesystem::remove_all(temp, cleanup_error);
}
#else
TEST_CASE("Standalone rejects inspector activation when support is disabled",
          "[standalone][inspect][negative]") {
    ScopedEnv profile("PULP_INSPECT_PROFILE");
    profile.unset();
    StandaloneApp app(null_processor_factory);
    app.set_config(StandaloneConfig{.inspector_profile = "develop"});
    REQUIRE_FALSE(app.run_with_editor());
}
#endif
