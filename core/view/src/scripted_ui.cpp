#include <pulp/view/scripted_ui.hpp>
#include <pulp/view/accessibility_provider.hpp>
#include <pulp/view/value_channel_set.hpp>
#include <pulp/runtime/log.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace pulp::view {

namespace {

std::atomic<std::uint64_t> next_scripted_ui_identity{0};

LogCallback default_log_callback() {
    return [](std::string_view level, std::string_view msg) {
        runtime::log_info("script-ui[{}] {}", std::string(level), std::string(msg));
    };
}

std::unique_ptr<ScriptEngine> make_engine(LogCallback callback = {}) {
    auto engine = std::make_unique<ScriptEngine>();
    engine->set_log_callback(callback ? std::move(callback) : default_log_callback());
    return engine;
}

std::optional<std::filesystem::file_time_type> safe_last_write_time(const std::filesystem::path& path) {
    std::error_code ec;
    auto time = std::filesystem::last_write_time(path, ec);
    if (ec) return std::nullopt;
    return time;
}

void load_script_before_deadline(
    WidgetBridge& bridge, ScriptEngine& engine, const std::string& code,
    std::optional<ScriptInspectorBridge::EvaluationDeadline> deadline) {
    if (!deadline) {
        bridge.load_script(code);
        return;
    }

    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    bool interrupt_issued = false;
    std::thread watchdog([&] {
        std::unique_lock<std::mutex> lock(mutex);
        if (cv.wait_until(lock, *deadline, [&] { return done; }))
            return;
        interrupt_issued = true;
        engine.request_interrupt();
    });

    std::exception_ptr load_error;
    try {
        bridge.load_script(code);
    } catch (...) {
        load_error = std::current_exception();
    }

    bool deadline_exhausted = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        deadline_exhausted = std::chrono::steady_clock::now() >= *deadline;
        done = true;
    }
    cv.notify_all();
    watchdog.join();
    if (interrupt_issued)
        engine.clear_pending_interrupt();
    if (deadline_exhausted)
        throw std::runtime_error("Runtime.evaluate realm reset timed out");
    if (load_error)
        std::rethrow_exception(load_error);
}

} // namespace

ScriptedUiSession::ScriptedUiSession(View& root, state::StateStore& store, ScriptedUiOptions options)
    : identity_(next_scripted_ui_identity.fetch_add(1, std::memory_order_relaxed) + 1)
    , root_(root)
    , store_(store)
    , script_path_(std::move(options.script_path))
    , theme_path_(options.theme_path.empty() ? script_path_.parent_path() / "theme.json"
                                             : std::move(options.theme_path))
    , asset_roots_(std::move(options.asset_roots))
    , granted_capabilities_(options.granted_capabilities)
    , hot_reload_enabled_(options.enable_hot_reload)
    , theme_reload_enabled_(options.enable_theme_reload)
    , runtime_import_enabled_(options.enable_runtime_import)
    , value_channel_access_(
          options.value_channel_access
              ? std::move(options.value_channel_access)
              : ValueChannelAccess{
                    [channels = options.value_channels](
                        const ValueChannelVisitor& visitor) { visitor(channels); }})
    , log_callback_(default_log_callback())
{
    inspector_bridge_.set_post_evaluation_reset(
        [this](auto deadline) { return reset_after_runtime_evaluation(deadline); });
}

void ScriptedUiSession::set_log_callback(LogCallback callback) {
    log_callback_ = callback ? std::move(callback) : default_log_callback();
}

std::uint64_t ScriptedUiSession::add_log_callback(LogCallback callback) {
    if (!callback)
        return 0;
    const auto token = ++next_log_subscription_;
    log_subscribers_.emplace(token, std::move(callback));
    return token;
}

void ScriptedUiSession::remove_log_callback(std::uint64_t token) {
    if (token != 0)
        log_subscribers_.erase(token);
}

