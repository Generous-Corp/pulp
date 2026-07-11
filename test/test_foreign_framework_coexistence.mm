// Foreign-framework coexistence (PAM WS-6 / G8).
//
// Pulp is designed to share a process with a SECOND native UI framework (a
// JUCE plugin, a host's own AppKit UI, another SDK) when it is embedded via
// PluginViewHost. This test pins the behaviors that make that safe on macOS,
// using a plain non-Pulp NSWindow as the "foreign framework" stand-in — a raw
// NSWindow exercises exactly the AppKit run-loop surfaces (modal/tracking
// modes, focus changes, resize, teardown) a JUCE editor window would.
//
// See docs/guides/foreign-framework-coexistence.md for the full contract.
//
// Two invariants are pinned here:
//
//  A. The CPU (CoreGraphics) window host's idle pump keeps firing while the
//     run loop is in a modal / event-tracking mode — the mode a foreign
//     framework's `runModal:` (or Pulp's own menu/context tracking) installs.
//     This is the regression test for the window_host_mac.mm fix that moves
//     the idle NSTimer from NSDefaultRunLoopMode to NSRunLoopCommonModes.
//     Under the old code the idle callback count stays frozen in a modal mode.
//
//  B. Pulp's pump-agnostic primitives — an events::Timer (its own std::thread,
//     not the run loop) and an audio-render thread (models the host's audio
//     callback) — plus UI hit-testing stay correct across the full lifecycle
//     of the foreign window: open, resize, focus, close. None of them depend
//     on Pulp owning the run loop, so the foreign window churning the main
//     loop must not perturb them.
//
// The test deliberately never calls WindowHost::run_event_loop(): that path
// owns [NSApp run] + menu bar + activation policy and is HOSTILE to a shared
// process. Construction alone gives a real NSWindow-backed host, and we pump
// the run loop by hand exactly like an embedding host would.

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

#include <catch2/catch_test_macros.hpp>

#include <pulp/events/event_loop.hpp>
#include <pulp/events/timer.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/window_host.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

namespace {

using namespace std::chrono_literals;

bool is_main_thread() { return [NSThread isMainThread]; }

// A minimal clickable root. on_click fires on mouse-down when the deepest
// hit view is this one; a childless root is always the deepest hit.
class ClickRoot : public pulp::view::View {
public:
    void paint(pulp::canvas::Canvas&) override {}
    std::atomic<int> clicks{0};
};

// Pump the AppKit run loop in an explicit mode until `deadline`, so we can
// prove what does and does not fire per mode. Returns after the deadline
// regardless of whether any source fired.
void pump_run_loop(NSString* mode, std::chrono::milliseconds budget) {
    const auto end = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < end) {
        @autoreleasepool {
            [[NSRunLoop currentRunLoop]
                runMode:mode
                beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
        }
    }
}

// Create a hidden CPU (CoreGraphics) window host. use_gpu=false selects
// MacWindowHost even when Skia is present — that host is the one whose idle
// pump is a run-loop NSTimer, so it is the subject of invariant A. Returns
// nullptr off the main thread (NSWindow construction is main-thread-only).
std::unique_ptr<pulp::view::WindowHost> make_hidden_cpu_host(pulp::view::View& root) {
    (void)[NSApplication sharedApplication];  // NSWindow needs a shared app
    pulp::view::WindowOptions opts;
    opts.width = 320;
    opts.height = 240;
    opts.title = "Pulp coexistence host";
    opts.initially_hidden = true;
    opts.use_gpu = false;
    return pulp::view::WindowHost::create(root, opts);
}

}  // namespace

TEST_CASE("Pulp CPU-host idle pump fires under a foreign modal run-loop mode",
          "[coexistence][window-host][mac]") {
    if (!is_main_thread()) {
        WARN("skipped: not on the main thread");
        return;
    }

    ClickRoot root;
    auto host = make_hidden_cpu_host(root);
    REQUIRE(host != nullptr);

    std::atomic<int> idle_ticks{0};
    host->set_idle_callback([&idle_ticks] {
        idle_ticks.fetch_add(1, std::memory_order_relaxed);
    });

    // A foreign framework opening a modal panel installs NSModalPanelRunLoopMode
    // (part of the common-modes set). Order a raw NSWindow front to mirror the
    // AppKit state a foreign window would create.
    NSWindow* foreign = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 200, 150)
                  styleMask:NSWindowStyleMaskTitled
                    backing:NSBackingStoreBuffered
                      defer:YES];
    [foreign orderFront:nil];

    // Sanity: the pump fires in the ordinary default mode.
    pump_run_loop(NSDefaultRunLoopMode, 200ms);
    const int after_default = idle_ticks.load(std::memory_order_relaxed);
    REQUIRE(after_default > 0);

    // The crux: keep pumping ONLY in the modal-panel mode. With the timer in
    // NSRunLoopCommonModes it keeps ticking; with the old default-mode-only
    // scheduling it would freeze here.
    pump_run_loop(NSModalPanelRunLoopMode, 300ms);
    const int after_modal = idle_ticks.load(std::memory_order_relaxed);
    REQUIRE(after_modal > after_default);

    // Also survives event-tracking mode (the mode a drag / menu tracking loop
    // uses — Pulp's own or the foreign framework's).
    pump_run_loop(NSEventTrackingRunLoopMode, 200ms);
    REQUIRE(idle_ticks.load(std::memory_order_relaxed) > after_modal);

    [foreign close];
}

