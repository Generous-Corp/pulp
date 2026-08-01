#include <pulp/events/main_thread_dispatcher.hpp>
#include <pulp/format/standalone.hpp>
#include <pulp/inspect/discovery.hpp>
#include <pulp/view/scripted_ui.hpp>

#import <AppKit/AppKit.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

std::filesystem::path g_script_path;

class StandaloneWorkflowProcessor final : public pulp::format::Processor {
public:
    pulp::format::PluginDescriptor descriptor() const override {
        return {
            .name = "Standalone Inspector Workflow Fixture",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.test.standalone-inspector-workflow",
            .version = "1.0.0",
            .category = pulp::format::PluginCategory::Effect,
            .input_buses = {},
            .output_buses = {{"Output", 2}},
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
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {
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
            pulp::view::ScriptedUiOptions{.script_path = g_script_path});
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

private:
    pulp::state::StateStore* store_ = nullptr;
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

bool teardown_is_complete(
    const std::filesystem::path& runtime_path,
    const pulp::inspect::InspectorDiscoveryRecord& record) {
    std::error_code error;
    if (std::filesystem::exists(record.record_path, error) || error)
        return false;
    if (std::filesystem::exists(record.credential_path, error) || error)
        return false;

    auto ownership_path = record.record_path;
    ownership_path.replace_extension(".lock");
    if (!std::filesystem::is_regular_file(ownership_path, error) || error)
        return false;
    for (std::filesystem::directory_iterator iterator(runtime_path, error), end;
         !error && iterator != end; iterator.increment(error)) {
        if (iterator->path() != ownership_path
            || !iterator->is_regular_file(error) || error) {
            return false;
        }
    }
    return !error;
}

} // namespace

int main(int argc, char** argv) {
    std::filesystem::path ready_path;
    std::filesystem::path stop_path;
    std::filesystem::path runtime_path =
        pulp::inspect::default_inspector_runtime_directory();
    bool wait_until_stop = false;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--ready" && i + 1 < argc)
            ready_path = argv[++i];
        else if (argument == "--stop" && i + 1 < argc)
            stop_path = argv[++i];
        else if (argument == "--runtime-dir" && i + 1 < argc)
            runtime_path = argv[++i];
        else if (argument == "--wait-until-stop")
            wait_until_stop = true;
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
            if (records.size() == 1) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (stop.stop_requested()) return;
        if (records.size() != 1
            || !write_ready_file(ready_path, records.front(), runtime_path)) {
            controller_failed.store(true, std::memory_order_release);
            request_native_quit();
            return;
        }
        published_record = records.front();

        const auto stop_deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!stop.stop_requested()
               && (wait_until_stop
                   || std::chrono::steady_clock::now() < stop_deadline)
               && !std::filesystem::exists(stop_path)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
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
    config.inspector_profile = "develop";
    app.set_config(config);

    const bool ran = app.run_with_editor(/*use_gpu=*/true);
    controller.request_stop();
    controller.join();

    if (!published_record || !teardown_is_complete(runtime_path, *published_record))
        return 5;
    return ran && !controller_failed.load(std::memory_order_acquire) ? 0 : 6;
}
