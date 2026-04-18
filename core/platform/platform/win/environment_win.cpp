// Windows adapter for the unified Environment API (#342).
//
// Pure Win32 — no UWP/WinRT — so this works in every plugin host
// (DAW process, standalone, AAX wrapper) regardless of WinRT init.
//
// Two observers feed Environment::publish():
//
//   1. A hidden message-only window (HWND_MESSAGE parent) whose
//      WindowProc translates WM_ACTIVATEAPP → LifecycleState
//      (foreground when wParam != 0, inactive otherwise) and
//      WM_DPICHANGED / WM_DISPLAYCHANGE / WM_SETTINGCHANGE → display
//      and color-scheme refresh.
//
//   2. A worker thread that
//        a. waits on RegNotifyChangeKeyValue() for changes to
//           HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\
//           Personalize\AppsUseLightTheme (light/dark mode toggle), and
//        b. polls GlobalMemoryStatusEx() every 5 s to map
//           dwMemoryLoad → MemoryPressure tier.
//      The thread merges both signals into one publish() per wake-up
//      so listeners see a single coherent snapshot.
//
// Safe-area / keyboard / orientation are mobile-only and stay at the
// EnvironmentState defaults on Windows desktop, per the
// EnvironmentState contract.

#if defined(_WIN32)

#include <pulp/platform/environment.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>

namespace pulp::platform {

namespace {

// ── Display detection ─────────────────────────────────────────────────

// GetDpiForSystem is on Windows 10 1607+. We look it up dynamically so
// the adapter still links on older toolchains/SDKs that don't expose
// it; if it isn't present we fall back to GetDeviceCaps(LOGPIXELSX).
UINT query_system_dpi() noexcept {
    using GetDpiForSystemFn = UINT (WINAPI*)();
    static auto fn = []() -> GetDpiForSystemFn {
        if (HMODULE h = GetModuleHandleW(L"user32.dll")) {
            return reinterpret_cast<GetDpiForSystemFn>(
                GetProcAddress(h, "GetDpiForSystem"));
        }
        return nullptr;
    }();
    if (fn) return fn();
    HDC dc = GetDC(nullptr);
    UINT dpi = 96;
    if (dc) {
        int v = GetDeviceCaps(dc, LOGPIXELSX);
        if (v > 0) dpi = static_cast<UINT>(v);
        ReleaseDC(nullptr, dc);
    }
    return dpi;
}

std::string primary_monitor_name() {
    // Walk EnumDisplayDevicesW (DISPLAY_DEVICE_PRIMARY_DEVICE) for the
    // adapter, then ask GetMonitorInfoW for its monitor's friendly name.
    DISPLAY_DEVICEW dev{};
    dev.cb = sizeof(dev);
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dev, 0); ++i) {
        if (dev.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) {
            // DeviceString is the adapter description (e.g. "Intel UHD
            // Graphics 630"); good enough as a human-readable label
            // when the per-monitor name isn't available.
            char buf[128] = {};
            int n = WideCharToMultiByte(
                CP_UTF8, 0, dev.DeviceString, -1, buf, sizeof(buf),
                nullptr, nullptr);
            if (n > 0) return std::string(buf);
            return {};
        }
    }
    return {};
}

DisplayInfo snapshot_main_display() {
    DisplayInfo info;
    const int phys_w = GetSystemMetrics(SM_CXSCREEN);
    const int phys_h = GetSystemMetrics(SM_CYSCREEN);
    const UINT dpi = query_system_dpi();
    const float scale = dpi > 0 ? static_cast<float>(dpi) / 96.0f : 1.0f;
    info.physical_width  = phys_w;
    info.physical_height = phys_h;
    info.scale           = scale;
    info.width  = scale > 0.0f ? static_cast<float>(phys_w) / scale
                               : static_cast<float>(phys_w);
    info.height = scale > 0.0f ? static_cast<float>(phys_h) / scale
                               : static_cast<float>(phys_h);
    info.refresh_hz = 0.0f; // EnumDisplaySettings could fill this; not
                            // worth the cost on a poll path.
    info.name = primary_monitor_name();
    return info;
}

// ── Color scheme detection ────────────────────────────────────────────

constexpr wchar_t kPersonalizeKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
constexpr wchar_t kAppsUseLightTheme[] = L"AppsUseLightTheme";

ColorScheme detect_color_scheme() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kPersonalizeKey, 0,
                      KEY_READ, &key) != ERROR_SUCCESS) {
        return ColorScheme::unknown;
    }
    DWORD value = 1;
    DWORD size = sizeof(value);
    DWORD type = 0;
    LONG r = RegQueryValueExW(key, kAppsUseLightTheme, nullptr, &type,
                              reinterpret_cast<LPBYTE>(&value), &size);
    RegCloseKey(key);
    if (r != ERROR_SUCCESS || type != REG_DWORD) return ColorScheme::unknown;
    return value == 0 ? ColorScheme::dark : ColorScheme::light;
}

// ── Memory pressure ───────────────────────────────────────────────────
//
// Win32 has no per-process LMK signal at the desktop session layer,
// so we sample GlobalMemoryStatusEx().dwMemoryLoad on the worker tick.
// Tiers mirror the macOS dispatch source: < 80 normal, < 95 moderate,
// >= 95 critical.

MemoryPressure detect_memory_pressure() {
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (!GlobalMemoryStatusEx(&ms)) return MemoryPressure::normal;
    if (ms.dwMemoryLoad >= 95) return MemoryPressure::critical;
    if (ms.dwMemoryLoad >= 80) return MemoryPressure::moderate;
    return MemoryPressure::normal;
}

// ── Shared observer state ─────────────────────────────────────────────

