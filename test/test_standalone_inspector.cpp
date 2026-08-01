#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <pulp/canvas/recording_canvas.hpp>
#include <pulp/format/detail/standalone_inspector.hpp>
#include <pulp/format/standalone.hpp>
#include <pulp/format/view_bridge.hpp>
#include <pulp/view/scripted_ui.hpp>
#include <pulp/view/value_channel_set.hpp>
#include <pulp/view/window_host.hpp>
#if PULP_TEST_STANDALONE_INSPECTOR
#include <pulp/events/main_thread_dispatcher.hpp>
#include <pulp/inspect/client.hpp>
#include <pulp/inspect/discovery.hpp>
#include <pulp/inspect/inspector_overlay.hpp>
#endif

#include <choc/text/choc_JSON.h>

#include <algorithm>
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
    bool exit_drain_supported = true;
    bool deferred_close_supported = true;
    std::function<void()> close_callback;
    std::function<void()> capture_callback;
    std::function<bool()> event_loop_step;
    std::function<void()> deferred_close;
    int deferred_close_calls = 0;
    int run_until_calls = 0;
    int readiness_checks = 0;
    bool run_until_ready = false;

    void show() override {}
    void hide() override {}
    bool is_visible() const override { return false; }
    void repaint() override { ++repaint_calls; }
    std::vector<std::uint8_t> capture_png() override {
        if (capture_callback)
            capture_callback();
        return capture_bytes;
    }
    bool supports_compositor_capture() const override { return capture_supported; }
    bool event_loop_blocks_until_close() const override { return blocking_event_loop; }
    bool event_loop_supports_exit_drain() const override {
        return exit_drain_supported;
    }
    bool supports_deferred_close() const override {
        return deferred_close_supported;
    }
    void request_close_deferred() override {
        ++deferred_close_calls;
        deferred_close = [this] { request_close(); };
    }
    void run_deferred_close() {
        auto close = std::move(deferred_close);
        if (close)
            close();
    }
    void request_close() override {
        if (close_callback)
            close_callback();
    }
    void set_close_callback(std::function<void()> callback) override {
        close_callback = std::move(callback);
    }
    void run_event_loop() override {}
    void run_event_loop_until(std::function<bool()> ready_to_return) override {
        ++run_until_calls;
        run_event_loop();
        for (int attempt = 0; attempt < 64; ++attempt) {
            ++readiness_checks;
            if (!ready_to_return || ready_to_return()) {
                run_until_ready = true;
                return;
            }
            if (!event_loop_step || !event_loop_step())
                return;
        }
    }
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
        : store_(store), script_(std::move(script)) {
        channels_.declare_meter("gain_reduction", "dB", 0.0f);
    }

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
    pulp::view::ValueChannelSet* value_channels() override {
        return &channels_;
    }

private:
    pulp::state::StateStore& store_;
    std::filesystem::path script_;
    pulp::view::ValueChannelSet channels_;
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
            ++post_count_;
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
    std::size_t post_count() const {
        std::lock_guard lock(mutex_);
        return post_count_;
    }
    std::size_t pending_count() const {
        std::lock_guard lock(mutex_);
        return tasks_.size();
    }
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
    std::size_t pump_all() {
        std::size_t pumped = 0;
        while (pump_one())
            ++pumped;
        return pumped;
    }

private:
    std::thread::id main_thread_;
    mutable std::mutex mutex_;
    std::deque<pulp::events::Task> tasks_;
    std::size_t post_count_ = 0;
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

TEST_CASE("Standalone inspector rejects an event loop without exit draining",
          "[standalone][inspect][lifecycle][negative]") {
    StandaloneApp app(null_processor_factory);
    TestProcessor processor;
    pulp::state::StateStore store;
    ViewBridge bridge(processor, store);
    View root;
    StubWindowHost window;
    window.exit_drain_supported = false;
    auto runtime =
        StandaloneInspectorRuntime::create(app, processor, bridge, root, window, "develop", {});
    REQUIRE(runtime == nullptr);
}