void ScriptedUiSession::dispatch_log(std::string_view level,
                                     std::string_view message) {
    auto primary = log_callback_;
    std::vector<LogCallback> subscribers;
    subscribers.reserve(log_subscribers_.size());
    for (const auto& [token, callback] : log_subscribers_) {
        (void)token;
        subscribers.push_back(callback);
    }
    if (primary)
        primary(level, message);
    for (const auto& subscriber : subscribers)
        subscriber(level, message);
}

LogCallback ScriptedUiSession::engine_log_callback() {
    // The engine keeps this trampoline stable for its lifetime. Dispatch takes
    // a callback snapshot, so a subscriber may add/remove/replace sinks without
    // destroying the closure currently executing inside ScriptEngine.
    return [this](std::string_view level, std::string_view message) {
        dispatch_log(level, message);
    };
}

ScriptedUiSession::~ScriptedUiSession() {
    // A caller that evaluated and never pumped a frame still owes the realm
    // reset. Destroying the session destroys the realm either way, so this is
    // not what contains the evaluated code — it is what stops a reset FAILURE
    // from being silently dropped by a session that never reached a frame.
    // reset_after_runtime_evaluation() logs and quarantines on failure.
    try {
        (void)inspector_bridge_.run_pending_post_evaluation_reset();
    } catch (...) {
        // Destructors must not throw. The reset itself is already contained;
        // this covers the allocation the drain does around it.
    }
    // Detach so a blocked off-thread evaluate wakes with a "detached" result
    // and no later pump touches the engine being destroyed. NOTE: this wakes a
    // *blocked* waiter but does not join a background thread mid-interrupt()/
    // capabilities(); a host that wired the bridge to a control router must
    // stop that server's reader thread, clear DomainHandler's borrowed runtime
    // evaluator, and destroy the evaluator BEFORE destroying this session.
    inspector_bridge_.detach();
    // Any live or previously detached realm may still be borrowed by a native
    // accessibility callback that re-entered owner teardown. Unpublish first,
    // detach realm-owned Views without destroying them, then transfer their
    // graph into an owner the active provider lease can retain until unwind.
    const bool has_runtime_realms =
        bridge_ != nullptr || !retired_runtime_realms_.empty();
    if (has_runtime_realms) {
        try {
            accessibility_tree_will_change(root_);
        } catch (...) {
            // Destructors must not throw. Provider retirement visits every
            // registered handle even when an individual backend fails.
        }
    }
    if (bridge_) {
        bridge_->quarantine_realm();
        try {
            bridge_->clear_quarantined_realm();
        } catch (...) {
            // Selective extraction allocates only during preflight. If that
            // fails, atomically retain the complete attached tree instead of
            // leaving callbacks that borrow the realm about to be destroyed.
            bridge_->force_retire_root_for_owner_teardown();
        }
        runtime_realm_teardown_owner_->current_engine = std::move(engine_);
        runtime_realm_teardown_owner_->current_bridge = std::move(bridge_);
    }
    runtime_realm_teardown_owner_->retired =
        std::move(retired_runtime_realms_);
    if (has_runtime_realms)
        accessibility_retain_until_retired(
            root_, runtime_realm_teardown_owner_);
    store_.flush_deferred_gesture_releases();
}

// Late-attach of the host's GpuSurface. Hosts (e.g. au_view_controller_ios.mm)
// call this AFTER PluginViewHost::create returns, so the JS-side navigator.gpu
// / canvas.getContext('webgpu')
// shim routes through Pulp's live Dawn instance instead of a mock.
void ScriptedUiSession::attach_gpu_surface(render::GpuSurface* gpu_surface) {
    gpu_surface_ = gpu_surface;
    if (!runtime_realm_quarantined_ && bridge_) {
        bridge_->attach_gpu_surface(gpu_surface);
    }
    // Stashed in gpu_surface_ so that the next hot-reload rebuild_from_code
    // passes the same surface into the freshly-constructed WidgetBridge.
}