class WinEnvObserver {
public:
    void start();

    LifecycleState lifecycle() const noexcept {
        return lifecycle_.load(std::memory_order_acquire);
    }
    void set_lifecycle(LifecycleState s) noexcept {
        lifecycle_.store(s, std::memory_order_release);
    }

    MemoryPressure pressure() const noexcept {
        return pressure_.load(std::memory_order_acquire);
    }
    void set_pressure(MemoryPressure p) noexcept {
        pressure_.store(p, std::memory_order_release);
    }

    void publish_snapshot() {
        EnvironmentState s;
        s.display         = snapshot_main_display();
        s.color_scheme    = detect_color_scheme();
        s.lifecycle       = lifecycle();
        s.memory_pressure = pressure();
        Environment::instance().publish(s);
    }

    // Owned by the observer so we can signal the worker to wake up
    // without waiting for the registry change. start() creates them;
    // they live for the process lifetime.
    HANDLE wake_event() const noexcept { return wake_event_; }

private:
    static LRESULT CALLBACK window_proc(HWND, UINT, WPARAM, LPARAM);
    void create_message_window();
    void run_worker();

    std::atomic<LifecycleState> lifecycle_{LifecycleState::foreground};
    std::atomic<MemoryPressure> pressure_{MemoryPressure::normal};
    std::atomic<bool>           running_{false};
    std::once_flag              start_once_;
    std::thread                 worker_;
    HANDLE                      wake_event_ = nullptr;
};

WinEnvObserver g_observer;

LRESULT CALLBACK WinEnvObserver::window_proc(HWND hwnd, UINT msg,
                                             WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_ACTIVATEAPP: {
        // wParam: TRUE if the app is being activated, FALSE otherwise.
        g_observer.set_lifecycle(wparam ? LifecycleState::foreground
                                        : LifecycleState::inactive);
        g_observer.publish_snapshot();
        return 0;
    }
    case WM_DPICHANGED:
    case WM_DISPLAYCHANGE: {
        g_observer.publish_snapshot();
        return 0;
    }
    case WM_SETTINGCHANGE: {
        // wParam == 0 with lParam pointing at "ImmersiveColorSet" is
        // the documented signal for personalization changes (light/
        // dark mode). Anything else is harmless to refresh on.
        g_observer.publish_snapshot();
        // Also wake the worker so a manual registry edit (no broadcast)
        // is picked up promptly without waiting for the 5 s tick.
        if (HANDLE w = g_observer.wake_event()) SetEvent(w);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void WinEnvObserver::create_message_window() {
    static const wchar_t kClassName[] = L"PulpEnvironmentObserverWindow";

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = &WinEnvObserver::window_proc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClassName;
    // RegisterClassExW returns 0 on duplicate-class; that's fine,
    // CreateWindowExW will succeed against the existing registration.
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        0, kClassName, L"PulpEnvironmentObserver",
        0, 0, 0, 0, 0,
        HWND_MESSAGE,             // message-only parent
        nullptr,
        wc.hInstance,
        nullptr);
    if (!hwnd) return;
    // We never destroy the window — it lives for the process lifetime,
    // matching the macOS adapter's dispatch_once observer.
}

void WinEnvObserver::run_worker() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kPersonalizeKey, 0,
                      KEY_READ | KEY_NOTIFY, &key) != ERROR_SUCCESS) {
        key = nullptr;
    }

    HANDLE reg_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    auto arm_reg = [&]() {
        if (!key || !reg_event) return;
        ResetEvent(reg_event);
        RegNotifyChangeKeyValue(key, FALSE,
            REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET,
            reg_event, TRUE);
    };
    arm_reg();

    constexpr DWORD kPollMs = 5000;

    // Initial snapshot so listeners get state without waiting for an
    // OS event. Mirrors PulpEnvironmentObserver::startObserving on macOS.
    set_pressure(detect_memory_pressure());
    publish_snapshot();

    while (running_.load(std::memory_order_acquire)) {
        HANDLE handles[2];
        DWORD count = 0;
        if (reg_event) handles[count++] = reg_event;
        if (wake_event_) handles[count++] = wake_event_;

        DWORD wait = (count > 0)
            ? WaitForMultipleObjects(count, handles, FALSE, kPollMs)
            : (Sleep(kPollMs), WAIT_TIMEOUT);

        if (!running_.load(std::memory_order_acquire)) break;

        // Re-arm registry watch any time it fired.
        if (reg_event && wait == WAIT_OBJECT_0) {
            arm_reg();
        }

        // Always sample memory pressure on a wake-up, then publish a
        // single coherent snapshot regardless of which signal woke us.
        set_pressure(detect_memory_pressure());
        publish_snapshot();
    }

    if (reg_event) CloseHandle(reg_event);
    if (key) RegCloseKey(key);
}

void WinEnvObserver::start() {
    std::call_once(start_once_, [this]() {
        wake_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        running_.store(true, std::memory_order_release);

        // Worker thread first (it does the initial publish so listeners
        // get a snapshot even if no message ever pumps).
        worker_ = std::thread([this]() { run_worker(); });
        worker_.detach(); // process-lived, mirrors macOS dispatch source

        // Hidden window must be created on a thread that pumps messages.
        // Plugin hosts already pump on the main thread, so creating the
        // window from the caller's thread (typically main) is correct.
        // If the caller isn't the message-pump thread, WM_ACTIVATEAPP
        // simply won't dispatch — the worker keeps publishing display +
        // color + memory snapshots regardless.
        create_message_window();
    });
}

} // namespace

void start_environment_observer_win() {
    g_observer.start();
}

} // namespace pulp::platform

#endif // _WIN32