TEST_CASE("Standalone inspector rejects a host without deferred close",
          "[standalone][inspect][lifecycle][negative]") {
    StandaloneApp app(null_processor_factory);
    TestProcessor processor;
    pulp::state::StateStore store;
    ViewBridge bridge(processor, store);
    View root;
    StubWindowHost window;
    window.deferred_close_supported = false;
    auto runtime =
        StandaloneInspectorRuntime::create(app, processor, bridge, root, window, "develop", {});
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
    int close_calls = 0;
    window.set_close_callback(runtime->wrap_close([&] { ++close_calls; }));
    runtime->pump();
    REQUIRE(runtime->startup_failed());
    REQUIRE(window.deferred_close_calls == 1);
    REQUIRE(close_calls == 0);
    REQUIRE(static_cast<bool>(window.deferred_close));

    {
        std::ofstream source(script);
        source << "console.warn('after-failure'); createLabel('v', 'two', '');";
    }
    std::string reload_error;
    REQUIRE(processor->active_scripted_ui()->reload(&reload_error));
    REQUIRE(primary_logs == std::vector<std::string>{"after-failure"});
    window.run_deferred_close();
    REQUIRE(close_calls == 1);
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
    pulp::canvas::RecordingCanvas indicator_canvas;
    View::paint_overlays(indicator_canvas, bridge.view());
    REQUIRE(std::any_of(
        indicator_canvas.commands().begin(), indicator_canvas.commands().end(),
        [](const pulp::canvas::DrawCommand& command) {
            return command.type == pulp::canvas::DrawCommand::Type::fill_text
                && command.text == "INSPECT develop";
        }));
    REQUIRE(bridge.view()->interaction().overlay_queue.empty());

    const auto records = reader.list();
    REQUIRE(records.size() == 1);
    pulp::inspect::InspectorClient client;
    REQUIRE(client.connect(records.front(), reader));
    const auto request_from = [&](pulp::inspect::InspectorClient& active_client,
                                  std::string method, std::string params) {
        auto response = std::async(std::launch::async,
            [&active_client, method = std::move(method), params = std::move(params)] {
                return active_client.request(
                    method, params, std::chrono::seconds(1));
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
    const auto request = [&](std::string method, std::string params) {
        return request_from(client, std::move(method), std::move(params));
    };

    const auto capabilities = request("Session.getCapabilities", "{}");
    REQUIRE_FALSE(capabilities.is_error);
    REQUIRE(capabilities.params_json.find("state.write") != std::string::npos);
    const auto context = request("Inspector.getAgentContext", "{}");
    REQUIRE_FALSE(context.is_error);
    const auto context_json = choc::json::parse(context.params_json);
    REQUIRE(context_json["identity"]["pluginId"].getString()
            == "com.pulp.test.standalone-inspector");
    REQUIRE(context_json["identity"]["sessionId"].getString()
            == records.front().session_id);
    REQUIRE(context_json["identity"]["instanceId"].getString()
            == records.front().instance_id);
    REQUIRE(context_json["editor"]["open"].getBool());
    REQUIRE_FALSE(context_json["editor"]["windowVisible"].getBool());
    REQUIRE(context_json["hotReload"]["available"].getBool());
    REQUIRE_FALSE(context_json["binary"]["path"].getString().empty());

    const auto posts_before_document = dispatcher.post_count();
    const auto document = request("DOM.getDocument", "{}");
    REQUIRE_FALSE(document.is_error);
    REQUIRE(document.params_json.find("\"v\"") != std::string::npos);
    REQUIRE(dispatcher.post_count() == posts_before_document + 1);

    const auto parameters = request("State.getParameters", "{}");
    REQUIRE_FALSE(parameters.is_error);
    const auto parameters_json = choc::json::parse(parameters.params_json);
    REQUIRE(parameters_json.isArray());
    REQUIRE(parameters_json.size() == 1);
    REQUIRE(parameters_json[0]["id"].getInt64() == 2);
    REQUIRE(parameters_json[0]["name"].getString() == "Gain");
    REQUIRE(parameters_json[0]["value"].getWithDefault<double>(-1.0) == 0.0);

    const auto value_channels = request("State.getValueChannels", "{}");
    REQUIRE_FALSE(value_channels.is_error);
    const auto value_channels_json = choc::json::parse(value_channels.params_json);
    REQUIRE(value_channels_json.isArray());
    REQUIRE(value_channels_json.size() == 1);
    REQUIRE(value_channels_json[0]["name"].getString() == "gain_reduction");
    REQUIRE(value_channels_json[0]["unit"].getString() == "dB");
    REQUIRE(value_channels_json[0]["shape"].getString() == "meter");

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

    const auto denied = request(
        "State.setParameter",
        R"({"id":2,"value":3,"secret":"must-not-enter-audit"})");
    REQUIRE(denied.is_error);
    REQUIRE(denied.error_code == "controller_lease_required");
    REQUIRE_FALSE(request("Session.acquireController", "{}").is_error);
    REQUIRE_FALSE(request(
        "State.setParameter", R"({"id":2,"value":3})").is_error);
    REQUIRE(app.state().get_value(2) == Catch::Approx(3.0f));

    const auto audit = runtime->audit_snapshot_for_testing();
    REQUIRE(audit.size() == 2);
    REQUIRE(audit[0].session_id == records.front().session_id);
    REQUIRE(audit[0].instance_id == records.front().instance_id);
    REQUIRE(audit[0].client_id == audit[1].client_id);
    REQUIRE_FALSE(audit[0].client_id.empty());
    REQUIRE(audit[0].method == "State.setParameter");
    REQUIRE(audit[0].outcome ==
            pulp::format::detail::StandaloneInspectorAuditOutcome::Denied);
    REQUIRE(audit[0].error_code == "controller_lease_required");
    REQUIRE(audit[1].method == "State.setParameter");
    REQUIRE(audit[1].outcome ==
            pulp::format::detail::StandaloneInspectorAuditOutcome::Applied);
    REQUIRE(audit[1].error_code.empty());
    REQUIRE(audit[0].method.find("must-not-enter-audit") == std::string::npos);

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
    runtime.reset();

    auto observe_runtime = StandaloneInspectorRuntime::create(
        app, *processor, bridge, *bridge.view(), window, "observe", {});
    REQUIRE(observe_runtime != nullptr);
    observe_runtime->pump();
    const auto observe_records = reader.list();
    REQUIRE(observe_records.size() == 1);
    pulp::inspect::InspectorClient observe_client;
    REQUIRE(observe_client.connect(observe_records.front(), reader));
    const auto observe_denied = request_from(
        observe_client, "State.setParameter", R"({"id":2,"value":4})");
    REQUIRE(observe_denied.is_error);
    REQUIRE(observe_denied.error_code == "capability_denied");
    REQUIRE(app.state().get_value(2) == Catch::Approx(3.0f));
    observe_client.disconnect();
    observe_runtime->stop();
    observe_runtime.reset();
    REQUIRE(reader.list().empty());
    bridge.view()->interaction().overlay_queue.clear();

    auto cancellation_runtime = StandaloneInspectorRuntime::create(
        app, *processor, bridge, *bridge.view(), window, "develop", {});
    REQUIRE(cancellation_runtime != nullptr);
    cancellation_runtime->pump();
    const auto cancellation_records = reader.list();
    REQUIRE(cancellation_records.size() == 1);
    pulp::inspect::InspectorClient cancellation_client;
    REQUIRE(cancellation_client.connect(cancellation_records.front(), reader));
    REQUIRE_FALSE(request_from(cancellation_client, "Session.acquireController", "{}").is_error);

    int app_quit_close_calls = 0;
    auto app_quit_source = std::make_shared<int>(1);
    std::weak_ptr<int> app_quit_source_lifetime = app_quit_source;
    auto app_quit_close = cancellation_runtime->wrap_close([&] {
        ++app_quit_close_calls;
        app_quit_source.reset();
    });
    const auto pending_before = dispatcher.pending_count();
    auto cancelled_mutation = std::async(std::launch::async, [&] {
        return cancellation_client.request("State.setParameter", R"({"id":2,"value":7})",
                                           std::chrono::seconds(1));
    });
    const auto queued_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (dispatcher.pending_count() == pending_before &&
           std::chrono::steady_clock::now() < queued_deadline) {
        std::this_thread::yield();
    }
    REQUIRE(dispatcher.pending_count() == pending_before + 1);

    const auto stop_started = std::chrono::steady_clock::now();
    cancellation_runtime->stop();
    const auto stop_elapsed = std::chrono::steady_clock::now() - stop_started;
    app_quit_close();
    cancellation_runtime->stop();
    REQUIRE(stop_elapsed < std::chrono::milliseconds(500));
    REQUIRE(cancellation_runtime->retirement_pending());
    const auto cancellation_pending_state = cancellation_runtime->lifecycle_state();
    REQUIRE_FALSE(cancellation_pending_state.rpc_accepting);
    REQUIRE_FALSE(cancellation_pending_state.dispatch_accepting);
    REQUIRE(cancellation_pending_state.borrowed_sources_attached);
    REQUIRE(app_quit_close_calls == 0);
    REQUIRE_FALSE(app_quit_source_lifetime.expired());
    REQUIRE(app.state().get_value(2) == Catch::Approx(3.0f));
    REQUIRE(cancelled_mutation.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    const auto cancelled_response = cancelled_mutation.get();
    REQUIRE(cancelled_response.is_error);
    REQUIRE((cancelled_response.error_code == "dispatch_cancelled" ||
             cancelled_response.error_code == "connection_closed"));

    window.event_loop_step = [&] { return dispatcher.pump_one(); };
    window.run_event_loop_until([&] { return cancellation_runtime->try_finish_retirement(); });
    REQUIRE(window.run_until_calls == 1);
    REQUIRE(window.readiness_checks >= 2);
    REQUIRE(window.run_until_ready);
    REQUIRE(app.state().get_value(2) == Catch::Approx(3.0f));
    REQUIRE(cancellation_runtime->try_finish_retirement());
    REQUIRE(app_quit_close_calls == 1);
    REQUIRE(app_quit_source_lifetime.expired());
    REQUIRE_FALSE(cancellation_runtime->lifecycle_state().borrowed_sources_attached);
    app_quit_close();
    cancellation_runtime->stop();
    REQUIRE(cancellation_runtime->try_finish_retirement());
    REQUIRE(app_quit_close_calls == 1);
    cancellation_client.disconnect();
    cancellation_runtime.reset();
    dispatcher.pump_all();
    window.event_loop_step = {};
    REQUIRE(reader.list().empty());

    auto reentrant_runtime = StandaloneInspectorRuntime::create(
        app, *processor, bridge, *bridge.view(), window, "develop", {});
    REQUIRE(reentrant_runtime != nullptr);
    reentrant_runtime->pump();
    const auto reentrant_records = reader.list();
    REQUIRE(reentrant_records.size() == 1);
    pulp::inspect::InspectorClient reentrant_client;
    REQUIRE(reentrant_client.connect(reentrant_records.front(), reader));

    int reentrant_close_calls = 0;
    int capture_source_touches = 0;
    auto borrowed_source = std::make_shared<int>(1);
    std::weak_ptr<int> borrowed_source_lifetime = borrowed_source;
    auto reentrant_close = reentrant_runtime->wrap_close([&] {
        ++reentrant_close_calls;
        borrowed_source.reset();
    });
    window.capture_callback = [&] {
        ++capture_source_touches;
        const auto close_started = std::chrono::steady_clock::now();
        reentrant_close();
        const auto close_elapsed = std::chrono::steady_clock::now() - close_started;
        reentrant_runtime->stop();
        reentrant_close();
        REQUIRE(close_elapsed < std::chrono::milliseconds(500));
        REQUIRE(reentrant_runtime->retirement_pending());
        const auto reentrant_pending_state = reentrant_runtime->lifecycle_state();
        REQUIRE_FALSE(reentrant_pending_state.rpc_accepting);
        REQUIRE_FALSE(reentrant_pending_state.dispatch_accepting);
        REQUIRE(reentrant_pending_state.borrowed_sources_attached);
        REQUIRE(reentrant_close_calls == 0);
        REQUIRE_FALSE(borrowed_source_lifetime.expired());
        REQUIRE(processor->active_scripted_ui() != nullptr);
    };
    auto reentrant_capture = std::async(std::launch::async, [&] {
        return reentrant_client.request("Capture.screenshot", "{}", std::chrono::seconds(1));
    });
    const auto reentrant_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (dispatcher.pending_count() == 0 &&
           std::chrono::steady_clock::now() < reentrant_deadline) {
        std::this_thread::yield();
    }
    REQUIRE(dispatcher.pending_count() == 1);
    REQUIRE(dispatcher.pump_one());
    REQUIRE(capture_source_touches == 1);
    REQUIRE(reentrant_runtime->try_finish_retirement());
    REQUIRE(reentrant_close_calls == 1);
    REQUIRE(borrowed_source_lifetime.expired());
    REQUIRE_FALSE(reentrant_runtime->lifecycle_state().borrowed_sources_attached);
    reentrant_close();
    reentrant_runtime->stop();
    REQUIRE(reentrant_runtime->try_finish_retirement());
    REQUIRE(reentrant_close_calls == 1);
    REQUIRE(reentrant_capture.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    (void)reentrant_capture.get();
    reentrant_client.disconnect();
    window.capture_callback = {};
    reentrant_runtime.reset();
    dispatcher.pump_all();
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
