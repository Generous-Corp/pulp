// Linux adapter for the unified Environment API (#342).
//
// Linux desktop has no single OS-level "environment changed" channel
// the way macOS (NSApp), Windows (WM_*) and the mobile platforms do.
// We poll the cheap signals on a low-frequency timer thread:
//
//   color_scheme  — XSettings "Net/ThemeName" / GNOME "color-scheme"
//                   gsetting (org.gnome.desktop.interface), heuristic
//                   fallback to GTK_THEME env or "dark" prefix in name
//   display       — X11 RandR (ScreenResources -> first CRTC) or
//                   Wayland wl_output / xdg-output if XDG_SESSION_TYPE
//                   indicates wayland; we read what's cheap (DISPLAY/
//                   primary monitor size + DPI) without pulling in the
//                   GTK or Qt event loop.
//   lifecycle     — out of scope for desktop Linux (no daemon-driven
//                   foreground/background concept; window-manager
//                   focus is per-window, surfaced by view/render)
//   memory        — /proc/meminfo MemAvailable threshold poll
//
// Safe-area / keyboard / orientation are mobile-only and stay at
// EnvironmentState defaults on Linux desktop.
//
// This is a minimal adapter — pure-stdlib + libX11/Xrandr where
// available. It's deliberately heuristic: full XSettings would pull
// in libxsettings-client; full Wayland output info needs the
// wl_registry dance. Both can land in a follow-up if a real desktop
// integration needs more fidelity.

#if defined(__linux__) && !defined(__ANDROID__)

#include <pulp/platform/environment.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

namespace pulp::platform {

namespace {

// ── Color scheme detection ────────────────────────────────────────────────

ColorScheme detect_color_scheme_from_env() {
    // GTK 3+/4 honor GTK_THEME=Adwaita-dark / GTK_THEME=...:dark.
    if (const char* gtk = std::getenv("GTK_THEME")) {
        std::string s(gtk);
        for (auto& c : s) c = static_cast<char>(std::tolower(c));
        if (s.find("dark") != std::string::npos) return ColorScheme::dark;
    }
    // Qt 6 honors QT_STYLE_OVERRIDE; some distros set it.
    if (const char* qt = std::getenv("QT_STYLE_OVERRIDE")) {
        std::string s(qt);
        for (auto& c : s) c = static_cast<char>(std::tolower(c));
        if (s.find("dark") != std::string::npos) return ColorScheme::dark;
    }
    return ColorScheme::unknown;
}

ColorScheme detect_color_scheme_gsettings() {
    // GNOME 42+ exposes color-scheme via gsettings:
    //   gsettings get org.gnome.desktop.interface color-scheme
    //     -> 'prefer-dark' | 'prefer-light' | 'default'
    // We shell out rather than link gio so the adapter stays
    // dependency-free for non-GNOME desktops.
    FILE* p = popen(
        "gsettings get org.gnome.desktop.interface color-scheme 2>/dev/null",
        "r");
    if (!p) return ColorScheme::unknown;
    char buf[64] = {};
    size_t n = fread(buf, 1, sizeof(buf) - 1, p);
    pclose(p);
    if (n == 0) return ColorScheme::unknown;
    std::string out(buf, n);
    if (out.find("dark") != std::string::npos)  return ColorScheme::dark;
    if (out.find("light") != std::string::npos) return ColorScheme::light;
    return ColorScheme::unknown;
}

ColorScheme detect_color_scheme() {
    auto cs = detect_color_scheme_from_env();
    if (cs != ColorScheme::unknown) return cs;
    return detect_color_scheme_gsettings();
}

// ── Display detection ─────────────────────────────────────────────────────
//
// We read DISPLAY/Wayland session type to know which protocol we're on,
// but for a minimal adapter we only need rough display info: primary
// resolution + scale. Most Linux desktops surface scale via the
// GDK_SCALE / QT_SCALE_FACTOR env vars; for fractional scale on
// Wayland the OS hands compositors a per-output factor we'd need
// xdg-output to read. Until we link wayland-client, we honor the env
// vars and fall back to scale=1.

DisplayInfo detect_display() {
    DisplayInfo info;
    info.scale = 1.0f;

    if (const char* gdk = std::getenv("GDK_SCALE")) {
        try {
            float v = std::stof(gdk);
            if (v >= 1.0f && v <= 4.0f) info.scale = v;
        } catch (...) {}
    } else if (const char* qts = std::getenv("QT_SCALE_FACTOR")) {
        try {
            float v = std::stof(qts);
            if (v >= 1.0f && v <= 4.0f) info.scale = v;
        } catch (...) {}
    }

    // xrandr --query | head -2 gives a "Screen 0: ... current 3840 x 2160"
    // line we can parse without linking libXrandr. Cheap enough at
    // adapter-poll cadence.
    FILE* p = popen("xrandr --query 2>/dev/null | head -1", "r");
    if (p) {
        char line[256] = {};
        if (std::fgets(line, sizeof(line), p)) {
            int w = 0, h = 0;
            const char* cur = std::strstr(line, "current ");
            if (cur && std::sscanf(cur, "current %d x %d", &w, &h) == 2) {
                info.physical_width  = w;
                info.physical_height = h;
                info.width  = info.scale > 0.0f
                    ? static_cast<float>(w) / info.scale : static_cast<float>(w);
                info.height = info.scale > 0.0f
                    ? static_cast<float>(h) / info.scale : static_cast<float>(h);
            }
        }
        pclose(p);
    }

    if (const char* dn = std::getenv("DISPLAY"))      info.name = dn;
    else if (const char* wd = std::getenv("WAYLAND_DISPLAY")) info.name = wd;
    return info;
}

// ── Memory pressure ───────────────────────────────────────────────────────
//
// Linux has no general LMK signal at the desktop session layer (only
// cgroup-driven for systemd-resourced apps). Poll /proc/meminfo and
// report 'critical' when MemAvailable < 5% of MemTotal, 'moderate'
// when < 15%. Heuristic but enough for the cache-purge contract.

MemoryPressure detect_memory_pressure() {
    std::ifstream f("/proc/meminfo");
    if (!f) return MemoryPressure::normal;
    long total_kb = 0;
    long avail_kb = 0;
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("MemTotal:", 0) == 0) {
            std::sscanf(line.c_str(), "MemTotal: %ld kB", &total_kb);
        } else if (line.rfind("MemAvailable:", 0) == 0) {
            std::sscanf(line.c_str(), "MemAvailable: %ld kB", &avail_kb);
        }
    }
    if (total_kb <= 0 || avail_kb <= 0) return MemoryPressure::normal;
    const double frac = static_cast<double>(avail_kb)
                      / static_cast<double>(total_kb);
    if (frac < 0.05) return MemoryPressure::critical;
    if (frac < 0.15) return MemoryPressure::moderate;
    return MemoryPressure::normal;
}

