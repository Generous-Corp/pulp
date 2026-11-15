// What a static, open plug-in editor costs the host's main thread.
//
// Every AU and CLAP editor installs the bridge's idle pump
// (make_editor_idle_pump) as the plug-in view host's idle callback, and the
// host's link-thread gate dispatches a frame whenever an idle callback exists.
// This suite measures that cost on the CPU plug-in host with a real
// ViewBridge over parameter-bound controls, as a DAW would see it: idle-pump
// calls, frames, process wake-ups and CPU time per second, with no idle
// callback as the floor and continuous repaint as the ceiling.

#include <catch2/catch_test_macros.hpp>

#include <pulp/format/editor_idle_pump.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/format/view_bridge.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/parameter_binding.hpp>
#include <pulp/view/plugin_view_host.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>

#import <AppKit/AppKit.h>

#include <libproc.h>
#include <mach/mach_time.h>
#include <sys/resource.h>
#include <unistd.h>

#include <chrono>
#include <memory>
#include <vector>

using namespace pulp;

namespace {

constexpr int kParams = 8;

class StaticEditorProcessor final : public format::Processor {
  public:
    format::PluginDescriptor descriptor() const override {
        return {"Idle Probe", "Pulp", "com.pulp.idle-probe", "1.0.0",
                format::PluginCategory::Effect};
    }
    void define_parameters(state::StateStore& s) override {
        for (int i = 0; i < kParams; ++i)
            s.add_parameter({static_cast<state::ParamID>(i + 1), "P", "", {0.0f, 1.0f, 0.5f}});
    }
    void prepare(const format::PrepareContext&) override {}
    void process(audio::BufferView<float>&, const audio::BufferView<const float>&,
                 midi::MidiBuffer&, midi::MidiBuffer&, const format::ProcessContext&) override {}
    format::ViewSize view_size() const override {
        return {480, 320, 320, 240, 1024, 768};
    }
    std::unique_ptr<view::View> create_view() override {
        auto root = std::make_unique<BoundView>(*store_);
        return root;
    }
    void attach(state::StateStore& store) {
        store_ = &store;
    }

  private:
    class BoundView final : public view::View {
      public:
        explicit BoundView(state::StateStore& store) {
            for (int i = 0; i < kParams; ++i) {
                auto knob = std::make_unique<view::Knob>();
                knob->set_bounds({float(10 + 55 * i), 10, 50, 50});
                bindings_.push_back(
                    view::bind_parameter(*knob, store, static_cast<state::ParamID>(i + 1)));
                add_child(std::move(knob));
            }
        }

      private:
        std::vector<view::ParameterBinding> bindings_;
    };
    state::StateStore* store_ = nullptr;
};

struct Usage {
    double cpu_seconds = 0;
    std::uint64_t wakeups = 0;
};

Usage process_usage() {
    rusage_info_v4 ri{};
    Usage u;
    if (proc_pid_rusage(getpid(), RUSAGE_INFO_V4, reinterpret_cast<rusage_info_t*>(&ri)) != 0)
        return u;
    mach_timebase_info_data_t tb{};
    mach_timebase_info(&tb);
    const double ns = double(ri.ri_user_time + ri.ri_system_time) * tb.numer / tb.denom;
    u.cpu_seconds = ns / 1e9;
    u.wakeups = ri.ri_interrupt_wkups + ri.ri_pkg_idle_wkups;
    return u;
}

void pump_main_runloop(double seconds) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < end)
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
}

enum class Arm { no_idle_callback, editor_idle_pump, continuous_repaint };

struct Rates {
    double idle_calls = 0;
    double frames = 0;
    double wakeups = 0;
    double cpu_percent = 0;
    bool link_ran = false;
};

Rates measure(Arm arm, double seconds) {
    Rates r;
    @autoreleasepool {
        [NSApplication sharedApplication];
        StaticEditorProcessor processor;
        state::StateStore store;
        processor.attach(store);
        processor.set_state_store(&store);
        processor.define_parameters(store);
        format::ViewBridge bridge(processor, store);
        REQUIRE(bridge.open(nullptr));
        view::View* root = bridge.view();
        REQUIRE(root != nullptr);

        view::PluginViewHost::Options options;
        options.size = {480, 320};
        options.use_gpu = false;
        auto host = view::PluginViewHost::create(*root, options);
        REQUIRE(host != nullptr);

        int idle_calls = 0;
        if (arm == Arm::editor_idle_pump) {
            auto pump = format::make_editor_idle_pump(bridge);
            host->set_idle_callback([&idle_calls, pump] {
                ++idle_calls;
                pump();
            });
        }
        if (arm == Arm::continuous_repaint)
            root->set_continuous_repaint(true);

        int frames = 0;
        view::FrameClock* clock = root->frame_clock();
        REQUIRE(clock != nullptr);
        // The activity channel counts dispatched frames without itself asking
        // for frames; a render subscriber would keep the clock running.
        const int subscription = clock->subscribe_activity([&frames](float) { ++frames; });

        NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 480, 320)
                                                       styleMask:NSWindowStyleMaskBorderless
                                                         backing:NSBackingStoreBuffered
                                                           defer:NO];
        window.releasedWhenClosed = NO;
        [window setContentView:(__bridge NSView*)host->native_handle()];
        bridge.notify_attached();
        // The window exists only so the host gets a display link: it is never
        // ordered front, so nothing appears on screen.
        REQUIRE_FALSE(window.isVisible);

        // Let the mount frames drain, then measure a static editor.
        pump_main_runloop(0.3);
        r.link_ran = frames > 0;
        const int idle0 = idle_calls;
        const int frames0 = frames;
        const Usage u0 = process_usage();
        pump_main_runloop(seconds);
        const Usage u1 = process_usage();
        REQUIRE_FALSE(window.isVisible);
        r.idle_calls = (idle_calls - idle0) / seconds;
        r.frames = (frames - frames0) / seconds;
        r.wakeups = double(u1.wakeups - u0.wakeups) / seconds;
        r.cpu_percent = 100.0 * (u1.cpu_seconds - u0.cpu_seconds) / seconds;

        clock->unsubscribe_activity(subscription);
        host->set_idle_callback({});
        [window setContentView:nil];
        [window close];
        host.reset();
        bridge.close();
    }
    return r;
}

} // namespace

TEST_CASE("idle cost of a static open plug-in editor on the CPU host",
          "[view][mac][idle-pump][performance]") {
    constexpr double kSeconds = 3.0;
    const Rates floor = measure(Arm::no_idle_callback, kSeconds);
    if (!floor.link_ran)
        SKIP("no CVDisplayLink frames in this process (headless): nothing to measure");
    const Rates pumped = measure(Arm::editor_idle_pump, kSeconds);
    const Rates ceiling = measure(Arm::continuous_repaint, kSeconds);
    for (const auto& [name, r] :
         {std::pair{"no idle callback", floor}, std::pair{"editor idle pump", pumped},
          std::pair{"continuous repaint", ceiling}}) {
        WARN(name << ": idle-pump calls " << r.idle_calls << "/s, frames " << r.frames
                  << "/s, process wake-ups " << r.wakeups << "/s, CPU " << r.cpu_percent << "%");
    }
    // Control: continuous repaint must keep frames flowing, or the instrument
    // sees nothing.
    CHECK(ceiling.frames > 20.0);
}
