#include <pulp/events/main_thread_dispatcher.hpp>
#include <pulp/format/standalone.hpp>
#include <pulp/inspect/discovery.hpp>
#include <pulp/view/scripted_ui.hpp>
#include <pulp/view/value_channel_set.hpp>

#import <AppKit/AppKit.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

namespace {

std::filesystem::path g_script_path;
std::atomic<std::uint32_t> g_note_on_count{0};
std::atomic<std::uint32_t> g_note_off_count{0};
std::atomic<bool> g_transport_match{false};
bool g_expose_value_channels = true;
bool g_support_reload = true;
std::atomic<std::uint64_t> g_reload_generation{0};
std::atomic<pulp::view::ScalarSource*> g_scalar_source{nullptr};

void wait_for_external_state_change_poll() {
    static std::mutex mutex;
    static std::condition_variable cv;
    std::unique_lock lock(mutex);
    cv.wait_for(lock, std::chrono::milliseconds(10));
}

class StandaloneWorkflowProcessor final : public pulp::format::Processor {
public:
    StandaloneWorkflowProcessor() {
        scalar_ = channels_.declare_scalar("workflow_scalar", "normalized");
        g_scalar_source.store(scalar_, std::memory_order_release);
        vector_ = channels_.declare_vector("workflow_vector", "sample");
        events_ = channels_.declare_events("workflow_events", "edge");
        channels_.declare_scalar("workflow_stale", "normalized");
    }

    pulp::format::PluginDescriptor descriptor() const override {
        return {
            .name = "Standalone Inspector Workflow Fixture",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.test.standalone-inspector-workflow",
            .version = "1.0.0",
            .category = pulp::format::PluginCategory::Effect,
            .input_buses = {},
            .output_buses = {{"Output", 2}},
            .accepts_midi = true,
        };
    }

    void define_parameters(pulp::state::StateStore& store) override {
        store_ = &store;
        store.add_parameter({
            .id = 42'017,
            .name = "Workflow Gain",
            .unit = "dB",
            .range = {-24.0f, 6.0f, -6.0f},
        });
    }

    void prepare(const pulp::format::PrepareContext&) override {}

    void process(pulp::audio::BufferView<float>& output,
                 const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer& midi_in, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext& context) override {
        for (const auto& event : midi_in) {
            if (event.is_note_on() && event.channel() == 2 && event.note() == 64)
                g_note_on_count.fetch_add(1, std::memory_order_relaxed);
            if (event.is_note_off() && event.channel() == 2 && event.note() == 64)
                g_note_off_count.fetch_add(1, std::memory_order_relaxed);
        }
        if (!context.is_playing && context.position_samples == 96'000 &&
            context.tempo_bpm == 90.0) {
            g_transport_match.store(true, std::memory_order_release);
        }
        const auto publication = publications_.fetch_add(1, std::memory_order_relaxed) + 1;
        const float scalar_value = static_cast<float>(publication % 100) / 100.0f;
        scalar_->publish(scalar_value);
        const std::array<float, 4> vector_values{
            scalar_value, -scalar_value, scalar_value * 0.5f, -scalar_value * 0.5f};
        vector_->publish(vector_values.data(), static_cast<int>(vector_values.size()));
        // Four occurrences per audio block deliberately outrun the bounded
        // telemetry batch retained for a 1 Hz subscriber. This makes the
        // slow-consumer loss proof deterministic across device buffer sizes.
        const std::array<pulp::view::ValueEvent, 4> occurrences{{
            {.frame_index = 0, .value = scalar_value},
            {.frame_index = 64, .value = scalar_value},
            {.frame_index = 128, .value = scalar_value},
            {.frame_index = 192, .value = scalar_value},
        }};
        events_->publish(occurrences.data(), static_cast<int>(occurrences.size()));
        output.clear();
    }

    pulp::format::ViewSize view_size() const override {
        return {420, 240, 420, 240, 420, 240};
    }

    std::unique_ptr<pulp::view::View> create_view() override {
        if (store_ == nullptr) return nullptr;
        auto root = std::make_unique<pulp::view::View>();
        scripted_ = std::make_unique<pulp::view::ScriptedUiSession>(
            *root, *store_,
            pulp::view::ScriptedUiOptions{
                .script_path = g_script_path,
                .value_channels = g_expose_value_channels ? &channels_ : nullptr,
                .granted_capabilities = {},
            });
        std::string error;
        if (!scripted_->load(&error)) {
            scripted_.reset();
            return nullptr;
        }
        return root;
    }

    pulp::view::ScriptedUiSession* active_scripted_ui() override {
        return scripted_.get();
    }

    const pulp::view::ScriptedUiSession* active_scripted_ui() const override {
        return scripted_.get();
    }

