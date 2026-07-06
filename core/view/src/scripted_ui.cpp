#include <pulp/view/scripted_ui.hpp>
#include <pulp/runtime/log.hpp>
#include <chrono>
#include <fstream>
#include <sstream>

namespace pulp::view {

namespace {

std::unique_ptr<ScriptEngine> make_engine() {
    auto engine = std::make_unique<ScriptEngine>();
    engine->set_log_callback([](std::string_view level, std::string_view msg) {
        runtime::log_info("script-ui[{}] {}", std::string(level), std::string(msg));
    });
    return engine;
}

std::optional<std::filesystem::file_time_type> safe_last_write_time(const std::filesystem::path& path) {
    std::error_code ec;
    auto time = std::filesystem::last_write_time(path, ec);
    if (ec) return std::nullopt;
    return time;
}

} // namespace

ScriptedUiSession::ScriptedUiSession(View& root, state::StateStore& store, ScriptedUiOptions options)
    : root_(root)
    , store_(store)
    , script_path_(std::move(options.script_path))
    , theme_path_(options.theme_path.empty() ? script_path_.parent_path() / "theme.json"
                                             : std::move(options.theme_path))
    , asset_roots_(std::move(options.asset_roots))
    , hot_reload_enabled_(options.enable_hot_reload)
    , theme_reload_enabled_(options.enable_theme_reload)
{
}

ScriptedUiSession::~ScriptedUiSession() = default;

// Late-attach of the host's GpuSurface. Hosts (e.g. au_view_controller_ios.mm)
// call this AFTER PluginViewHost::create returns, so the JS-side navigator.gpu
// / canvas.getContext('webgpu')
// shim routes through Pulp's live Dawn instance instead of a mock.
void ScriptedUiSession::attach_gpu_surface(render::GpuSurface* gpu_surface) {
    gpu_surface_ = gpu_surface;
    if (bridge_) {
        bridge_->attach_gpu_surface(gpu_surface);
    }
    // Stashed in gpu_surface_ so that the next hot-reload rebuild_from_code
    // passes the same surface into the freshly-constructed WidgetBridge.
}

bool ScriptedUiSession::load(std::string* error) {
    auto code = read_text_file(script_path_);
    if (code.empty()) {
        if (error) *error = "could not read script file: " + script_path_.string();
        return false;
    }

    if (!rebuild_from_code(code, false, error)) {
        return false;
    }

    if (hot_reload_enabled_) {
        reloader_ = std::make_unique<HotReloader>(script_path_, [this](const std::string& next_code) {
            std::string reload_error;
            if (!rebuild_from_code(next_code, true, &reload_error)) {
                runtime::log_error("Scripted UI hot reload failed for '{}': {}",
                                   script_path_.string(), reload_error);
                return;
            }
            runtime::log_info("Scripted UI hot reload applied from '{}'", script_path_.string());
        });
    }

    last_theme_exists_ = std::filesystem::exists(theme_path_);
    last_theme_write_time_ = last_theme_exists_ ? safe_last_write_time(theme_path_) : std::nullopt;
    return true;
}

bool ScriptedUiSession::reload(std::string* error) {
    auto code = read_text_file(script_path_);
    if (code.empty()) {
        if (error) *error = "could not read script file: " + script_path_.string();
        return false;
    }
    // preserve_state=true: keep widget values across the rebuild; rebuild_from_code
    // probes the new code on a throwaway tree first, so a bad reload leaves the
    // current UI intact.
    return rebuild_from_code(code, /*preserve_state=*/true, error);
}

bool ScriptedUiSession::reload_from(std::filesystem::path script_path, std::string* error) {
    script_path_ = std::move(script_path);
    theme_path_ = script_path_.parent_path() / "theme.json";
    last_theme_exists_ = std::filesystem::exists(theme_path_);
    last_theme_write_time_ = last_theme_exists_ ? safe_last_write_time(theme_path_) : std::nullopt;
    return reload(error);
}

bool ScriptedUiSession::poll(std::string* error) {
    bool changed = false;
    if (bridge_) {
        // pulp #1412 — host idle pump must drain BOTH async-shell results
        // (poll_async_results) AND timers + rAF callbacks
        // (service_frame_callbacks). Without the second call, JS
        // setTimeout / setInterval callbacks queue forever on Mac/iOS
        // because nothing else drives the bridge's message loop on the
        // host idle cadence (CVDisplayLink / CADisplayLink).
        // poll_async_results: drains async-exec results + flushes frames.
        // service_frame_callbacks: pumps engine message loop + drains
        //   native-tracked timers + flushes frames.
        // Together they form the full per-vsync bridge pump.
        bridge_->poll_async_results();
        bridge_->service_frame_callbacks();
    }
    if (reloader_ && reloader_->poll_reload()) {
        changed = true;
    }
    if (poll_theme_reload(error)) {
        changed = true;
    }
    return changed;
}

void ScriptedUiSession::set_repaint_callback(std::function<void()> cb) {
    repaint_callback_ = std::move(cb);
    if (bridge_) {
        bridge_->set_repaint_callback(repaint_callback_);
    }
}

bool ScriptedUiSession::rebuild_from_code(const std::string& code, bool preserve_state, std::string* error) {
    // JS-axis reload timings (item 1.2). steady_clock; UI/control thread only.
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    const auto ms = [](clock::time_point a, clock::time_point b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };
    last_reload_metrics_ = ReloadMetrics{};   // reset; stays partial on early failure
    try {
        const auto theme_for_reload = preserve_state ? base_theme_ : root_.theme();
        auto probe_engine = make_engine();
        View probe_root;
        probe_root.set_theme(theme_for_reload);
        probe_root.flex().direction = FlexDirection::column;
        state::StateStore probe_store;
        for (const auto& group : store_.all_groups()) {
            probe_store.add_group(group);
        }
        for (const auto& param : store_.all_params()) {
            probe_store.add_parameter(param);
            probe_store.set_value(param.id, store_.get_value(param.id));
        }
        auto probe_bridge = std::make_unique<WidgetBridge>(*probe_engine, probe_root, probe_store);
        probe_bridge->set_asset_roots(asset_roots_);
        probe_bridge->load_script(code);
        const auto t_probe = clock::now();

        // Pre-resolve the theme override HERE — the last FALLIBLE step — BEFORE
        // we snapshot/clear/commit, so a bad theme file fails the reload with the
        // live UI fully intact instead of AFTER the bridge is already swapped
        // (rollback-safety, item 1.5). The apply past the commit is infallible.
        Theme resolved_theme = theme_for_reload;
        bool next_theme_exists = false;
        std::optional<std::filesystem::file_time_type> next_theme_write_time;
        if (theme_reload_enabled_ &&
            !resolve_theme_override(theme_for_reload, resolved_theme, next_theme_exists,
                                    next_theme_write_time, error)) {
            last_reload_metrics_.probe_ms = ms(t0, t_probe);
            last_reload_metrics_.total_ms = ms(t0, clock::now());
            return false;  // nothing snapshot/cleared/committed yet — old UI intact
        }

        WidgetReloadSnapshot saved_values;
        if (preserve_state && bridge_) {
            bridge_->snapshot_values(saved_values);
            bridge_->clear();
        }
        const auto t_snapshot = clock::now();

        root_.set_theme(theme_for_reload);
        auto next_engine = make_engine();
        auto next_bridge = std::make_unique<WidgetBridge>(*next_engine, root_, store_,
                                                          gpu_surface_);
        next_bridge->set_asset_roots(asset_roots_);
        if (repaint_callback_) {
            next_bridge->set_repaint_callback(repaint_callback_);
        }
        next_bridge->load_script(code);
        base_theme_ = root_.theme();

        engine_ = std::move(next_engine);
        bridge_ = std::move(next_bridge);
        // Infallible apply of the pre-resolved theme — no failure point past the
        // commit (item 1.5).
        if (theme_reload_enabled_) {
            root_.set_theme(resolved_theme);
            last_theme_exists_ = next_theme_exists;
            last_theme_write_time_ = next_theme_write_time;
        }
        const auto t_rebuild = clock::now();
        if (preserve_state) {
            bridge_->restore_values(saved_values);
        }
        const auto t_restore = clock::now();

        last_reload_metrics_.probe_ms = ms(t0, t_probe);
        last_reload_metrics_.snapshot_ms = ms(t_probe, t_snapshot);
        last_reload_metrics_.rebuild_ms = ms(t_snapshot, t_rebuild);
        last_reload_metrics_.restore_ms = ms(t_rebuild, t_restore);
        last_reload_metrics_.total_ms = ms(t0, t_restore);
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        last_reload_metrics_.total_ms = ms(t0, clock::now());
        return false;
    } catch (...) {
        if (error) *error = describe_exception();
        last_reload_metrics_.total_ms = ms(t0, clock::now());
        return false;
    }
}

bool ScriptedUiSession::resolve_theme_override(
    const Theme& base, Theme& out_merged, bool& out_exists,
    std::optional<std::filesystem::file_time_type>& out_write_time,
    std::string* error) const {
    if (!std::filesystem::exists(theme_path_)) {
        out_merged = base;          // no override file → the base theme as-is
        out_exists = false;
        out_write_time.reset();
        return true;
    }
    auto json = read_text_file(theme_path_);
    if (json.empty()) {
        if (error) *error = "could not read theme file: " + theme_path_.string();
        return false;
    }
    try {
        Theme merged = base;
        merged.apply_overrides(Theme::from_json(json));   // FALLIBLE: JSON parse
        out_merged = std::move(merged);
        out_exists = true;
        out_write_time = safe_last_write_time(theme_path_);
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    } catch (...) {
        if (error) *error = describe_exception();
        return false;
    }
}

bool ScriptedUiSession::apply_theme_override(std::string* error) {
    if (!theme_reload_enabled_) {
        return true;
    }
    Theme merged;
    bool exists = false;
    std::optional<std::filesystem::file_time_type> write_time;
    if (!resolve_theme_override(base_theme_, merged, exists, write_time, error)) {
        return false;
    }
    root_.set_theme(merged);           // infallible apply
    last_theme_exists_ = exists;
    last_theme_write_time_ = write_time;
    return true;
}

bool ScriptedUiSession::poll_theme_reload(std::string* error) {
    if (!theme_reload_enabled_) {
        return false;
    }

    const bool exists = std::filesystem::exists(theme_path_);
    auto write_time = exists ? safe_last_write_time(theme_path_) : std::nullopt;
    const bool changed = (exists != last_theme_exists_) || (write_time != last_theme_write_time_);
    if (!changed) {
        return false;
    }

    std::string theme_error;
    if (!apply_theme_override(&theme_error)) {
        if (error) *error = theme_error;
        runtime::log_error("Scripted UI theme reload failed for '{}': {}",
                           theme_path_.string(), theme_error);
        return false;
    }

    runtime::log_info("Scripted UI theme override reloaded from '{}'", theme_path_.string());
    return true;
}

std::string ScriptedUiSession::read_text_file(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return {};
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

std::string ScriptedUiSession::describe_exception() {
    return "unknown exception";
}

} // namespace pulp::view
