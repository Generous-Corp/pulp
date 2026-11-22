// MainThreadScope and MainThreadTimer on Windows with no host run loop
// registered: a message-only window on the creating thread carries posts
// (PostMessage) and timers (SetTimer). The test thread pumps messages the way a
// host's UI thread does; nothing runs unless it does.

#include <pulp/platform/main_thread.hpp>

#include <catch2/catch_test_macros.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>
#include <vector>

using namespace pulp::platform;
using std::chrono::milliseconds;

namespace {

// Dispatch this thread's messages until `done` holds or `limit` passes.
bool pump_until(const std::function<bool()>& done, milliseconds limit = milliseconds(3000)) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (done())
            return true;
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 5, QS_ALLINPUT);
    }
}

void pump_for(milliseconds span) {
    pump_until([] { return false; }, span);
}

// The fallback window, found the way a stray one would be: by class, among the
// message-only windows.
HWND find_fallback_window() {
    return FindWindowExW(HWND_MESSAGE, nullptr, detail::win32_fallback_class_name(), nullptr);
}

bool fallback_class_registered() {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&detail::win32_fallback_class_name), &module);
    WNDCLASSEXW info{};
    info.cbSize = sizeof info;
    return GetClassInfoExW(module, detail::win32_fallback_class_name(), &info) != 0;
}

// A host loop that only counts what it is asked to do.
class CountingLoop final : public HostRunLoop {
  public:
    bool on_ui_thread() const override {
        return std::this_thread::get_id() == ui_;
    }
    std::uint64_t add_timer(milliseconds, std::function<void()>) override {
        ++timers_added;
        return next_++;
    }
    void remove_timer(std::uint64_t) override {}

    int timers_added = 0;

  private:
    std::thread::id ui_ = std::this_thread::get_id();
    std::uint64_t next_ = 1;
};

} // namespace

TEST_CASE("with no host loop a timer and posts run in order on the creating thread",
          "[platform][main-thread][win32]") {
    REQUIRE_FALSE(main_thread_supported());
    REQUIRE(find_fallback_window() == nullptr); // control: nothing before a scope
    const auto ui = std::this_thread::get_id();

    std::vector<int> order;
    std::vector<std::thread::id> threads;
    {
        MainThreadScope scope;
        REQUIRE(main_thread_supported());
        REQUIRE(is_main_thread());
        REQUIRE(find_fallback_window() != nullptr); // control: the fallback is the carrier

        int fires = 0;
        MainThreadTimer timer(scope, [&] {
            ++fires;
            threads.push_back(std::this_thread::get_id());
        });
        timer.fire_every(milliseconds(15));
        REQUIRE(timer.pending());

        std::atomic<bool> all_accepted{true};
        std::thread producer([&] {
            for (int i = 0; i < 20; ++i)
                if (!scope.post([&order, &threads, i] {
                        order.push_back(i);
                        threads.push_back(std::this_thread::get_id());
                    }))
                    all_accepted = false;
        });
        producer.join();
        REQUIRE(all_accepted.load());

        REQUIRE(pump_until([&] { return order.size() == 20 && fires >= 3; }));
        for (int i = 0; i < 20; ++i)
            REQUIRE(order[static_cast<std::size_t>(i)] == i);
        for (const auto& id : threads)
            REQUIRE(id == ui);
    }
}

TEST_CASE("nothing runs until the owner thread pumps, and other threads are refused",
          "[platform][main-thread][win32]") {
    MainThreadScope scope;
    REQUIRE(main_thread_supported());

    std::atomic<int> ran{0};
    MainThreadTimer timer(scope, [&] { ++ran; });
    timer.fire_after(milliseconds(10));
    REQUIRE(scope.post([&] { ++ran; }));

    std::this_thread::sleep_for(milliseconds(100)); // not pumping
    REQUIRE(ran.load() == 0);

    // A scope made on another thread while the window lives is refused rather
    // than given a window nobody pumps.
    bool refused = false;
    bool other_is_main = true;
    std::thread other([&] {
        MainThreadScope foreign;
        refused = !foreign.post([] {});
        other_is_main = is_main_thread();
    });
    other.join();
    REQUIRE(refused);
    REQUIRE_FALSE(other_is_main);

    REQUIRE(pump_until([&] { return ran.load() == 2; }));
}