    bool supports_editor_reload() const override { return g_support_reload; }
    std::uint64_t editor_reload_generation() const override {
        return g_reload_generation.load(std::memory_order_acquire);
    }
    bool reload_active_scripted_ui_in_place(std::string* error) override {
        return scripted_ != nullptr && scripted_->reload(error);
    }
    pulp::view::ValueChannelSet* value_channels() override {
        return g_expose_value_channels ? &channels_ : nullptr;
    }

private:
    pulp::state::StateStore* store_ = nullptr;
    std::atomic<std::uint64_t> publications_{0};
    pulp::view::ValueChannelSet channels_;
    pulp::view::ScalarSource* scalar_ = nullptr;
    pulp::view::VectorSource* vector_ = nullptr;
    pulp::view::EventSource* events_ = nullptr;
    std::unique_ptr<pulp::view::ScriptedUiSession> scripted_;
};

std::unique_ptr<pulp::format::Processor> create_processor() {
    return std::make_unique<StandaloneWorkflowProcessor>();
}

void request_native_quit() {
    pulp::events::MainThreadDispatcher::call_async([] {
        [NSApp stop:nil];
        auto* event = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                         location:NSZeroPoint
                                    modifierFlags:0
                                        timestamp:0
                                     windowNumber:0
                                          context:nil
                                          subtype:0
                                            data1:0
                                            data2:0];
        [NSApp postEvent:event atStart:NO];
    });
}

bool write_ready_file(const std::filesystem::path& ready,
                      const pulp::inspect::InspectorDiscoveryRecord& record,
                      const std::filesystem::path& runtime_path) {
    const auto temporary = ready.string() + ".tmp";
    std::ofstream output(temporary, std::ios::trunc);
    if (!output) return false;
    output << "{\n"
           << "  \"session_id\": \"" << record.session_id << "\",\n"
           << "  \"instance_id\": \"" << record.instance_id << "\",\n"
           << "  \"publication_id\": \"" << record.publication_id << "\",\n"
           << "  \"plugin_id\": \"" << record.plugin_id << "\",\n"
           << "  \"runtime_dir\": \"" << runtime_path.generic_string()
           << "\"\n"
           << "}\n";
    output.close();
    if (!output) return false;
    std::error_code error;
    std::filesystem::rename(temporary, ready, error);
    return !error;
}

bool write_observation_file(const std::filesystem::path& path) {
    if (path.empty()) return true;
    const auto temporary = path.string() + ".tmp";
    std::ofstream output(temporary, std::ios::trunc);
    if (!output) return false;
    output << "{\n"
           << "  \"note_on_count\": "
           << g_note_on_count.load(std::memory_order_acquire) << ",\n"
           << "  \"note_off_count\": "
           << g_note_off_count.load(std::memory_order_acquire) << ",\n"
           << "  \"transport_match\": "
           << (g_transport_match.load(std::memory_order_acquire)
                   ? "true" : "false")
           << "\n}\n";
    output.close();
    if (!output) return false;
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    return !error;
}

bool teardown_is_complete(
    const std::filesystem::path&,
    const pulp::inspect::InspectorDiscoveryRecord& record) {
    std::error_code error;
    if (std::filesystem::exists(record.record_path, error) || error)
        return false;
    if (std::filesystem::exists(record.credential_path, error) || error)
        return false;

    auto ownership_path = record.record_path;
    ownership_path.replace_extension(".lock");
    return std::filesystem::is_regular_file(ownership_path, error) && !error;
}

} // namespace

int main(int argc, char** argv) {
    std::filesystem::path ready_path;
    std::filesystem::path stop_path;
    std::filesystem::path observation_path;
    std::filesystem::path reload_path;
    std::filesystem::path runtime_path =
        pulp::inspect::default_inspector_runtime_directory();
    bool wait_until_stop = false;
    std::string inspector_profile = "develop";
    std::vector<std::string> inspector_capabilities;
    bool runtime_eval = false;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--ready" && i + 1 < argc)
            ready_path = argv[++i];
        else if (argument == "--stop" && i + 1 < argc)
            stop_path = argv[++i];
        else if (argument == "--runtime-dir" && i + 1 < argc)
            runtime_path = argv[++i];
        else if (argument == "--observation" && i + 1 < argc)
            observation_path = argv[++i];
        else if (argument == "--reload" && i + 1 < argc)
            reload_path = argv[++i];
        else if (argument == "--wait-until-stop")
            wait_until_stop = true;
        else if (argument == "--profile" && i + 1 < argc)
            inspector_profile = argv[++i];
        else if (argument == "--capability" && i + 1 < argc)
            inspector_capabilities.emplace_back(argv[++i]);
        else if (argument == "--runtime-eval")
            runtime_eval = true;
        else if (argument == "--no-value-channels")
            g_expose_value_channels = false;
        else if (argument == "--no-reload")
            g_support_reload = false;
    }
    if (ready_path.empty() || stop_path.empty() || runtime_path.empty()) return 2;
    if (::setenv("PULP_INSPECTOR_RUNTIME_DIR",
                 runtime_path.string().c_str(), 1) != 0) {
        return 3;
    }

    std::filesystem::create_directories(ready_path.parent_path());
    g_script_path = ready_path.parent_path() / "standalone-workflow-ui.js";
    {
        std::ofstream script(g_script_path, std::ios::trunc);
        script << R"JS(
console.log("REAL INITIAL SCRIPT LOG");
setBackground("root", "#111827");
setFlex("root", "padding_top", 24);
setFlex("root", "padding_left", 28);
setFlex("root", "padding_right", 28);
setFlex("root", "gap", 18);
createLabel("workflow-title", "REAL STANDALONE INSPECTOR WORKFLOW", "root");
setFontSize("workflow-title", 19);
setTextColor("workflow-title", "#f9fafb");
createRow("workflow-panel", "root");
setFlex("workflow-panel", "height", 128);
setFlex("workflow-panel", "padding_top", 16);
setFlex("workflow-panel", "padding_left", 20);
setFlex("workflow-panel", "gap", 20);
setBackground("workflow-panel", "#2563eb");
createKnob("workflow-gain", "workflow-panel");
setLabel("workflow-gain", "Workflow Gain");
setFlex("workflow-gain", "width", 88);
setFlex("workflow-gain", "height", 96);
bindWidgetToParam("workflow-gain", "Workflow Gain");
)JS";
        if (g_expose_value_channels) script << R"JS(
