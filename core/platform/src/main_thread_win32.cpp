// The Windows fallback loop behind MainThreadScope and MainThreadTimer. See
// main_thread_win32.hpp.
//
// Thread: the window belongs to the thread that first acquires it, and every
// timer and post runs only while that thread pumps messages (a plug-in
// editor's UI thread does, through the host's message loop). A second thread
// is refused rather than given a window it never pumps.
//
// Lifetime: the window and its class exist while a scope uses them. The class
// name carries this module's handle, so two binaries in one process never
// share a window or a window procedure, and the class is unregistered with the
// window, so nothing in this module is reachable from the system once the last
// scope is gone, which is what lets the module unload.
//
// Modal loops: WM_TIMER and posted messages are dispatched by the menu and
// move/size modal loops, so timers keep firing there.
//
// Coalescing: any number of posts waiting on this loop share one window
// message.

#include "main_thread_win32.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cwchar>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace pulp::platform::detail {

namespace {

constexpr UINT kDrainMessage = WM_APP + 1;

HMODULE this_module() noexcept {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&this_module), &module);
    return module;
}

class Win32FallbackLoop final : public HostRunLoop {
  public:
    static Win32FallbackLoop& instance() noexcept {
        // Never destroyed: a scope in a plug-in's statics may outlive main().
        static Win32FallbackLoop* loop = new Win32FallbackLoop;
        return *loop;
    }

    bool on_ui_thread() const override {
        return owner_.load(std::memory_order_acquire) == GetCurrentThreadId();
    }

    std::uint64_t add_timer(std::chrono::milliseconds period, std::function<void()> tick) override {
        if (!on_ui_thread() || !tick)
            return 0;
        HWND window = hwnd_.load(std::memory_order_acquire);
        if (window == nullptr)
            return 0;
        const UINT_PTR id = next_timer_++;
        const auto ms = static_cast<UINT>(std::max<long long>(period.count(), 1));
        // SetTimer raises anything below USER_TIMER_MINIMUM (10 ms) to it.
        if (SetTimer(window, id, ms, nullptr) == 0)
            return 0;
        timers_[id] = std::make_shared<std::function<void()>>(std::move(tick));
        return id;
    }

    void remove_timer(std::uint64_t id) override {
        auto it = timers_.find(static_cast<UINT_PTR>(id));
        if (it == timers_.end())
            return;
        timers_.erase(it);
        HWND window = hwnd_.load(std::memory_order_acquire);
        // KillTimer only works from the owner thread. From any other the entry
        // above is gone, so a stray WM_TIMER finds nothing to run.
        if (window != nullptr && on_ui_thread())
            KillTimer(window, static_cast<UINT_PTR>(id));
    }

    bool acquire() {
        const DWORD me = GetCurrentThreadId();
        HWND window = hwnd_.load(std::memory_order_acquire);
        if (window != nullptr) {
            if (owner_.load(std::memory_order_acquire) == me)
                return true;
            if (IsWindow(window))
                return false; // another live thread owns it
            // The owner thread ended, which destroyed its window: start over.
            forget();
        }
        return create();
    }

