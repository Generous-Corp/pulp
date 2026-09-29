#include <pulp/view/gpu_acquire_diagnostics.hpp>

#include <cctype>
#include <cmath>
#include <string_view>

namespace pulp::view {

GpuAcquireDiagnostics gpu_acquire_diagnostics(double now_s,
                                              double vsync_target_s,
                                              double refresh_period_s,
                                              int frames_in_flight) noexcept {
    GpuAcquireDiagnostics d;
    d.frames_in_flight = frames_in_flight;
    if (std::isfinite(refresh_period_s) && refresh_period_s > 0.0)
        d.refresh_period_ms = refresh_period_s * 1000.0;
    if (std::isfinite(vsync_target_s) && vsync_target_s > 0.0 && std::isfinite(now_s)) {
        d.vsync_driven = true;
        d.late_ms = (now_s - vsync_target_s) * 1000.0;
    }
    return d;
}

bool gpu_timing_requested(const char* env_value) noexcept {
    if (!env_value) return false;
    const std::string_view v(env_value);
    auto equals = [&](std::string_view word) {
        if (v.size() != word.size()) return false;
        for (size_t i = 0; i < v.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(v[i])) != word[i]) return false;
        return true;
    };
    return equals("1") || equals("true") || equals("yes") || equals("on");
}

} // namespace pulp::view
