#include <pulp/view/scripted_ui.hpp>
#include <pulp/runtime/log.hpp>
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
    , theme_path_(script_path_.parent_path() / "theme.json")
    , hot_reload_enabled_(options.enable_hot_reload)
    , theme_reload_enabled_(options.enable_theme_reload)
{
}

ScriptedUiSession::~ScriptedUiSession() = default;

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
        probe_bridge->load_script(code);

        WidgetReloadSnapshot saved_values;
        if (preserve_state && bridge_) {
            bridge_->snapshot_values(saved_values);
            bridge_->clear();
        }

        root_.set_theme(theme_for_reload);
        auto next_engine = make_engine();
        auto next_bridge = std::make_unique<WidgetBridge>(*next_engine, root_, store_);
        if (repaint_callback_) {
            next_bridge->set_repaint_callback(repaint_callback_);
        }
        next_bridge->load_script(code);
        base_theme_ = root_.theme();

        engine_ = std::move(next_engine);
        bridge_ = std::move(next_bridge);
        if (!apply_theme_override(error)) {
            return false;
        }
        if (preserve_state) {
            bridge_->restore_values(saved_values);
        }
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

    root_.set_theme(base_theme_);

    if (!std::filesystem::exists(theme_path_)) {
        last_theme_exists_ = false;
        last_theme_write_time_.reset();
        return true;
    }

    auto json = read_text_file(theme_path_);
    if (json.empty()) {
        if (error) *error = "could not read theme file: " + theme_path_.string();
        return false;
    }

    try {
        auto merged = base_theme_;
        merged.apply_overrides(Theme::from_json(json));
        root_.set_theme(merged);
        last_theme_exists_ = true;
        last_theme_write_time_ = safe_last_write_time(theme_path_);
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    } catch (...) {
        if (error) *error = describe_exception();
        return false;
    }
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