    void release() {
        HWND window = hwnd_.load(std::memory_order_acquire);
        if (window == nullptr || !on_ui_thread())
            return; // off the owner thread nothing can be destroyed here
        for (auto& entry : timers_)
            KillTimer(window, entry.first);
        timers_.clear();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_.clear();
            wake_posted_ = false;
            // Posts that race this see no window and are refused.
            hwnd_.store(nullptr, std::memory_order_release);
        }
        owner_.store(0, std::memory_order_release);
        DestroyWindow(window);
        UnregisterClassW(class_name_, this_module());
        class_registered_ = false;
    }

    bool live() const noexcept {
        return hwnd_.load(std::memory_order_acquire) != nullptr;
    }

    const wchar_t* class_name() noexcept {
        name_class();
        return class_name_;
    }

    bool request_drain(std::function<void()> drain) {
        std::lock_guard<std::mutex> lock(mutex_);
        HWND window = hwnd_.load(std::memory_order_acquire);
        if (window == nullptr)
            return false;
        pending_.push_back(std::move(drain));
        if (!wake_posted_) {
            if (PostMessageW(window, kDrainMessage, 0, 0))
                wake_posted_ = true;
        }
        return true;
    }

  private:
    // The window procedure. Runs on the owner thread only.
    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        Win32FallbackLoop& self = instance();
        if (message == WM_TIMER) {
            auto it = self.timers_.find(static_cast<UINT_PTR>(wparam));
            if (it != self.timers_.end()) {
                // Hold the callback: it may remove its own timer.
                std::shared_ptr<std::function<void()>> fn = it->second;
                (*fn)();
            }
            return 0;
        }
        if (message == kDrainMessage) {
            std::vector<std::function<void()>> batch;
            {
                std::lock_guard<std::mutex> lock(self.mutex_);
                self.wake_posted_ = false;
                batch.swap(self.pending_);
            }
            for (auto& drain : batch)
                drain();
            return 0;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    void name_class() noexcept {
        if (class_name_[0] != L'\0')
            return;
        // One class per binary: the module handle is unique among loaded
        // modules.
        std::swprintf(class_name_, sizeof class_name_ / sizeof class_name_[0], L"PulpMainThread_%p",
                      static_cast<void*>(this_module()));
    }

    bool create() {
        name_class();
        HMODULE module = this_module();
        WNDCLASSEXW cls{};
        cls.cbSize = sizeof cls;
        cls.lpfnWndProc = &Win32FallbackLoop::window_proc;
        cls.hInstance = module;
        cls.lpszClassName = class_name_;
        if (!RegisterClassExW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return false;
        class_registered_ = true;
        HWND window = CreateWindowExW(0, class_name_, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                      module, nullptr);
        if (window == nullptr) {
            UnregisterClassW(class_name_, module);
            class_registered_ = false;
            return false;
        }
        owner_.store(GetCurrentThreadId(), std::memory_order_release);
        hwnd_.store(window, std::memory_order_release);
        return true;
    }

    // Drop state left by a thread that ended without releasing.
    void forget() {
        timers_.clear();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_.clear();
            wake_posted_ = false;
            hwnd_.store(nullptr, std::memory_order_release);
        }
        owner_.store(0, std::memory_order_release);
        if (class_registered_) {
            UnregisterClassW(class_name_, this_module());
            class_registered_ = false;
        }
    }

    std::atomic<HWND> hwnd_{nullptr};
    std::atomic<DWORD> owner_{0};
    std::mutex mutex_; // guards pending_, wake_posted_ and the hwnd_ hand-off
    std::vector<std::function<void()>> pending_;
    bool wake_posted_ = false;
    // Owner thread only.
    std::map<UINT_PTR, std::shared_ptr<std::function<void()>>> timers_;
    UINT_PTR next_timer_ = 1;
    bool class_registered_ = false;
    wchar_t class_name_[64] = {};
};

} // namespace

HostRunLoop& win32_fallback_loop() noexcept {
    return Win32FallbackLoop::instance();
}

bool win32_fallback_acquire() {
    return Win32FallbackLoop::instance().acquire();
}

void win32_fallback_release() {
    Win32FallbackLoop::instance().release();
}

bool win32_fallback_on_ui_thread() noexcept {
    return Win32FallbackLoop::instance().live() && Win32FallbackLoop::instance().on_ui_thread();
}

bool win32_fallback_request_drain(std::function<void()> drain) {
    return Win32FallbackLoop::instance().request_drain(std::move(drain));
}

bool win32_fallback_live() noexcept {
    return Win32FallbackLoop::instance().live();
}

const wchar_t* win32_fallback_class_name() noexcept {
    return Win32FallbackLoop::instance().class_name();
}

} // namespace pulp::platform::detail