// ── Poll thread ───────────────────────────────────────────────────────────

class LinuxEnvObserver {
public:
    ~LinuxEnvObserver() {
        // Signal the poll loop to exit and join, so process teardown
        // (or an unloaded plugin module) doesn't destroy a still-
        // joinable std::thread — which calls std::terminate(). The
        // 5 s sleep means worst-case we block for 5 s at shutdown;
        // acceptable for a singleton observer. See #438 P1 / #444.
        running_.store(false, std::memory_order_release);
        if (poll_thread_.joinable()) {
            poll_thread_.join();
        }
    }

    void start() {
        std::call_once(start_once_, [this]() {
            running_.store(true, std::memory_order_release);
            poll_thread_ = std::thread([this]() { run(); });
        });
    }

private:
    void publish_snapshot() {
        EnvironmentState s;
        s.display      = detect_display();
        s.color_scheme = detect_color_scheme();
        s.memory_pressure = detect_memory_pressure();
        // Lifecycle is per-window on desktop Linux; the view/window
        // host surfaces its own focus events via callbacks already.
        s.lifecycle = LifecycleState::foreground;
        Environment::instance().publish(s);
    }

    void run() {
        // Poll cadence: 5 s. Fast enough that a theme switch from a
        // settings-app click is visible; slow enough that the popen()
        // calls don't show up in profiling. Use 100 ms sub-sleeps so
        // the destructor's running_ flip is noticed within 100 ms
        // rather than blocking shutdown for up to 5 s.
        publish_snapshot();
        constexpr int kPollMs = 5000;
        constexpr int kTickMs = 100;
        while (running_.load(std::memory_order_acquire)) {
            for (int slept = 0;
                 slept < kPollMs && running_.load(std::memory_order_acquire);
                 slept += kTickMs) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(kTickMs));
            }
            if (!running_.load(std::memory_order_acquire)) break;
            publish_snapshot();
        }
    }

    std::once_flag start_once_;
    std::atomic<bool> running_{false};
    std::thread poll_thread_;
};

LinuxEnvObserver g_observer;

} // namespace

void start_environment_observer_linux() {
    g_observer.start();
}

} // namespace pulp::platform

#endif // __linux__ && !__ANDROID__
