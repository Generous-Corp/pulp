#include <pulp/events/main_thread_dispatcher.hpp>
#include <pulp/format/detail/standalone_inspector.hpp>
#include <pulp/format/standalone.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/widgets.hpp>

#import <AppKit/AppKit.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace {

class InspectorProcessProcessor final : public pulp::format::Processor {
public:
    pulp::format::PluginDescriptor descriptor() const override {
        return {
            .name = "Inspector Process Fixture",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.test.inspector-process-fixture",
            .version = "1.0.0",
            .category = pulp::format::PluginCategory::Effect,
            .input_buses = {},
            .output_buses = {{"Output", 2}},
        };
    }

    void define_parameters(pulp::state::StateStore& store) override {
        store.add_parameter({
            .id = 1,
            .name = "Gain",
            .unit = "dB",
            .range = {-18.0f, 18.0f, 0.0f},
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
        return {320, 180, 320, 180, 320, 180};
    }

    std::unique_ptr<pulp::view::View> create_view() override {
        using namespace pulp::view;
        auto root = std::make_unique<View>();
        root->set_theme(Theme::dark());
        root->set_background_color(pulp::canvas::Color::rgba8(16, 19, 29, 255));
        root->flex().direction = FlexDirection::column;
        root->flex().padding = 16.0f;
        root->flex().gap = 12.0f;

        auto title = std::make_unique<Label>("LIVE STANDALONE INSPECTOR");
        title->set_font_size(18.0f);
        title->set_font_weight(700);
        title->flex().preferred_height = 30.0f;
        root->add_child(std::move(title));

        auto panel = std::make_unique<View>();
        panel->set_background_color(pulp::canvas::Color::rgba8(74, 126, 255, 255));
        panel->flex().preferred_height = 64.0f;
        root->add_child(std::move(panel));

        auto status = std::make_unique<Label>("Authenticated compositor capture");
        status->set_font_size(13.0f);
        status->flex().preferred_height = 24.0f;
        root->add_child(std::move(status));
        return root;
    }
};

std::unique_ptr<pulp::format::Processor> create_processor() {
    return std::make_unique<InspectorProcessProcessor>();
}

} // namespace

int main(int argc, char** argv) {
    bool dsp_only = false;
    std::string exit_screenshot;
    int frames = 180;
    int app_quit_after_ms = 0;
    std::string quit_on_request_arm;
    std::string close_on_request_arm;
    std::string accepted_marker;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--dsp-only") {
            dsp_only = true;
        } else if (arg == "--exit-screenshot" && i + 1 < argc) {
            exit_screenshot = argv[++i];
        } else if (arg == "--frames" && i + 1 < argc) {
            frames = std::atoi(argv[++i]);
        } else if (arg == "--app-quit-after-ms" && i + 1 < argc) {
            app_quit_after_ms = std::atoi(argv[++i]);
        } else if (arg == "--quit-on-request-arm" && i + 1 < argc) {
            quit_on_request_arm = argv[++i];
        } else if (arg == "--close-on-request-arm" && i + 1 < argc) {
            close_on_request_arm = argv[++i];
        } else if (arg == "--accepted-marker" && i + 1 < argc) {
            accepted_marker = argv[++i];
        }
    }

    pulp::format::StandaloneApp app(create_processor);
    pulp::format::StandaloneConfig config;
    config.sample_rate = 48'000.0;
    config.buffer_size = 256;
    config.output_channels = 2;
    config.input_channels = 0;
    config.persist_settings = false;
    app.set_config(config);

    if (dsp_only) {
        if (!app.start()) return 2;
        app.stop();
        return 0;
    }

    if ((exit_screenshot.empty() || frames <= 0) && app_quit_after_ms <= 0 &&
        quit_on_request_arm.empty() && close_on_request_arm.empty())
        return 3;
    if (!exit_screenshot.empty()) {
        config.screenshot_path = std::move(exit_screenshot);
        config.screenshot_frame_delay = frames;
    }
    app.set_config(config);
    const auto request_arm =
        !quit_on_request_arm.empty() ? quit_on_request_arm : close_on_request_arm;
    const bool quit_on_request = !quit_on_request_arm.empty();
    auto hook_fired = std::make_shared<std::atomic<bool>>(false);
    auto action_executed = std::make_shared<std::atomic<bool>>(false);
    auto held_task_mutex = std::make_shared<std::mutex>();
    auto held_task = std::make_shared<std::function<void()>>();
    auto stop_held_task_releaser = std::make_shared<std::atomic<bool>>(false);
    std::thread held_task_releaser;
    if (!request_arm.empty()) {
        pulp::format::detail::set_standalone_inspector_rpc_post_override_for_testing(
            [request_arm, accepted_marker, quit_on_request, hook_fired, action_executed,
             held_task_mutex, held_task](std::function<void()>& task) -> std::optional<bool> {
                if (hook_fired->load(std::memory_order_acquire) ||
                    !std::filesystem::exists(request_arm)) {
                    return std::nullopt;
                }
                bool expected = false;
                if (!hook_fired->compare_exchange_strong(expected, true,
                                                         std::memory_order_acq_rel)) {
                    return std::nullopt;
                }
                {
                    std::lock_guard lock(*held_task_mutex);
                    *held_task = std::move(task);
                }
                const bool action_posted = pulp::events::MainThreadDispatcher::call_async(
                    [quit_on_request, action_executed] {
                        if (quit_on_request) {
                            [NSApp stop:nil];
                        } else if (auto* window = [[NSApp windows] firstObject]) {
                            [window performClose:nil];
                        }
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
                        action_executed->store(true, std::memory_order_release);
                    });
                if (action_posted && !accepted_marker.empty()) {
                    std::ofstream marker(accepted_marker);
                    marker << "accepted\n";
                }
                return action_posted;
            });
        held_task_releaser = std::thread([action_executed, held_task_mutex, held_task,
                                          stop_held_task_releaser] {
            while (!stop_held_task_releaser->load(std::memory_order_acquire) &&
                   !action_executed->load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (stop_held_task_releaser->load(std::memory_order_acquire))
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            std::function<void()> task;
            {
                std::lock_guard lock(*held_task_mutex);
                task = std::move(*held_task);
            }
            if (task)
                pulp::events::MainThreadDispatcher::call_async(std::move(task));
        });
    }
    auto stop_app_quit = std::make_shared<std::atomic<bool>>(false);
    std::thread app_quit;
    if (app_quit_after_ms > 0) {
        app_quit = std::thread([app_quit_after_ms, stop_app_quit] {
            while (!stop_app_quit->load(std::memory_order_acquire) &&
                   !pulp::events::MainThreadDispatcher::has_backend()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(app_quit_after_ms);
            while (!stop_app_quit->load(std::memory_order_acquire) &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (!stop_app_quit->load(std::memory_order_acquire)) {
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
        });
    }
    const bool result = app.run_with_editor(/*use_gpu=*/true);
    stop_app_quit->store(true, std::memory_order_release);
    stop_held_task_releaser->store(true, std::memory_order_release);
    if (app_quit.joinable())
        app_quit.join();
    if (held_task_releaser.joinable())
        held_task_releaser.join();
    pulp::format::detail::set_standalone_inspector_rpc_post_override_for_testing({});
    return result ? 0 : 4;
}