bool ScriptedUiSession::load(std::string* error) {
    if (runtime_realm_quarantined_) {
        if (error) *error = "scripted UI runtime realm is quarantined";
        return false;
    }
    auto code = read_text_file(script_path_);
    if (code.empty()) {
        if (error) *error = "could not read script file: " + script_path_.string();
        return false;
    }

    if (!rebuild_from_code(code, script_path_, false, error)) {
        return false;
    }

    if (hot_reload_enabled_) {
        // On a platform shipping the no-op stub the reloader accepts the flag and
        // then ignores every save, so say so once rather than look broken. The
        // branch compiles away entirely where a watcher exists.
        if constexpr (!HotReloader::kWatchesFiles) {
            static std::once_flag warned;
            std::call_once(warned, [] {
                runtime::log_info(
                    "Scripted UI hot reload requested, but this platform has no file "
                    "watcher — edits to the script will not reload. Editor rebuilds "
                    "following a DSP hot-swap still apply.");
            });
        }
        reloader_ = std::make_unique<HotReloader>(script_path_, [this](const std::string& next_code) {
            std::string reload_error;
            if (!rebuild_from_code(next_code, script_path_, true, &reload_error)) {
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
    if (runtime_realm_quarantined_) {
        if (error) *error = "scripted UI runtime realm is quarantined";
        return false;
    }
    auto code = read_text_file(script_path_);
    if (code.empty()) {
        if (error) *error = "could not read script file: " + script_path_.string();
        return false;
    }
    // preserve_state=true: keep widget values across the rebuild; rebuild_from_code
    // probes the new code on a throwaway tree first, so a bad reload leaves the
    // current UI intact.
    return rebuild_from_code(code, script_path_, /*preserve_state=*/true, error);
}

bool ScriptedUiSession::reload_from(std::filesystem::path script_path, std::string* error) {
    script_path_ = std::move(script_path);
    theme_path_ = script_path_.parent_path() / "theme.json";
    last_theme_exists_ = std::filesystem::exists(theme_path_);
    last_theme_write_time_ = last_theme_exists_ ? safe_last_write_time(theme_path_) : std::nullopt;
    return reload(error);
}

bool ScriptedUiSession::poll(std::string* error) {
    // A completed Runtime.evaluate owes a realm reset. Discharge it first, at
    // the top of the frame: before this frame's bridge pump could execute a
    // timer, animation frame, Promise job, or patched callback the evaluated
    // code left behind, and before this frame's request pump can queue another
    // evaluation. Reconstructing here rather than inside evaluate() is what
    // lets a caller keep the widget pointers it held across the request until
    // it returns to its run loop; the containment is identical, because
    // nothing evaluated reaches a frame on either schedule.
    const auto pending_reset_error =
        inspector_bridge_.run_pending_post_evaluation_reset();
    // Destruction of the realm retired by the previous inspector pump may call
    // platform hosts or user-supplied widget releasers. Run it before this
    // frame's request pump, never inside the previous response deadline.
    // Destruction may enter host/user widget releasers, which can re-enter
    // owner-thread Runtime.evaluate and append a fresh retirement. Reserve the
    // outgoing batch first, but keep every realm session-owned until the
    // throwable provider barrier succeeds.
    std::vector<RetiredRuntimeRealm> retired_batch;
    const auto retired_count = retired_runtime_realms_.size();
    retired_batch.reserve(retired_count);
    if (accessibility_retirement_pending_) {
        // Provider shutdown can wait for native clients, so it belongs in this
        // unbounded owner-thread pump rather than the Runtime.evaluate response
        // fence. A failed reset keeps the partial realm quarantined and native
        // providers retired; only a successful replacement is republished.
        try {
            accessibility_tree_will_change(root_);
            if (!runtime_realm_quarantined_)
                accessibility_tree_changed(root_);
            accessibility_retirement_pending_ = false;
        } catch (const std::exception& e) {
            if (error) *error = e.what();
            return false;
        } catch (...) {
            if (error) *error = describe_exception();
            return false;
        }
    }
    if (retired_count != 0
        && accessibility_tree_retirement_ready(root_)) {
        for (std::size_t i = 0; i < retired_count; ++i)
            retired_batch.push_back(std::move(retired_runtime_realms_[i]));
        retired_runtime_realms_.erase(
            retired_runtime_realms_.begin(),
            retired_runtime_realms_.begin()
                + static_cast<std::ptrdiff_t>(retired_count));
    }
    // Only destroy the batch that existed on entry. Provider shutdown above
    // may re-enter Runtime.evaluate and append another retirement; that newer
    // realm remains in the member until a later owner-thread poll.
    retired_batch.clear();
    // Deadline cleanup may close a gesture lease without synchronously entering
    // host code. Deliver those end notifications on the following UI pump,
    // outside the Runtime.evaluate response fence (even if the realm stayed
    // quarantined).
    store_.flush_deferred_gesture_releases();
    // Reported here rather than from evaluate(), which now returns before the
    // reset has been attempted. The fail-closed path is unchanged: the reset
    // already quarantined the partial realm and detached the engine.
    if (!pending_reset_error.empty()) {
        if (error) *error = "evaluated realm reset failed: " + pending_reset_error;
        return false;
    }
    if (runtime_realm_quarantined_)
        return false;
    if (post_evaluation_reset_callback_pending_) {
        // The reset replaced every script-owned View and its callback chain.
        // Notify the host only after the replacement realm is known-good, but
        // before its first frame can accept input. Clear first so a callback
        // that re-enters poll() cannot run twice for one replacement.
        post_evaluation_reset_callback_pending_ = false;
        if (post_evaluation_reset_callback_)
            post_evaluation_reset_callback_();
    }
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
    // Drain one queued inspector evaluate on the engine thread here — after the
    // frame pump, never mid-paint / mid-layout — so an off-thread
    // Runtime.evaluate never races the render. Rebuild the realm before another
    // frame pump can run: arbitrary evaluated code may have queued timers,
    // animation frames, Promise jobs, or event callbacks that would otherwise
    // escape the request watchdog and execute later without a deadline.
    if (inspector_bridge_.pump())
        changed = true;
    if (reloader_ && reloader_->poll_reload()) {
        changed = true;
    }
    if (poll_theme_reload(error)) {
        changed = true;
    }
    return changed;
}

std::string ScriptedUiSession::reset_after_runtime_evaluation(
    ScriptInspectorBridge::EvaluationDeadline deadline) {
    std::string reset_error;
    if (!last_good_code_.empty() && !last_good_script_path_.empty()
        && rebuild_from_code(
            last_good_code_, last_good_script_path_, true, &reset_error, deadline)) {
        post_evaluation_reset_callback_pending_ = true;
        return {};
    }
    if (reset_error.empty())
        reset_error = "no cached last-good scripted UI source";

    runtime::log_error(
        "Scripted UI reset after Runtime.evaluate failed: {}", reset_error);
    inspector_bridge_.detach();
    if (bridge_)
        bridge_->quarantine_realm();
    runtime_realm_quarantined_ = true;
    // Provider shutdown can wait on native accessibility clients, so defer it
    // to poll() outside the Runtime.evaluate deadline even when the timeout
    // happened before an old realm could be detached.
    accessibility_retirement_pending_ = true;
    return reset_error;
}

void ScriptedUiSession::set_repaint_callback(std::function<void()> cb) {
    repaint_callback_ = std::move(cb);
    if (bridge_) {
        bridge_->set_repaint_callback(repaint_callback_);
    }
}

void ScriptedUiSession::set_post_evaluation_reset_callback(
    std::function<void()> cb) {
    post_evaluation_reset_callback_ = std::move(cb);
}

void ScriptedUiSession::attach_native_message_handler(
    std::string handler_name, NativeMessageHandler handler) {
    if (handler_name.empty() || !handler)
        throw std::invalid_argument(
            "native message attachment requires a name and handler");

    auto [it, inserted] = native_message_handlers_.try_emplace(
        handler_name, std::make_shared<NativeMessageAttachment>());
    it->second->handler = std::move(handler);
    if (inserted && engine_ && !runtime_realm_quarantined_)
        install_native_message_handler(*engine_, it->first, it->second, false);
}

void ScriptedUiSession::detach_native_message_handler(
    std::string_view handler_name) {
    if (handler_name.empty())
        return;
    const auto it = native_message_handlers_.find(std::string(handler_name));
    if (it == native_message_handlers_.end())
        return;
    it->second->handler = {};
    native_message_handlers_.erase(it);
}

void ScriptedUiSession::install_native_message_handlers(
    ScriptEngine& engine, bool validation_realm) const {
    for (const auto& [name, attachment] : native_message_handlers_)
        install_native_message_handler(engine, name, attachment, validation_realm);
}

void ScriptedUiSession::install_native_message_handler(
    ScriptEngine& engine, const std::string& name,
    std::shared_ptr<NativeMessageAttachment> attachment,
    bool validation_realm) {
    engine.register_function(
        name,
        [attachment = std::move(attachment), validation_realm](
            const choc::value::Value* args, std::size_t num_args) {
                if (num_args != 1 || args == nullptr || !args[0].isString()) {
                    return choc::value::Value(
                        R"({"ok":false,"error":"native message handler expects one JSON string"})");
                }
                if (validation_realm) {
                    return choc::value::Value(
                        R"({"ok":false,"error":"native message handler unavailable during validation"})");
                }
                if (!attachment->handler) {
                    return choc::value::Value(
                        R"({"ok":false,"error":"native message handler detached"})");
                }
                try {
                    return choc::value::Value(
                        attachment->handler(std::string_view(args[0].getString())));
                } catch (...) {
                    return choc::value::Value(
                        R"({"ok":false,"error":"native message handler failed"})");
                }
            });
}

bool ScriptedUiSession::rebuild_from_code(
    const std::string& code, const std::filesystem::path& source_path,
    bool preserve_state, std::string* error,
    std::optional<ScriptInspectorBridge::EvaluationDeadline> deadline) {
    // JS-axis reload timings (item 1.2). steady_clock; UI/control thread only.
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    const auto ms = [](clock::time_point a, clock::time_point b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };
    last_reload_metrics_ = ReloadMetrics{};   // reset; stays partial on early failure
    std::unique_ptr<ScriptEngine> next_engine;
    std::unique_ptr<WidgetBridge> next_bridge;
    bool accessibility_retired = false;
    const auto republish_accessibility_after_failure = [&] {
        // Deadline recovery is fail-closed: once native nodes are retired, a
        // partially cleared or provisional realm must not become observable to
        // assistive technology. poll() republishes only a successful reset.
        if (!accessibility_retired || deadline) return;
        try {
            accessibility_tree_changed(root_);
        } catch (const std::exception& e) {
            accessibility_retirement_pending_ = true;
            runtime::log_warn(
                "Scripted UI accessibility recovery failed: {}", e.what());
        } catch (...) {
            accessibility_retirement_pending_ = true;
            runtime::log_warn("Scripted UI accessibility recovery failed");
        }
        accessibility_retired = false;
    };
    const auto retain_provisional_realm = [&] {
        if (!next_bridge)
            return;
        if (!deadline) {
            // A normal reload has no fixed cleanup budget. Discard the failed
            // provisional realm through native-only cleanup so callbacks and
            // timers installed before the throw cannot remain pumpable, while
            // leaving the session retryable after the source is corrected.
            next_bridge->clear_for_realm_replacement();
            next_bridge.reset();
            next_engine.reset();
            inspector_bridge_.detach();
            bridge_.reset();
            engine_.reset();
            return;
        }
        // The old bridge was already cleared and destroyed before this
        // provisional bridge could be constructed. Retain the partial realm in
        // the session so reset_after_runtime_evaluation() can quarantine it in
        // constant work; destroying it here would leave its root-owned widgets
        // and callbacks dangling after the deadline.
        inspector_bridge_.detach();
        engine_ = std::move(next_engine);
        bridge_ = std::move(next_bridge);
        inspector_bridge_.attach(engine_.get());
        // The deadline reset's caller quarantines this retained realm in
        // constant work.
    };
    try {
        const auto check_deadline = [&] {
            if (deadline && clock::now() >= *deadline)
                throw std::runtime_error("Runtime.evaluate realm reset timed out");
        };
        check_deadline();
        // Realm reset restores the cached last-good effective theme. It must
        // neither trust evaluated live-theme mutations nor do filesystem I/O
        // or parse a theme file inside the fixed post-evaluation grace window.
        const auto theme_for_reload = deadline
            ? last_good_effective_theme_
            : (preserve_state ? base_theme_ : root_.theme());
        check_deadline();
        auto probe_engine = make_engine();
        install_native_message_handlers(*probe_engine, true);
        check_deadline();
        View probe_root;
        probe_root.set_theme(theme_for_reload);
        probe_root.flex().direction = FlexDirection::column;
        state::StateStore probe_store;
        for (const auto& group : store_.all_groups()) {
            check_deadline();
            probe_store.add_group(group);
        }
        for (const auto& param : store_.all_params()) {
            check_deadline();
            probe_store.add_parameter(param);
            probe_store.set_value(param.id, store_.get_value(param.id));
        }
        check_deadline();
        auto probe_bridge = std::make_unique<WidgetBridge>(
            *probe_engine, probe_root, probe_store, nullptr, granted_capabilities_);
        check_deadline();
        if (runtime_import_enabled_)
            probe_bridge->install_runtime_import_handlers();
        probe_bridge->set_asset_roots(asset_roots_);
        probe_bridge->set_script_base_dir(source_path.parent_path());
        load_script_before_deadline(*probe_bridge, *probe_engine, code, deadline);
        const auto t_probe = clock::now();

        // Pre-resolve the theme override HERE — the last FALLIBLE step — BEFORE
        // we snapshot/clear/commit, so a bad theme file fails the reload with the
        // live UI fully intact instead of AFTER the bridge is already swapped
        // (rollback-safety, item 1.5). The apply past the commit is infallible.
        Theme resolved_theme = theme_for_reload;
        bool next_theme_exists = last_theme_exists_;
        auto next_theme_write_time = last_theme_write_time_;
        if (!deadline && theme_reload_enabled_ &&
            !resolve_theme_override(theme_for_reload, resolved_theme, next_theme_exists,
                                    next_theme_write_time, error)) {
            last_reload_metrics_.probe_ms = ms(t0, t_probe);
            last_reload_metrics_.total_ms = ms(t0, clock::now());
            return false;  // nothing snapshot/cleared/committed yet — old UI intact
        }

        WidgetReloadSnapshot saved_values;
        if (preserve_state && bridge_) {
            bridge_->snapshot_values(
                saved_values, check_deadline, /*include_custom_state=*/!deadline);
            // Native providers borrow Views from the current bridge. Provider
            // disconnection may be deferred while an accessibility callback is
            // active, even during an ordinary reload, so retain the detached
            // realm until poll() proves every retired fragment lease drained.
            retired_runtime_realms_.emplace_back();
            // Close native provider call gates before any View detaches. The
            // deadline path defers only the potentially blocking disconnect
            // drain and replacement publication to poll(); it must not leave
            // stale providers callable against an invisible retained realm.
            accessibility_retired = true;
            accessibility_tree_will_change(root_);
            if (deadline)
                accessibility_retirement_pending_ = true;
            try {
                if (deadline)
                    bridge_->clear_for_realm_replacement(check_deadline);
                else
                    bridge_->clear_for_realm_replacement();
            } catch (...) {
                retired_runtime_realms_.pop_back();
                throw;
            }
            auto& retired = retired_runtime_realms_.back();
            retired.engine = std::move(engine_);
            retired.bridge = std::move(bridge_);
        }
        check_deadline();
        const auto t_snapshot = clock::now();

        root_.set_theme(theme_for_reload);
        check_deadline();
        next_engine = make_engine(engine_log_callback());
        install_native_message_handlers(*next_engine, false);
        check_deadline();
        next_bridge = std::make_unique<WidgetBridge>(
            *next_engine, root_, store_, gpu_surface_, granted_capabilities_);
        check_deadline();
        if (runtime_import_enabled_)
            next_bridge->install_runtime_import_handlers();
        // Re-attach on every reload without retaining a processor generation.
        next_bridge->set_value_channel_access(value_channel_access_);
        next_bridge->set_asset_roots(asset_roots_);
        next_bridge->set_script_base_dir(source_path.parent_path());
        if (repaint_callback_) {
            next_bridge->set_repaint_callback(repaint_callback_);
        }
        load_script_before_deadline(*next_bridge, *next_engine, code, deadline);
        check_deadline();
        if (!deadline)
            base_theme_ = root_.theme();

        // Detach the bridge from the OLD engine BEFORE the move destroys it, so
        // an off-thread interrupt() can't dereference a freed ScriptEngine. The
        // engine pointer is only mutated on this thread, so detach()+attach()
        // bracket the swap atomically w.r.t. the engine thread; a concurrent
        // interrupt() either completes on the still-alive old engine or sees the
        // null gap.
        inspector_bridge_.detach();
        engine_ = std::move(next_engine);
        bridge_ = std::move(next_bridge);
        // Re-point the inspector bridge at the freshly-committed engine so a
        // debug console survives hot reloads. attach() runs on the UI/engine
        // thread (same thread as poll()'s pump), which it records for
        // inline-eval detection.
        inspector_bridge_.attach(engine_.get());
        check_deadline();
        // Infallible apply of the pre-resolved theme — no failure point past the
        // commit (item 1.5). A deadline reset must reapply the cached effective
        // theme after source load because the source itself may call setTheme().
        if (deadline || theme_reload_enabled_) {
            root_.set_theme(resolved_theme);
        }
        if (!deadline && theme_reload_enabled_) {
            last_theme_exists_ = next_theme_exists;
            last_theme_write_time_ = next_theme_write_time;
        }
        const auto t_rebuild = clock::now();
        if (preserve_state) {
            bridge_->restore_values(
                saved_values, check_deadline, /*include_custom_state=*/!deadline);
        }
        check_deadline();
        const auto t_restore = clock::now();

        last_reload_metrics_.probe_ms = ms(t0, t_probe);
        last_reload_metrics_.snapshot_ms = ms(t_probe, t_snapshot);
        last_reload_metrics_.rebuild_ms = ms(t_snapshot, t_rebuild);
        last_reload_metrics_.restore_ms = ms(t_rebuild, t_restore);
        last_reload_metrics_.total_ms = ms(t0, t_restore);
        last_good_code_ = code;
        last_good_script_path_ = source_path;
        if (!deadline)
            last_good_effective_theme_ = root_.theme();
        if (!deadline) {
            // The pre-clear will-change barrier already made every old native
            // node inert. Republishing the replacement tree is best-effort and
            // must not turn an otherwise committed reload into a false failure.
            try {
                accessibility_tree_changed(root_);
            } catch (const std::exception& e) {
                accessibility_retirement_pending_ = true;
                runtime::log_warn(
                    "Scripted UI accessibility tree rebuild failed: {}", e.what());
            } catch (...) {
                accessibility_retirement_pending_ = true;
                runtime::log_warn(
                    "Scripted UI accessibility tree rebuild failed");
            }
            accessibility_retired = false;
        }
        return true;
    } catch (const std::exception& e) {
        retain_provisional_realm();
        republish_accessibility_after_failure();
        if (error) *error = e.what();
        last_reload_metrics_.total_ms = ms(t0, clock::now());
        return false;
    } catch (...) {
        retain_provisional_realm();
        republish_accessibility_after_failure();
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
    last_good_effective_theme_ = merged;
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
