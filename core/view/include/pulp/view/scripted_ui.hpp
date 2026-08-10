#pragma once

#include <pulp/state/store.hpp>
#include <pulp/view/hot_reload.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/script_inspector_bridge.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widget_bridge.hpp>
#include <cstdint>
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

class ValueChannelSet;

struct ScriptedUiOptions {
    std::filesystem::path script_path;
    std::filesystem::path theme_path;
    std::vector<std::filesystem::path> asset_roots;
    bool enable_hot_reload = false;
    bool enable_theme_reload = true;
    /// Compatibility adapter for stable, non-reloadable processors. Converted
    /// to ValueChannelAccess during construction and never retained directly.
    ValueChannelSet* value_channels = nullptr;
    /// Leased access to the hosting processor's named value channels. The
    /// visitor must not retain a set or source after its callback returns.
    ValueChannelAccess value_channel_access;
    /// Native bridge APIs granted to this realm. Trusted/local scripted UIs
    /// retain the historical all-capabilities default; protected hosts pass an
    /// explicit empty or reviewed set. Appended to preserve the public
    /// aggregate's legacy positional layout.
    CapabilitySet granted_capabilities = CapabilitySet::all();
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
    /// Advance one frame: discharge the realm reset a completed
    /// `Runtime.evaluate` owes, retire and destroy realms replaced since the
    /// last frame, pump the bridge and one queued inspector request, then poll
    /// the script and theme files. Returns whether anything changed; returns
    /// false with `error` set when the owed realm reset (or the accessibility
    /// retirement it defers) could not complete.
    ///
    /// A host MUST call this from its frame loop even when it drives the UI
    /// itself: it is where an evaluated realm is discarded, so skipping it
    /// leaves evaluated timers and callbacks live in the realm they were
    /// created in.
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
    /// Install the host-owned rebind run after a successful deferred
    /// Runtime.evaluate realm reset. The callback runs from poll(), after the
    /// replacement bridge is live and before that bridge pumps a frame. Hosts
    /// that attach native observers to script-owned Views must resolve those
    /// Views again here; the reset deliberately discards every callback that
    /// belonged to the evaluated realm.
    ///
    /// The callback must not retain pre-reset View pointers or perform another
    /// inspector evaluation (which would immediately owe another reset).
    void set_post_evaluation_reset_callback(std::function<void()> cb);
    /// Replace the live JS console sink and retain it across hot reloads.
    /// This is the primary application-owned sink; secondary scoped observers
    /// installed with add_log_callback() are preserved.
    void set_log_callback(LogCallback callback);
    /// Add a secondary console observer without replacing the primary sink.
    /// The returned nonzero token remains valid until explicitly removed or
    /// this session is destroyed, and survives engine hot reloads.
    std::uint64_t add_log_callback(LogCallback callback);
    void remove_log_callback(std::uint64_t token);
    WidgetBridge* bridge() const {
        return runtime_realm_quarantined_ ? nullptr : bridge_.get();
    }
    /// Actual effectful API grants installed in the live bridge, or the grants
    /// that will be installed before the first successful load. Returned by
    /// value so inspector policy cannot mutate the realm.
    CapabilitySet granted_capabilities() const noexcept {
        return bridge_ ? bridge_->granted_capabilities() : granted_capabilities_;
    }

    /// The runtime-inspector bridge for this session's JS engine. Always
    /// present (even before load()); it tracks the live engine across hot
    /// reloads and is pumped once per poll(). A host passes it to the separately
    /// linked make_script_runtime_evaluator() adapter, retains that adapter, and
    /// lends the adapter to DomainHandler::set_runtime_evaluator() so the
    /// Runtime.* methods reach the live UI.
    ///
    /// TEARDOWN CONTRACT: the bridge is owned by (and lives as long as) this
    /// session, but its off-thread methods are called from an InspectorServer
    /// reader thread. A host that wires it MUST, before destroying this session,
    /// stop that reader thread, clear DomainHandler's evaluator pointer, and
    /// destroy the retained evaluator before this session, so no background
    /// call dereferences the bridge post-destruction.
    ScriptInspectorBridge* script_inspector() { return &inspector_bridge_; }
    /// Process-unique identity for borrowed-source lifetime/ABA checks.
    std::uint64_t identity() const noexcept { return identity_; }

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
    bool hot_reload_pending() const {
        return reloader_ && reloader_->has_pending_reload();
    }
    bool theme_reload_enabled() const { return theme_reload_enabled_; }

private:
    const std::uint64_t identity_;
    View& root_;
    state::StateStore& store_;
    std::filesystem::path script_path_;
    std::filesystem::path theme_path_;
    std::string last_good_code_;
    std::filesystem::path last_good_script_path_;
    std::vector<std::filesystem::path> asset_roots_;
    CapabilitySet granted_capabilities_ = CapabilitySet::all();
    bool hot_reload_enabled_ = false;
    bool theme_reload_enabled_ = false;
    ValueChannelAccess value_channel_access_;

