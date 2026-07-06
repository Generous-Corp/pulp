#pragma once

#include <pulp/state/store.hpp>
#include <pulp/view/hot_reload.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widget_bridge.hpp>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace pulp::render {
class GpuSurface;
}

namespace pulp::view {

struct ScriptedUiOptions {
    std::filesystem::path script_path;
    std::filesystem::path theme_path;
    std::vector<std::filesystem::path> asset_roots;
    bool enable_hot_reload = false;
    bool enable_theme_reload = true;
};

// Manages a JS-driven widget tree, optional theme.json overrides, and
// standalone hot reload semantics with widget value preservation.
class ScriptedUiSession {
public:
    ScriptedUiSession(View& root, state::StateStore& store, ScriptedUiOptions options);
    ~ScriptedUiSession();

    ScriptedUiSession(const ScriptedUiSession&) = delete;
    ScriptedUiSession& operator=(const ScriptedUiSession&) = delete;

    bool load(std::string* error = nullptr);
    bool poll(std::string* error = nullptr);

    // Explicitly reload the current script in place: rebuilds the widget bridge
    // under the SAME root + GPU surface, preserving widget state, and probes the
    // new code first so a bad reload keeps the last-good UI. The on-demand
    // counterpart to enable_hot_reload's file-watched poll() — for a host/editor
    // that wants to reload a just-edited bundle without a file watcher.
    bool reload(std::string* error = nullptr);
    // Repoint at a different script file and reload it (e.g. swap to another
    // design bundle's ui.js). Updates script_path()/theme_path(); same in-place,
    // last-good semantics as reload(). Does not re-arm the hot-reload watcher.
    bool reload_from(std::filesystem::path script_path, std::string* error = nullptr);

    void set_repaint_callback(std::function<void()> cb);
    WidgetBridge* bridge() const { return bridge_.get(); }

    /// JS-axis reload timings, ms. Populated on every
    /// rebuild_from_code() — full on success, partial (later phases 0) on an
    /// early failure. The DSP-axis counterpart lives in reload_transaction.hpp's
    /// ReloadMetrics; together they feed the `reloaded in NNN ms` diagnostic and
    /// p50/p95 baselines.
    struct ReloadMetrics {
        double probe_ms = 0.0;     ///< parse + build a probe bridge to validate the code
        double snapshot_ms = 0.0;  ///< snapshot the live widget values before the rebuild
        double rebuild_ms = 0.0;   ///< build the live bridge from the new code + apply theme
        double restore_ms = 0.0;   ///< restore preserved widget values into the new tree
        double total_ms = 0.0;     ///< end-to-end
    };
    const ReloadMetrics& last_reload_metrics() const { return last_reload_metrics_; }
    /// Convenience: total wall-clock of the last reload, ms.
    double last_reload_ms() const { return last_reload_metrics_.total_ms; }

    // Attach the host's GpuSurface so the JS-side navigator.gpu /
    // canvas.getContext('webgpu') bridge routes through Pulp's live Dawn
    // instance. The format adapters open this session
    // BEFORE the PluginViewHost exists, so the surface arrives via this
    // setter once the host is built (e.g. inside `au_view_controller_ios.mm`
    // after `PluginViewHost::create`).
    //
    // Stored so that a hot-reload-triggered bridge rebuild reattaches the
    // same surface to the new bridge. Pass `nullptr` to detach (called from
    // the host's teardown before the bridge is destroyed).
    void attach_gpu_surface(render::GpuSurface* gpu_surface);
    render::GpuSurface* gpu_surface() const noexcept { return gpu_surface_; }

    const std::filesystem::path& script_path() const { return script_path_; }
    const std::filesystem::path& theme_path() const { return theme_path_; }
    bool hot_reload_enabled() const { return hot_reload_enabled_; }
    bool theme_reload_enabled() const { return theme_reload_enabled_; }

private:
    View& root_;
    state::StateStore& store_;
    std::filesystem::path script_path_;
    std::filesystem::path theme_path_;
    std::vector<std::filesystem::path> asset_roots_;
    bool hot_reload_enabled_ = false;
    bool theme_reload_enabled_ = false;

    std::unique_ptr<ScriptEngine> engine_;
    std::unique_ptr<WidgetBridge> bridge_;
    std::unique_ptr<HotReloader> reloader_;
    std::function<void()> repaint_callback_;
    render::GpuSurface* gpu_surface_ = nullptr;

    Theme base_theme_;
    ReloadMetrics last_reload_metrics_{};   // JS-axis reload timings (item 1.2)
    bool last_theme_exists_ = false;
    std::optional<std::filesystem::file_time_type> last_theme_write_time_;

    bool rebuild_from_code(const std::string& code, bool preserve_state, std::string* error);
    bool apply_theme_override(std::string* error);
    // Read + parse the sibling theme override onto `base` WITHOUT mutating any
    // live state — the FALLIBLE half of a theme apply, split out so a reload can
    // validate the theme BEFORE the irreversible bridge commit (rollback-safety,
    // item 1.5). Fills out_* and returns false (with `error`) on a bad theme file.
    bool resolve_theme_override(const Theme& base, Theme& out_merged, bool& out_exists,
                                std::optional<std::filesystem::file_time_type>& out_write_time,
                                std::string* error) const;
    bool poll_theme_reload(std::string* error);

    static std::string read_text_file(const std::filesystem::path& path);
    static std::string describe_exception();
};

} // namespace pulp::view
