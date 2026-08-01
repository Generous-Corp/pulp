#pragma once

#include <catch2/catch_test_macros.hpp>
#include <pulp/format/detail/standalone_inspector.hpp>
#include <pulp/format/standalone.hpp>
#include <pulp/format/view_bridge.hpp>
#include <pulp/view/scripted_ui.hpp>
#include <pulp/view/value_channel_set.hpp>
#include <pulp/view/window_host.hpp>
#if PULP_TEST_STANDALONE_INSPECTOR
#include <pulp/events/main_thread_dispatcher.hpp>
#include <pulp/inspect/client.hpp>
#endif

#include <chrono>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pulp::test::standalone_inspector {

using namespace pulp::format;
using namespace pulp::format::detail;
using namespace pulp::view;

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

#if PULP_TEST_STANDALONE_INSPECTOR
struct ScopedInspectorTelemetryClock {
    explicit ScopedInspectorTelemetryClock(
        StandaloneInspectorTelemetryClock clock) {
        set_standalone_inspector_telemetry_clock_for_testing(std::move(clock));
    }
    ~ScopedInspectorTelemetryClock() {
        set_standalone_inspector_telemetry_clock_for_testing({});
    }
};
#endif

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

inline std::unique_ptr<Processor> null_processor_factory() { return {}; }

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
inline std::vector<std::uint8_t> inspector_test_png() {
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
    InspectorProcessor(pulp::state::StateStore& store, std::filesystem::path script,
                       CapabilitySet capabilities = CapabilitySet::all())
        : store_(store), script_(std::move(script)), capabilities_(capabilities) {
        gain_reduction_ = channels_.declare_meter("gain_reduction", "dB", 0.0f);
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
        root_ = root.get();
        if (!replace_scripted_ui(capabilities_))
            return nullptr;
        return root;
    }
    ScriptedUiSession* active_scripted_ui() override { return scripted_.get(); }
    const ScriptedUiSession* active_scripted_ui() const override {
        return scripted_.get();
    }
    bool supports_editor_reload() const override { return true; }
    std::uint64_t editor_reload_generation() const override {
        return scripted_ui_generation_;
    }
    pulp::view::ValueChannelSet* value_channels() override {
        return &channels_;
    }
    bool replace_scripted_ui(CapabilitySet capabilities) {
        capabilities_ = capabilities;
        if (!root_)
            return false;
        auto replacement = std::make_unique<ScriptedUiSession>(
            *root_, store_, ScriptedUiOptions{
                .script_path = script_,
                .granted_capabilities = capabilities_});
        std::string error;
        if (!replacement->load(&error))
            return false;
        if (scripted_)
            ++retired_scripted_sessions;
        scripted_ = std::move(replacement);
        ++scripted_ui_generation_;
        return true;
    }
    void bump_scripted_ui_generation_without_replacement() {
        REQUIRE(scripted_ != nullptr);
        ++scripted_ui_generation_;
    }
    void publish_gain_reduction(float rms, float peak) {
        REQUIRE(gain_reduction_ != nullptr);
        pulp::view::MeterFrame frame;
        frame.channels = 1;
        frame.rms[0] = rms;
        frame.peak[0] = peak;
        gain_reduction_->publish(frame);
        (void)gain_reduction_->read();
    }
    std::size_t retired_scripted_sessions = 0;

private:
    pulp::state::StateStore& store_;
    std::filesystem::path script_;
    CapabilitySet capabilities_ = CapabilitySet::all();
    View* root_ = nullptr;
    std::uint64_t scripted_ui_generation_ = 0;
    pulp::view::ValueChannelSet channels_;
    pulp::view::MeterSource* gain_reduction_ = nullptr;
    std::unique_ptr<ScriptedUiSession> scripted_;
};

class NonReloadingInspectorProcessor final : public InspectorProcessor {
public:
    using InspectorProcessor::InspectorProcessor;
    bool supports_editor_reload() const override { return false; }
    std::uint64_t editor_reload_generation() const override { return 0; }
};

class ReloadingInspectorProcessor final : public InspectorProcessor {
public:
    ReloadingInspectorProcessor(pulp::state::StateStore& store,
                                std::filesystem::path script)
        : InspectorProcessor(store, std::move(script)) {
        replace_value_channels("before_reload", false);
    }
    bool supports_editor_reload() const override { return true; }
    std::uint64_t editor_reload_generation() const override { return generation_; }
    pulp::view::ValueChannelSet* value_channels() override {
        ++value_channel_visits;
        return reload_channels_.get();
    }
    void visit_active_scripted_ui(
        const std::function<void(ScriptedUiSession*)>& visitor) override {
        ++scripted_ui_visits;
        if (scripted_ui_null_visits != 0) {
            --scripted_ui_null_visits;
            visitor(nullptr);
            return;
        }
        InspectorProcessor::visit_active_scripted_ui(visitor);
    }
    void visit_active_scripted_ui(
        const std::function<void(const ScriptedUiSession*)>& visitor) const override {
        ++scripted_ui_visits;
        if (scripted_ui_null_visits != 0) {
            --scripted_ui_null_visits;
            visitor(nullptr);
            return;
        }
        InspectorProcessor::visit_active_scripted_ui(visitor);
    }
    void replace_value_channels(std::string name, bool bump = true) {
        auto replacement = std::make_shared<pulp::view::ValueChannelSet>();
        REQUIRE(replacement->declare_scalar(std::move(name)) != nullptr);
        reload_channels_ = std::move(replacement);
        if (bump)
            ++generation_;
    }
    void replace_with_empty_value_channels() {
        reload_channels_ = std::make_shared<pulp::view::ValueChannelSet>();
        ++generation_;
    }
    void advance_generation_without_replacing_editor() { ++generation_; }
    void hide_scripted_ui_for_next_visit() { ++scripted_ui_null_visits; }
    std::weak_ptr<pulp::view::ValueChannelSet> value_channel_lifetime() const {
        return reload_channels_;
    }
    std::size_t value_channel_visits = 0;
    mutable std::size_t scripted_ui_visits = 0;

private:
    mutable std::size_t scripted_ui_null_visits = 0;
    std::shared_ptr<pulp::view::ValueChannelSet> reload_channels_;
    std::uint64_t generation_ = 0;
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

template <typename Predicate>
bool spin_until(Predicate&& predicate, std::chrono::milliseconds timeout =
                                           std::chrono::seconds(2)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    return predicate();
}

inline pulp::inspect::InspectorMessage request_with_dispatch(
    pulp::inspect::InspectorClient& client,
    QueuedMainThreadBackend& dispatcher,
    std::string method,
    std::string params) {
    auto response = std::async(
        std::launch::async,
        [&client, method = std::move(method), params = std::move(params)] {
            return client.request(method, params, std::chrono::seconds(1));
        });
    REQUIRE(spin_until([&] {
        if (!dispatcher.pump_one())
            std::this_thread::yield();
        return response.wait_for(std::chrono::milliseconds(0))
            == std::future_status::ready;
    }));
    return response.get();
}
#endif

} // namespace pulp::test::standalone_inspector