createMeter("workflow-scalar", "workflow-panel");
bindMeter("workflow-scalar", "value:workflow_scalar");
createSpectrum("workflow-vector", "workflow-panel");
bindScope("workflow-vector", "value:workflow_vector");
)JS";
        script << R"JS(
createLabel("workflow-status", "Live scripted UI and compositor", "workflow-panel");
setFontSize("workflow-status", 14);
setTextColor("workflow-status", "#ffffff");
)JS";
        if (!script) return 4;
    }

    std::atomic<bool> controller_failed{false};
    std::optional<pulp::inspect::InspectorDiscoveryRecord> published_record;
    std::jthread controller([&](std::stop_token stop) {
        const auto discovery_deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(30);
        pulp::inspect::InspectorDiscoveryReader reader(runtime_path);
        std::vector<pulp::inspect::InspectorDiscoveryRecord> records;
        while (!stop.stop_requested()
               && std::chrono::steady_clock::now() < discovery_deadline) {
            records = reader.list();
            std::erase_if(records, [](const auto& record) {
                return record.process_id != static_cast<std::int64_t>(::getpid());
            });
            if (records.size() == 1) break;
            wait_for_external_state_change_poll();
        }
        if (stop.stop_requested()) return;
        if (records.size() != 1
            || !write_ready_file(ready_path, records.front(), runtime_path)) {
            controller_failed.store(true, std::memory_order_release);
            request_native_quit();
            return;
        }
        published_record = records.front();
        pulp::events::MainThreadDispatcher::call_async([] {
            if (auto* scalar = g_scalar_source.load(std::memory_order_acquire))
                (void)scalar->read();
        });

        const auto stop_deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(30);
        bool reload_consumed = false;
        while (!stop.stop_requested()
               && (wait_until_stop
                   || std::chrono::steady_clock::now() < stop_deadline)
               && !std::filesystem::exists(stop_path)) {
            if (!reload_consumed && !reload_path.empty()
                && std::filesystem::exists(reload_path)) {
                g_reload_generation.fetch_add(1, std::memory_order_release);
                reload_consumed = true;
            }
            wait_for_external_state_change_poll();
            if (!observation_path.empty() &&
                g_note_on_count.load(std::memory_order_acquire) > 0 &&
                g_note_off_count.load(std::memory_order_acquire) > 0 &&
                g_transport_match.load(std::memory_order_acquire) &&
                !write_observation_file(observation_path)) {
                controller_failed.store(true, std::memory_order_release);
                request_native_quit();
                return;
            }
        }
        if (stop.stop_requested()) return;
        if (!std::filesystem::exists(stop_path))
            controller_failed.store(true, std::memory_order_release);
        request_native_quit();
    });

    pulp::format::StandaloneApp app(create_processor);
    pulp::format::StandaloneConfig config;
    config.sample_rate = 48'000.0;
    config.buffer_size = 256;
    config.output_channels = 2;
    config.input_channels = 0;
    config.persist_settings = false;
    // Exercise the real GPU/compositor path without presenting a test window.
    // A far-future screenshot deadline keeps StandaloneApp in its hidden-window
    // mode while protocol-driven capture remains available.
    config.headless = true;
    config.screenshot_path = ready_path.string() + ".hidden.png";
    config.screenshot_frame_delay = std::numeric_limits<int>::max();
    config.screenshot_keeps_audio = true;
    config.inspector_profile = inspector_profile;
    config.inspector_capabilities = std::move(inspector_capabilities);
    config.inspector_runtime_eval = runtime_eval;
    app.set_config(config);

    const bool ran = app.run_with_editor(/*use_gpu=*/true);
    controller.request_stop();
    controller.join();

    if (!published_record || !teardown_is_complete(runtime_path, *published_record))
        return 5;
    return ran && !controller_failed.load(std::memory_order_acquire) ? 0 : 6;
}
