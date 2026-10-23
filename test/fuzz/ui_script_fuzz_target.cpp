// libFuzzer entry point over a scripted UI document (ui.js) evaluated through
// the widget bridge in a QuickJS realm.
//
// A design bundle's ui.js is attacker-controlled the moment a plugin loads a
// downloaded pack, so the whole script is the input. Every iteration builds a
// fresh engine, root view and store, grants the script no effectful bridge
// capabilities (the untrusted posture), caps the realm's heap where the
// backend can enforce one, and arms a watchdog that interrupts the realm after
// a short deadline so a `while (true)` is a rejected script rather than a
// libFuzzer timeout report. The bridge reports a bad script as a
// std::runtime_error (its documented failure path), which is not a finding;
// any other exception, bad_alloc and logic errors included, propagates so
// libFuzzer reports it.

#include <pulp/state/store.hpp>
#include <pulp/view/js_engine.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widget_bridge.hpp>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <typeinfo>

namespace {

constexpr auto kScriptDeadline = std::chrono::milliseconds(500);
constexpr std::size_t kHeapLimitBytes = std::size_t{64} * 1024 * 1024;

template <typename Engine> void cap_heap(Engine& engine) {
    if constexpr (requires { engine.set_memory_limit(std::size_t{}); })
        (void)engine.set_memory_limit(kHeapLimitBytes);
}

// Runs `body` with a watchdog that interrupts the engine at the deadline.
template <typename Body> void run_with_deadline(pulp::view::ScriptEngine& engine, Body&& body) {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    bool interrupt_issued = false;
    const auto deadline = std::chrono::steady_clock::now() + kScriptDeadline;
    std::thread watchdog([&] {
        std::unique_lock<std::mutex> lock(mutex);
        if (cv.wait_until(lock, deadline, [&] { return done; }))
            return;
        interrupt_issued = true;
        engine.request_interrupt();
    });

    try {
        body();
    } catch (const std::runtime_error& error) {
        // Exactly the bridge's script-error type; a derived exception is not
        // a rejected script and is rethrown.
        if (typeid(error) != typeid(std::runtime_error))
            throw;
    }

    {
        std::lock_guard<std::mutex> lock(mutex);
        done = true;
    }
    cv.notify_all();
    watchdog.join();
    if (interrupt_issued)
        engine.clear_pending_interrupt();
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string code(reinterpret_cast<const char*>(data), size);

    pulp::view::ScriptEngine engine(pulp::view::JsEngineType::quickjs);
    cap_heap(engine);

    pulp::view::View root;
    root.set_bounds({0, 0, 320, 240});
    pulp::state::StateStore store;
    store.add_parameter({.id = 1, .name = "gain", .range = {0.0f, 1.0f, 0.5f, 0.01f}});
    store.add_parameter({.id = 2, .name = "mix", .range = {0.0f, 100.0f, 50.0f, 1.0f}});

    pulp::view::WidgetBridge bridge(engine, root, store, nullptr, pulp::view::CapabilitySet{});
    run_with_deadline(engine, [&] {
        bridge.load_script(code);
        // Timers and animation frames the script scheduled run from the host
        // frame pump; two passes reach a callback and whatever it re-arms.
        bridge.service_frame_callbacks();
        bridge.service_frame_callbacks();
    });
    return 0;
}