    std::unique_ptr<ScriptEngine> engine_;
    std::unique_ptr<WidgetBridge> bridge_;
    struct RetiredRuntimeRealm {
        std::unique_ptr<ScriptEngine> engine;
        std::unique_ptr<WidgetBridge> bridge;
    };
    struct RuntimeRealmTeardownOwner {
        std::vector<RetiredRuntimeRealm> retired;
        std::unique_ptr<ScriptEngine> current_engine;
        std::unique_ptr<WidgetBridge> current_bridge;
    };
    // Owner-thread evaluate may run inline more than once between UI polls, so
    // retain every old realm until the next poll rather than overwriting one
    // slot inside a later response fence.
    std::vector<RetiredRuntimeRealm> retired_runtime_realms_;
    // Allocated with the session rather than during its noexcept destructor.
    // A reentrant native accessibility callback may retain this owner after the
    // ScriptedUiSession itself has gone away.
    std::shared_ptr<RuntimeRealmTeardownOwner> runtime_realm_teardown_owner_ =
        std::make_shared<RuntimeRealmTeardownOwner>();
    // Marshals off-thread inspector evaluate/interrupt requests onto the engine
    // thread. Re-attached to the live engine after every rebuild_from_code().
    ScriptInspectorBridge inspector_bridge_;
    std::unique_ptr<HotReloader> reloader_;
    std::function<void()> repaint_callback_;
    std::function<void()> post_evaluation_reset_callback_;
    bool post_evaluation_reset_callback_pending_ = false;
    LogCallback log_callback_;
    std::unordered_map<std::uint64_t, LogCallback> log_subscribers_;
    std::uint64_t next_log_subscription_ = 0;
    render::GpuSurface* gpu_surface_ = nullptr;

    Theme base_theme_;
    Theme last_good_effective_theme_;
    bool runtime_realm_quarantined_ = false;
    bool accessibility_retirement_pending_ = false;
    ReloadMetrics last_reload_metrics_{};   // JS-axis reload timings (item 1.2)
    bool last_theme_exists_ = false;
    std::optional<std::filesystem::file_time_type> last_theme_write_time_;

    bool rebuild_from_code(
        const std::string& code, const std::filesystem::path& source_path,
        bool preserve_state, std::string* error,
        std::optional<ScriptInspectorBridge::EvaluationDeadline> deadline = std::nullopt);
    bool apply_theme_override(std::string* error);
    // Read + parse the sibling theme override onto `base` WITHOUT mutating any
    // live state — the FALLIBLE half of a theme apply, split out so a reload can
    // validate the theme BEFORE the irreversible bridge commit (rollback-safety,
    // item 1.5). Fills out_* and returns false (with `error`) on a bad theme file.
    bool resolve_theme_override(const Theme& base, Theme& out_merged, bool& out_exists,
                                std::optional<std::filesystem::file_time_type>& out_write_time,
                                std::string* error) const;
    bool poll_theme_reload(std::string* error);
    std::string reset_after_runtime_evaluation(
        ScriptInspectorBridge::EvaluationDeadline deadline);
    LogCallback engine_log_callback();
    void dispatch_log(std::string_view level, std::string_view message);

    static std::string read_text_file(const std::filesystem::path& path);
    static std::string describe_exception();
};

} // namespace pulp::view
