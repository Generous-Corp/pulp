#pragma once

#include <pulp/format/view_bridge.hpp>
#include <pulp/runtime/log.hpp>
#include <pulp/view/plugin_view_host.hpp>
#include <pulp/view/scripted_ui.hpp>

#include <cstdlib>
#include <cstring>
#include <functional>

// Shared GPU PluginViewHost auto-selection for every plugin format adapter
// (AU v2 / VST3 / CLAP / iOS AUv3). The renderer, JS runtime, Yoga layout,
// and surface management are identical across formats — only the per-format
// shell/lifecycle glue differs — so the *decision* of whether to request the
// GPU host belongs in one place.
//
// A developer building a Skia/Dawn/scripted editor gets the GPU host
// automatically; they never hand-set a flag. Hardcoding `use_gpu=false` is
// exactly the trap that made the ChainerSynth AU fall back to AutoUi/CPU.
//
// See `planning/2026-05-22-gpu-view-host-in-plugins.md`.

namespace pulp::format {

/// Runtime opt-out: `PULP_DISABLE_PLUGIN_GPU=1` forces the CoreGraphics CPU
/// path. DAW/plugin host processes vary; a GPU failure must never make an
/// editor vanish, so the operator can always force CPU.
inline bool plugin_gpu_disabled_by_env() {
    const char* v = std::getenv("PULP_DISABLE_PLUGIN_GPU");
    return v != nullptr && v[0] != '\0' && std::strcmp(v, "0") != 0;
}

struct GpuHostDecision {
    bool use_gpu = false;          ///< final request passed to PluginViewHost::Options
    const char* mode = "autoui";   ///< "scripted" | "custom" | "autoui"
    bool wants_gpu = false;        ///< capability signal, before env opt-out
    bool create_view_null = false; ///< bridge has no view (open failed)
};

/// Decide whether the GPU host should be requested for `bridge`, and emit the
/// `[plugin-gpu-host]` debug-parity adapter log (mirrors `[gpu-host]`).
///
/// GPU is wanted when the editor renders through the Skia/Dawn pipeline:
///   - the framework scripted-UI path (`PULP_UI_SCRIPT_PATH`), detected via
///     `ViewBridge::uses_script_ui()`, or
///   - a custom `Processor::create_view()` whose view set
///     `View::set_requires_gpu_host(true)` (e.g. ChainerSynth).
inline GpuHostDecision decide_gpu_host(const ViewBridge& bridge) {
    GpuHostDecision d;
    const view::View* v = bridge.view();
    d.create_view_null = (v == nullptr);

    const bool scripted = bridge.uses_script_ui();
    const bool view_wants = (v != nullptr) && v->requires_gpu_host();
    d.wants_gpu = scripted || view_wants;

    if (scripted) {
        d.mode = "scripted";
    } else if (view_wants) {
        d.mode = "custom";
    } else {
        d.mode = "autoui";
    }

    const bool env_off = plugin_gpu_disabled_by_env();
    d.use_gpu = d.wants_gpu && !env_off;

    runtime::log_info(
        "[plugin-gpu-host] adapter mode={} use_gpu={} wants_gpu={} "
        "env_disabled={} scripted={} requires_gpu_host={} create_view_null={}",
        d.mode, d.use_gpu, d.wants_gpu, env_off, scripted, view_wants,
        d.create_view_null);

    return d;
}

/// Build the per-vsync idle pump for a bridge. GPU hosts invoke it once per
/// display-link tick so the scripted UI session keeps polling async results,
/// timers, and `requestAnimationFrame` while the editor is embedded. Captures
/// the bridge by pointer; the host MUST drop this callback (via `detach()` /
/// destruction) before the bridge is destroyed — every adapter resets its
/// host before `bridge.close()`.
inline std::function<void()> make_scripted_idle_pump(ViewBridge& bridge) {
    auto* bridge_ptr = &bridge;
    return [bridge_ptr]() {
        if (auto* session = bridge_ptr->scripted_ui()) {
            session->poll();
        }
    };
}

/// Scream-guard (runtime): after the host is created, verify the GPU path
/// actually took. If GPU was expected (requested + Skia compiled in) but the
/// host fell back to CoreGraphics, log LOUDLY — the same "expected-but-not-used
/// → scream" philosophy as the build-time Skia-not-linked guard (c6da9c4da).
/// On a no-Skia build the request is moot, so no scream fires.
inline void warn_if_unexpected_cpu_fallback(const GpuHostDecision& d,
                                            const view::PluginViewHost* host) {
#ifdef PULP_HAS_SKIA
    if (d.use_gpu && host != nullptr && !host->is_gpu_backed()) {
        runtime::log_error(
            "[plugin-gpu-host] gpu-init-failed falling_back=cpu mode={} "
            "use_gpu=true host=cpu — a GPU/scripted editor view silently "
            "fell back to CoreGraphics. Check Dawn/Metal availability in the "
            "host process (or set PULP_DISABLE_PLUGIN_GPU=1 intentionally).",
            d.mode);
    }
#else
    (void) d;
    (void) host;
#endif
}

} // namespace pulp::format