TEST_CASE("Pulp timers, UI, and audio stay live across a foreign window's lifecycle",
          "[coexistence][window-host][mac]") {
    if (!is_main_thread()) {
        WARN("skipped: not on the main thread");
        return;
    }

    ClickRoot root;
    root.on_click = [&root] { root.clicks.fetch_add(1, std::memory_order_relaxed); };
    auto host = make_hidden_cpu_host(root);
    REQUIRE(host != nullptr);

    // Pulp's pump-agnostic timer: owns its own thread via EventLoop, so it must
    // advance regardless of what the AppKit run loop is doing.
    pulp::events::EventLoop loop;
    std::atomic<int> timer_ticks{0};
    pulp::events::Timer timer(loop, 10ms, [&timer_ticks] {
        timer_ticks.fetch_add(1, std::memory_order_relaxed);
    });
    timer.start();

    // A background audio-render thread models the host's real-time audio
    // callback: it applies a deterministic transform (unity gain here) and
    // publishes both a progress counter and a correctness flag. Like Pulp's
    // real audio thread it is a plain std::thread, wholly independent of the
    // run loop — the point is to prove the foreign UI churn cannot corrupt or
    // stall it.
    std::atomic<bool> audio_run{true};
    std::atomic<long> audio_blocks{0};
    std::atomic<bool> audio_ok{true};
    std::thread audio([&] {
        constexpr int kBlock = 64;
        float in[kBlock];
        float out[kBlock];
        for (int i = 0; i < kBlock; ++i) in[i] = static_cast<float>(i) * 0.01f;
        while (audio_run.load(std::memory_order_acquire)) {
            for (int i = 0; i < kBlock; ++i) out[i] = in[i];  // unity-gain DSP
            for (int i = 0; i < kBlock; ++i) {
                if (out[i] != in[i]) audio_ok.store(false, std::memory_order_relaxed);
            }
            audio_blocks.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(1ms);
        }
    });

    auto baseline_timer = timer_ticks.load(std::memory_order_relaxed);
    auto baseline_audio = audio_blocks.load(std::memory_order_relaxed);

    // ── Foreign window lifecycle: open ────────────────────────────────────
    NSWindow* foreign = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 240, 180)
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskResizable)
                    backing:NSBackingStoreBuffered
                      defer:YES];
    [foreign orderFront:nil];
    pump_run_loop(NSDefaultRunLoopMode, 80ms);

    // Drive a Pulp UI interaction while the foreign window is up.
    root.set_bounds({0, 0, 320, 240});
    root.simulate_click({160, 120});
    REQUIRE(root.clicks.load(std::memory_order_relaxed) == 1);

    // ── resize ────────────────────────────────────────────────────────────
    [foreign setFrame:NSMakeRect(0, 0, 400, 300) display:YES];
    pump_run_loop(NSDefaultRunLoopMode, 80ms);

    // ── focus ─────────────────────────────────────────────────────────────
    [foreign makeFirstResponder:[foreign contentView]];
    [foreign orderFront:nil];
    pump_run_loop(NSEventTrackingRunLoopMode, 80ms);
    root.set_bounds({0, 0, 320, 240});
    root.simulate_click({80, 60});
    REQUIRE(root.clicks.load(std::memory_order_relaxed) == 2);

    // ── close ─────────────────────────────────────────────────────────────
    [foreign orderOut:nil];
    [foreign close];
    pump_run_loop(NSDefaultRunLoopMode, 80ms);

    // Give the off-loop primitives a moment independent of the run loop.
    std::this_thread::sleep_for(60ms);

    timer.stop();
    audio_run.store(false, std::memory_order_release);
    audio.join();

    // Pulp's own-thread timer advanced throughout the foreign lifecycle.
    REQUIRE(timer_ticks.load(std::memory_order_relaxed) > baseline_timer);
    // The audio thread never stalled and never produced wrong output.
    REQUIRE(audio_blocks.load(std::memory_order_relaxed) > baseline_audio);
    REQUIRE(audio_ok.load(std::memory_order_relaxed));
    // UI interaction routed correctly both before and after the foreign churn.
    REQUIRE(root.clicks.load(std::memory_order_relaxed) == 2);
}