TEST_CASE("a registered host loop is used and the fallback stays out of it",
          "[platform][main-thread][win32]") {
    CountingLoop host;
    const HostRunLoopToken token = register_host_run_loop(host);
    REQUIRE(token != 0);
    {
        MainThreadScope scope;
        int fires = 0;
        MainThreadTimer timer(scope, [&] { ++fires; });
        timer.fire_every(milliseconds(15));
        REQUIRE(host.timers_added >= 1); // the host's polling timer and the timer itself
        REQUIRE(timer.pending());
        REQUIRE_FALSE(detail::win32_fallback_live());
        REQUIRE(find_fallback_window() == nullptr);

        // Pumping drives nothing: the host owns this scope's work.
        pump_for(milliseconds(60));
        REQUIRE(fires == 0);
    }
    REQUIRE(unregister_host_run_loop(token));
    REQUIRE_FALSE(main_thread_supported());
}

TEST_CASE("destroying every scope leaves no window, class or timer behind",
          "[platform][main-thread][win32]") {
    std::atomic<int> fires{0};
    {
        MainThreadScope first;
        MainThreadScope second;
        MainThreadTimer a(first, [&] { ++fires; });
        MainThreadTimer b(second, [&] { ++fires; });
        a.fire_every(milliseconds(10));
        b.fire_every(milliseconds(10));
        REQUIRE(find_fallback_window() != nullptr);
        REQUIRE(fallback_class_registered()); // control: the probe can see a live class
        REQUIRE(pump_until([&] { return fires.load() >= 2; }));
    }
    REQUIRE_FALSE(detail::win32_fallback_live());
    REQUIRE_FALSE(main_thread_supported());
    REQUIRE(find_fallback_window() == nullptr);
    REQUIRE_FALSE(fallback_class_registered());

    const int after_teardown = fires.load();
    pump_for(milliseconds(80));
    REQUIRE(fires.load() == after_teardown);

    // The next scope starts a fresh window.
    {
        MainThreadScope again;
        bool ran = false;
        REQUIRE(again.post([&] { ran = true; }));
        REQUIRE(pump_until([&] { return ran; }));
    }
    REQUIRE(find_fallback_window() == nullptr);
}

TEST_CASE("a one-shot fires once and a cancelled timer never fires",
          "[platform][main-thread][win32]") {
    MainThreadScope scope;
    int once = 0;
    int cancelled = 0;
    MainThreadTimer one(scope, [&] { ++once; });
    MainThreadTimer gone(scope, [&] { ++cancelled; });
    one.fire_after(milliseconds(10));
    gone.fire_after(milliseconds(10));
    gone.cancel();
    REQUIRE(pump_until([&] { return once == 1; }));
    pump_for(milliseconds(80));
    REQUIRE(once == 1);
    REQUIRE(cancelled == 0);
    REQUIRE_FALSE(one.pending());
}

TEST_CASE("a task may destroy its own scope", "[platform][main-thread][win32]") {
    auto scope = std::make_unique<MainThreadScope>();
    bool ran = false;
    bool dropped_ran = false;
    MainThreadScope::Handle handle = scope->handle();
    REQUIRE(scope->post([&] {
        ran = true;
        scope.reset();
    }));
    REQUIRE(handle.post([&] { dropped_ran = true; }));
    REQUIRE(pump_until([&] { return ran; }));
    pump_for(milliseconds(30));
    REQUIRE_FALSE(dropped_ran);
    REQUIRE_FALSE(handle.alive());
    REQUIRE(find_fallback_window() == nullptr);
}
