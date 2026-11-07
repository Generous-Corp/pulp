// standalone_view_layer.cpp: the half of StandaloneApp that needs Pulp's view
// layer. Compiled into pulp::standalone and pulp::standalone-native; the
// view-free pulp::standalone-contract compiles standalone_view_layer_none.cpp
// in its place.

#include <pulp/format/app_updates.hpp>
#include <pulp/format/detail/delayed_action.hpp>
#include <pulp/format/detail/frame_time_summary.hpp>
#include <pulp/format/detail/screenshot_capture.hpp>
#include <pulp/format/detail/standalone_editor_chrome.hpp>
#include <pulp/format/detail/standalone_key_driver.hpp>
#include <pulp/format/detail/standalone_key_schedule.hpp>
#include <pulp/format/detail/standalone_key_sequence.hpp>
#include <pulp/format/detail/standalone_musical_typing.hpp>
#include <pulp/format/detail/standalone_updater.hpp>
#include <pulp/format/detail/standalone_window_factory.hpp>
#include <pulp/format/editor_idle_pump.hpp>
#include <pulp/format/editor_ui.hpp>
#include <pulp/format/settings_panel.hpp>
#include <pulp/format/standalone.hpp>
#include <pulp/format/view_bridge.hpp>
#include <pulp/platform/file_dialog.hpp>
#include <pulp/runtime/log.hpp>
#include <pulp/view/audio_inspector_panel.hpp>
#include <pulp/view/command_registry.hpp>
#include <pulp/view/register_audio_widgets.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/window_host.hpp>

#include <array>
#include <cstdio>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if PULP_ENABLE_AUDIO_PROBES
#include <pulp/format/detail/standalone_audio_capture_rolling_wav.hpp>
#include <pulp/format/detail/standalone_audio_capture_wav.hpp>
#include <pulp/format/detail/standalone_audio_probe_json.hpp>
#include <pulp/format/detail/standalone_audio_scope_json.hpp>
#include <pulp/view/audio_inspector_window.hpp>
#endif

namespace pulp::format {

namespace {

template <typename T, typename... Args>
std::unique_ptr<T, void (*)(T*)> make_view_layer(Args&&... args) {
    return {new T(std::forward<Args>(args)...), [](T* p) { delete p; }};
}

} // namespace

void StandaloneApp::attach_view_layer() {
    pulp::view::register_audio_widgets();
}

void StandaloneApp::prepare_view_input(const StandaloneConfig& config) {
    if (!config.enable_musical_typing_keyboard)
        return;
    command_registry_ = make_view_layer<view::CommandRegistry>();
    musical_typing_ = make_view_layer<detail::StandaloneMusicalTyping>(
        [this](const midi::MidiEvent& event) { return ui_midi_collector_.push_now(event); });
}

void StandaloneApp::drain_view_input(midi::MidiBuffer& midi, int frames) noexcept {
    if (musical_typing_)
        musical_typing_->drain_recovery_into(midi, frames);
}

void StandaloneApp::shutdown_view_input() noexcept {
    if (musical_typing_)
        musical_typing_->shutdown();
}

int StandaloneApp::view_waveform_capacity() const noexcept {
    return view::AudioWaveformView::kCapacity;
}

bool StandaloneApp::run_view_editor(bool use_gpu, const StandaloneConfig& effective_config) {
    if (!processor_ || !processor_->has_editor()) {
        runtime::log_error("Standalone: processor has no editor");
        stop();
        return false;
    }

    // Opt into the platform's built-in file-dialog backend so editor file
    // pickers work natively. No-op on macOS (compiled-in impl); on Linux this
    // installs the xdg-desktop-portal bridge when libdbus is available.
    platform::FileDialog::install_native_backend();

    // In-app updates: Sparkle when the app embeds it via pulp_add_sparkle(),
    // or the development stub. Installed before the editor opens, so a JS
    // editor's update bridge reads a live service from its first render, and
    // before the chrome is built, so the Settings panel offers its Updates tab.
    const auto updater_plan = detail::plan_standalone_updater(
        detail::probe_standalone_updater_environment(effective_config.headless));
    set_app_update_service(detail::make_standalone_update_service(updater_plan));

    std::string editor_error;
    auto bridge = std::make_unique<ViewBridge>(
        *processor_, store_,
        ViewBridge::Options{.enable_hot_reload = true, .role = ViewRole::Editor});
    if (!bridge->open(&editor_error)) {
        runtime::log_error("Standalone: failed to build editor UI ({})", editor_error);
        stop();
        return false;
    }
    // Hand off view ownership to either the top-level TabPanel or the
    // window host directly; bridge retains a raw pointer so
    // `notify_attached`, `resize`, and `close` continue to dispatch
    // `Processor::on_view_*` on the same view instance.
    auto root = bridge->release_view();
    if (!root) {
        runtime::log_error("Standalone: ViewBridge::release_view returned null");
        stop();
        return false;
    }

    const auto& size_hints = bridge->size_hints();
    const uint32_t w = size_hints.preferred_width;
    const uint32_t h = size_hints.preferred_height;
    auto desc = processor_->descriptor();

    auto chrome = detail::make_standalone_editor_chrome(
        std::move(root), effective_config, audio_system_.get(), midi_system_.get(),
        &input_meter_bridge_,
        detail::StandaloneSettingsActions{
            .apply_config = [this](const StandaloneConfig& cfg) { return apply_config(cfg); },
            .rebind_after_apply =
                [this](SettingsPanel& settings_panel) {
                    settings_panel.bind_systems(audio_system_.get(), midi_system_.get());
                },
            .on_test_signal_changed =
                [this](const TestSignalConfig& cfg) { test_signal_.set_config(cfg); },
            .on_file_load = [this](const std::string& path) { test_signal_.load_file(path); },
            .on_file_transport =
                [this](bool play, bool loop) {
                    test_signal_.set_loop(loop);
                    if (play)
                        test_signal_.play();
                    else
                        test_signal_.stop();
                },
        },
        processor_->settings_sections(), // plugin-contributed Settings tabs (e.g. Models)
        &output_meter_bridge_);
    auto* settings_ptr = chrome.settings_panel();
    auto& window_root = chrome.window_root();

    if (effective_config.enable_musical_typing_keyboard) {
        musical_typing_->register_command(*command_registry_);
        musical_typing_->install_key_route(window_root);
    }

    // Build WindowOptions from the bridge's cached ViewSize hints so
    // min_width/min_height propagate to platform window hosts that honor them.
    auto opts = detail::make_standalone_window_options(
        size_hints, chrome, desc.name + " — Standalone", use_gpu, bridge->editor_background_rgb());
    detail::add_standalone_settings_menu_command(opts, chrome);
    detail::add_standalone_updater_menu_command(opts, updater_plan);
    if (updater_plan.start_at_launch)
        detail::start_standalone_updater();
    opts.initially_hidden = effective_config.headless;
    if (musical_typing_)
        musical_typing_->add_menu_command(opts);

    detail::ensure_standalone_window_factory();
    auto window = view::WindowHost::create(window_root, opts);
    if (!window) {
        runtime::log_error("Standalone: WindowHost::create() failed");
        bridge->close();
        stop();
        return false;
    }
    if (musical_typing_)
        musical_typing_->set_primary_window(window.get());
    detail::configure_standalone_design_viewport(*window, size_hints, chrome);

    // Grow the window when the Settings tab is active so the Audio/MIDI panel —
    // which needs far more height than a fixed-size editor — lays its device
    // dropdowns and meters out at full size instead of being squished into the
    // editor's height; shrink back to the editor's own size on the editor tab.
    // Drives the SAME design-viewport + content-size path the keyboard resize
    // uses (set_design_viewport / set_fixed_aspect_ratio / request_content_size),
    // so paint scale and the OS aspect lock track each tab. Editor stays pinned
    // at its declared size (no letterbox); Settings reflows to fill its taller
    // window. Framework-level so every standalone settings chrome benefits.
    detail::configure_standalone_tab_resizing(*window, chrome, *bridge);

    // Editor-INITIATED resize: let the editor ask the standalone window to
    // resize itself (e.g. a chrome-hiding mode wanting a smaller shape). Drives
    // the SAME design-viewport + content-size path the Settings tab uses above,
    // and updates the bridge's reported hints so the new aspect sticks. Cleared
    // in the close callback before the window / bridge it captures die.
#if defined(__APPLE__) && TARGET_OS_OSX
    detail::install_standalone_editor_resize_handler(*processor_, this, *window, *bridge);
#endif

    // Window host is live — fire Processor::on_view_opened now.
    bridge->notify_attached();

    if (control_host_ && !control_host_->attach_editor(*bridge, *bridge->view(), *window)) {
        runtime::log_error("Standalone: canonical control host failed to attach the editor");
        detail::retire_standalone_editor(*window, *bridge);
        stop();
        return false;
    }

    auto* bridge_raw = bridge.get();
    detail::attach_standalone_editor_bridge(*window, chrome, *bridge);

    auto* scripted_ui_ptr = bridge->scripted_ui();
    detail::install_scripted_ui_repaint_callback(scripted_ui_ptr, *window);

    // Close the ViewBridge *before* stop() tears down the Processor.
    // `bridge->close()` dispatches Processor::on_view_closed(*view),
    // which reads the host-side Processor; if `stop()` had already
    // reset processor_, the callback would fire on freed memory.
    auto* window_raw = window.get();
    std::function<void()> close_editor = [this, window_raw, bridge_raw]() {
        // Drop the editor→host resize handler before the window / bridge it
        // captures are torn down by stop().
        if (processor_)
            processor_->set_editor_resize_handler(this, nullptr);
        // Revoke every control callback that borrows the visible editor before
        // ViewBridge dispatches on_view_closed and releases that view.
        if (control_host_)
            control_host_->stop();
        if (window_raw && bridge_raw)
            detail::retire_standalone_editor(*window_raw, *bridge_raw);
        stop();
    };

    window->set_close_callback(std::move(close_editor));

    // Installed before the loop starts: a launch-time open (double-click on a
    // document) is reported from inside run_event_loop(), so a handler wired up
    // afterwards would never see the file that launched the app.
    if (open_files_handler_)
        window->set_open_files_handler(open_files_handler_);

#if PULP_ENABLE_AUDIO_PROBES
    // Audio Inspector tool window. A SEPARATE floating window (sibling of the
    // layout inspector, not a tab in it) that observes `output_probe_`.
    // It dispatches its toggle (Cmd/Ctrl+Shift+A) through a shell-owned
    // CommandRegistry routed via `route_global_keys` — that writes
    // `window_root.on_global_key`, which is distinct from the layout inspector's
    // `on_global_click` (Cmd+I), so the two coexist without clobbering. Only
    // wired when a real WindowHost exists (the show() path needs one).
    view::AudioInspectorWindow* audio_inspector_ptr = nullptr;
    if (window) {
        // Fresh registry + window per run (a prior run's window referenced an
        // already-destroyed root view). Destroy the window before the registry
        // so its RAII handler removal targets a live registry.
        if (!command_registry_)
            command_registry_ = make_view_layer<view::CommandRegistry>();
        audio_inspector_ = make_view_layer<view::AudioInspectorWindow>(
            nullptr, window.get(), view::AudioInspectorWindow::HostFactory{}, desc.name);
        audio_inspector_->set_probe(&output_probe_);
        audio_inspector_->register_command_handler(*command_registry_);
        audio_inspector_ptr = audio_inspector_.get();
    }
#endif

    if (command_registry_)
        view::route_global_keys(window_root, *command_registry_);

    // Wire inspector into the idle callback to push overlay paint each frame.
    // The inspector uses View::overlay_queue() for rendering and intercepts
    // key events through the root view's on_global_click callback for Cmd+I.
    //
    // Keep pre-screenshot idle work in a composable callback so the headless
    // screenshot path can extend it instead of clobbering it with a second
    // set_idle_callback.
    std::function<void()> pre_screenshot_idle;
    // Settings-panel polling plus the debounced save of the plug-in state. Both
    // run on the UI thread from the window's idle loop.
    const std::function<void()> poll_settings = [this, settings_ptr] {
        if (settings_ptr)
            settings_ptr->poll();
        service_state_persistence();
    };
#if PULP_STANDALONE_INSPECTOR
    if (inspector_runtime) {
        auto* inspector_runtime_ptr = inspector_runtime.get();
        pre_screenshot_idle = [poll_settings, inspector_runtime_ptr] {
            poll_settings();
            inspector_runtime_ptr->pump();
        };
        window->set_idle_callback(pre_screenshot_idle);

        // Enable inspector by default when PULP_INSPECTOR env var is set.
        if (runtime::get_env("PULP_INSPECTOR")) {
            inspector_runtime->set_overlay_active(true);
            runtime::log_info("Standalone: inspector enabled via PULP_INSPECTOR env var");
        }
    } else {
        pre_screenshot_idle = detail::make_standalone_idle_callback({}, poll_settings);
        window->set_idle_callback(pre_screenshot_idle);
    }
#else
    pre_screenshot_idle = detail::make_standalone_idle_callback({}, poll_settings);
    window->set_idle_callback(pre_screenshot_idle);
#endif

    if (control_host_) {
        pre_screenshot_idle = detail::make_standalone_control_idle_callback(
            pre_screenshot_idle, *control_host_, *window);
        window->set_idle_callback(pre_screenshot_idle);
    }

    // Run the same automation, restore/reload, and scripted work as embedded
    // plugin editors. Compose it over settings/inspector work so standalone
    // state restores reconcile bound controls on the UI thread too.
    {
        auto prev_idle = pre_screenshot_idle;
        auto editor_idle = make_editor_idle_pump(*bridge_raw);
        pre_screenshot_idle = [prev_idle, editor_idle = std::move(editor_idle)]() mutable {
            editor_idle();
            if (prev_idle)
                prev_idle();
        };
        window->set_idle_callback(pre_screenshot_idle);
    }

#if PULP_ENABLE_AUDIO_PROBES
    // Refresh the Audio Inspector from `output_probe_` every UI tick. Compose
    // with whatever idle work is already wired (inspector overlay / scripted_ui
    // / settings poll) rather than clobbering it: capture the current
    // `pre_screenshot_idle` (empty in the inspector-off build, where the
    // standalone idle callback is installed directly) and call it first.
    // poll() is cheap and safe when the window is hidden or no probe is set.
    if (audio_inspector_ptr) {
        auto prior_idle = pre_screenshot_idle;
        pre_screenshot_idle = [prior_idle, audio_inspector_ptr] {
            if (prior_idle)
                prior_idle();
            audio_inspector_ptr->poll();
        };
        window->set_idle_callback(pre_screenshot_idle);
    }

    // Open the Audio Inspector when PULP_AUDIO_INSPECTOR is set (mirrors the
    // PULP_INSPECTOR layout-inspector block above). The capture ring was sized
    // in start() under the same env, so the window paints a live waveform.
    if (audio_inspector_ptr && runtime::get_env("PULP_AUDIO_INSPECTOR")) {
        audio_inspector_ptr->show();
        runtime::log_info("Standalone: audio inspector enabled via PULP_AUDIO_INSPECTOR env var");
    }
#endif

    // ── Synthetic key-sequence driver (test harness) ────────────────────────
    //
    // When `effective_config.test_key_sequence` is non-empty (set via config or
    // PULP_TEST_KEY_SEQUENCE), press each key in turn on a frame schedule and
    // photograph the surface between presses. This is the keyboard sibling of
    // the pointer-drag hook: it exists so a keyboard-driven UX claim can be
    // backed by a screenshot of the real shipping build instead of a unit test
    // that calls the script layer directly.
    //
    // The delivery deliberately enters through the platform view's own key
    // methods (see standalone_key_driver_mac.mm), reproducing the order the
    // window manager offers a key down in. Driving the script layer directly
    // would bypass the very code path a keyboard regression lives in.
    if (!effective_config.test_key_sequence.empty()) {
        std::vector<detail::KeySequenceStep> steps;
        std::string parse_error;
        if (!detail::parse_key_sequence(effective_config.test_key_sequence, steps, parse_error)) {
            // Fail loudly and press nothing. A partially-run sequence would
            // produce artifacts that look like a completed run.
            runtime::log_error("Standalone: key sequence rejected ({}) — no keys will be sent: {}",
                               parse_error, effective_config.test_key_sequence);
        } else if (!detail::synthetic_key_delivery_supported()) {
            runtime::log_error("Standalone: synthetic key delivery is unsupported on this "
                               "platform — no keys will be sent");
        } else {
            auto* host = window.get();
            auto* editor_view = bridge->view();
            const bool owns_close = effective_config.screenshot_path.empty();
            detail::KeySequenceSchedule schedule(steps, effective_config.test_key_frame_delay);
            const std::string shot_dir = effective_config.test_key_shot_dir;

            auto capture_png = [host, editor_view, w, h]() -> std::vector<uint8_t> {
                if (editor_view) {
                    auto r = pulp::view::capture_view(*editor_view, w, h);
                    if (r.ok && r.used == pulp::view::ScreenshotBackend::default_backend)
                        return r.png;
                }
                return host->capture_back_buffer_png();
            };
            auto write_shot = [shot_dir, capture_png](const std::string& name) {
                if (shot_dir.empty())
                    return;
                const std::string path = shot_dir + "/" + name + ".png";
                auto png = capture_png();
                if (png.empty()) {
                    runtime::log_error("Standalone: key-sequence capture empty for {}", path);
                    return;
                }
                std::ofstream out(path, std::ios::binary);
                out.write(reinterpret_cast<const char*>(png.data()),
                          static_cast<std::streamsize>(png.size()));
                runtime::log_info("Standalone: key-sequence frame written to {}", path);
            };

            auto prior = pre_screenshot_idle;
            pre_screenshot_idle = [prior, host, owns_close, schedule = std::move(schedule),
                                   write_shot = std::move(write_shot)]() mutable {
                if (prior)
                    prior();
                const auto tick = schedule.tick();
                switch (tick.action) {
                case detail::KeySequenceSchedule::Action::wait:
                    break;
                case detail::KeySequenceSchedule::Action::capture_initial:
                    write_shot("key-00-initial");
                    break;
                case detail::KeySequenceSchedule::Action::press: {
                    const auto& step = schedule.steps()[tick.index];
                    const bool sent =
                        detail::deliver_synthetic_key(host->native_content_view_handle(), step);
                    // Log every press, delivered or not, so a run that pressed
                    // fewer keys than requested is visible in the log rather
                    // than inferred from artifacts that silently stopped.
                    if (sent)
                        runtime::log_info("Standalone: key-sequence pressed [{}] {}", tick.index,
                                          step.label);
                    else
                        runtime::log_error("Standalone: key-sequence FAILED to deliver [{}] {}",
                                           tick.index, step.label);
                    break;
                }
                case detail::KeySequenceSchedule::Action::capture: {
                    const auto& step = schedule.steps()[tick.index];
                    char stem[64];
                    std::snprintf(stem, sizeof(stem), "key-%02d-%s",
                                  static_cast<int>(tick.index) + 1, step.label.c_str());
                    write_shot(stem);
                    break;
                }
                case detail::KeySequenceSchedule::Action::finish:
                    runtime::log_info("Standalone: key sequence complete");
                    // Only close when nothing else owns the exit. With
                    // --screenshot also set, the screenshot one-shot closes,
                    // and closing here first would race it out of its frame.
                    if (owns_close)
                        host->request_close();
                    break;
                }
            };
            window->set_idle_callback(pre_screenshot_idle);
            runtime::log_info("Standalone: key sequence armed — {} step(s), {} frame delay, "
                              "shots to '{}'",
                              steps.size(), effective_config.test_key_frame_delay,
                              shot_dir.empty() ? std::string("<none>") : shot_dir);
        }
    }

    if (!opts.initially_hidden)
        window->show();

    detail::log_standalone_window_open(w, h, *window, use_gpu, bridge->uses_script_ui(), chrome);

#if PULP_ENABLE_AUDIO_PROBES
    // ── Programmatic live-probe JSON dump (the agent/CI readout) ──
    //
    // Writes `output_probe().latest()` (+ the AudioStats subset) as a flat
    // JSON object to `audio_probe_json_path`. Factored into a free function
    // (audio_probe_snapshot_to_json) so the snapshot→JSON mapping is unit-
    // tested without a device. Used as a side-effect of the screenshot
    // one-shot when both are set, and drives a dedicated one-shot when only
    // the JSON dump was requested. Empty path → no-op.
    auto write_probe_json = [this](const std::string& path) {
        detail::write_audio_probe_json_file(path, output_probe_);
    };
    auto write_scope_json = [this, effective_config](const std::string& path) {
        detail::write_audio_scope_json_file(path, output_probe_, effective_config);
    };
    auto write_capture_wav = [this, effective_config](const std::string& path) {
        detail::write_audio_capture_wav_file(path, output_probe_, effective_config);
    };
    auto write_capture_rolling = [this, effective_config](const std::string& path) {
        detail::write_audio_capture_rolling_wav_file(
            path, rolling_capture_, effective_config,
            rolling_capture_channels_.load(std::memory_order_relaxed));
    };
#endif

    // ── Headless one-shot screenshot ────────────────────────────────────────
    //
    // When `effective_config.screenshot_path` is non-empty (set via
    // config/env or forwarded `pulp run --screenshot` args), wait
    // `screenshot_frame_delay` frames, then capture the host back buffer and
    // exit. This is the SDK-level shape
    // so every consumer (Spectr, examples, future plugins) gets headless
    // visual-regression capture without bespoke per-app code.
    if (!effective_config.screenshot_path.empty()) {
        auto* host = window.get();
        detail::ScreenshotCapture cap;
        cap.delay = effective_config.screenshot_frame_delay > 0
                        ? effective_config.screenshot_frame_delay
                        : 30;
        cap.path = effective_config.screenshot_path;
        auto* editor_view = bridge->view();
#if PULP_ENABLE_AUDIO_PROBES
        // Sibling capture of the Audio Inspector's OWN window surface, written
        // next to the main screenshot as "<stem>.audio-inspector.png" when the
        // inspector is open. Lets a headless run prove the live panel loaded
        // (PULP_AUDIO_INSPECTOR=1 --screenshot X.png yields X.audio-inspector.png).
        const std::string inspector_png_path = [&] {
            return detail::audio_inspector_screenshot_path(effective_config.screenshot_path);
        }();
#endif
        cap.capture_fn = [host, editor_view, w, h
#if PULP_ENABLE_AUDIO_PROBES
                          ,
                          audio_inspector_ptr, inspector_png_path, write_probe_json,
                          write_scope_json, write_capture_wav, write_capture_rolling,
                          probe_json_path = effective_config.audio_probe_json_path,
                          scope_json_path = effective_config.audio_scope_json_path,
                          capture_wav_path = effective_config.audio_capture_wav_path,
                          capture_rolling_path = effective_config.audio_capture_rolling_path
#endif
        ] {
#if PULP_ENABLE_AUDIO_PROBES
            // Side-effect: dump the live probe metrics as JSON at the same
            // frame the screenshot is taken (composes --screenshot with
            // --audio-probe-json in a single headless run). No-op when unset.
            write_probe_json(probe_json_path);
            write_scope_json(scope_json_path);
            write_capture_wav(capture_wav_path);
            write_capture_rolling(capture_rolling_path);
            // Side-effect: capture the inspector window's own surface at the
            // same frame, before the main window closes. capture_png() returns
            // empty on hosts without GPU capture — skip the write in that case.
            if (audio_inspector_ptr && audio_inspector_ptr->is_visible()) {
                if (auto* ihost = audio_inspector_ptr->window_host()) {
                    auto ipng = ihost->capture_png();
                    if (!ipng.empty()) {
                        std::ofstream out(inspector_png_path, std::ios::binary);
                        out.write(reinterpret_cast<const char*>(ipng.data()),
                                  static_cast<std::streamsize>(ipng.size()));
                        runtime::log_info("Standalone: audio inspector screenshot written to {}",
                                          inspector_png_path);
                    }
                }
            }
#endif
            // If the editor hosts a native overlay (a WebView), use its in-process
            // snapshot (WKWebView takeSnapshot) — capture_back_buffer_png() reads the
            // Skia back buffer and can't see an OS-composited native overlay. For a
            // normal Skia UI, capture_view rasters via `skia`, so we fall through to
            // the live back-buffer capture below.
            if (editor_view) {
                auto r = pulp::view::capture_view(*editor_view, w, h);
                if (r.ok && r.used == pulp::view::ScreenshotBackend::default_backend)
                    return r.png;
            }
            return host->capture_back_buffer_png();
        };
        cap.close_fn = [host] {
            // A screenshot run is finite, so it can say how it went: one
            // frame-time summary and one GPU-diagnostics line, never per frame.
            runtime::log_info("Standalone: {}",
                              detail::format_frame_time_summary(
                                  detail::summarize_frame_times(host->frame_time_samples_ms())));
            host->log_gpu_diagnostics_summary();
            host->request_close();
        };
        cap.on_error = [out_path = effective_config.screenshot_path](const std::string& msg) {
            runtime::log_error("Standalone: screenshot {} ({})", msg, out_path);
        };
        // Compose with the pre-screenshot idle callback (inspector +
        // scripted_ui poll + settings poll). The wrap calls the prior
        // callback every frame, then on frame N captures + closes.
        auto prior = pre_screenshot_idle; // may be empty (Android path)
        host->set_idle_callback([prior, cap = std::move(cap)]() mutable {
            if (prior)
                prior();
            cap();
        });
        runtime::log_info("Standalone: screenshot mode armed — will capture to {} after {} frames",
                          effective_config.screenshot_path, cap.delay);
    }
#if PULP_ENABLE_AUDIO_PROBES
    // Audio-dump one-shots: when a dump was requested WITHOUT --screenshot,
    // reuse the same frame-delay machinery to let the audio path run a few
    // blocks, then write the file and close. (When --screenshot is also set,
    // every dump rode the screenshot capture_fn above instead.)
    //
    // Each mode differs only in which config path arms it, which writer runs,
    // and what the arming log calls it — the delayed one-shot around them is
    // identical, so the modes are a table. A dump has no image bytes to validate
    // or file to write through ScreenshotCapture, so the plain DelayedAction
    // state machine drives them with the write as its side effect.
    //
    // At most ONE arms: the first requested entry wins and the rest are ignored,
    // because a single idle callback can only carry one one-shot.
    else {
        struct AudioDumpOneShot {
            const std::string& path;
            // By value: each writer is a distinct lambda type, so a reference
            // here would dangle on the materialized std::function temporary.
            std::function<void(const std::string&)> write_fn;
            const char* label;
        };
        const std::array<AudioDumpOneShot, 4> dumps{{
            {effective_config.audio_probe_json_path, write_probe_json, "audio-probe-json"},
            {effective_config.audio_scope_json_path, write_scope_json, "audio-scope-json"},
            {effective_config.audio_capture_wav_path, write_capture_wav, "audio-capture-wav"},
            {effective_config.audio_capture_rolling_path, write_capture_rolling,
             "audio-capture-rolling"},
        }};
        for (const auto& dump : dumps) {
            if (dump.path.empty())
                continue;
            auto* host = window.get();
            detail::DelayedAction cap;
            cap.delay = effective_config.screenshot_frame_delay > 0
                            ? effective_config.screenshot_frame_delay
                            : 30;
            cap.action_fn = [write_fn = dump.write_fn, path = dump.path] { write_fn(path); };
            cap.close_fn = [host] { host->request_close(); };
            auto prior = pre_screenshot_idle;
            host->set_idle_callback([prior, cap = std::move(cap)]() mutable {
                if (prior)
                    prior();
                cap();
            });
            runtime::log_info("Standalone: {} mode armed — will dump to {} after {} frames",
                              dump.label, dump.path, cap.delay);
            break;
        }
    }
#endif

    // Blocks until the window is closed. Most window-close paths have
    // already fired bridge->close(), but application-quit paths can return
    // from the event loop without going through that callback. Close here
    // while the processor is still alive; the call is idempotent.
#if PULP_STANDALONE_INSPECTOR
    if (inspector_runtime) {
        // Application quit can leave the event loop without invoking the window's
        // close callback. Begin the same fence-gated retirement used by ordinary
        // window close, then keep the platform's main-thread dispatcher alive until
        // the accepted operation unwinds and the borrowed state is detached.
        window->run_event_loop_until([&] {
            inspector_runtime->stop();
            return inspector_runtime->try_finish_retirement();
        });
        if (control_host_)
            control_host_->stop();
        return !inspector_runtime->startup_failed();
    }
#endif
    window->run_event_loop();
    // Application quit can leave the event loop without invoking the window's
    // close callback. Remove this owner's entry before the bridge/window or
    // processor is torn down; removal is harmless on platforms where no
    // editor-initiated resize handler was installed.
    if (processor_)
        processor_->set_editor_resize_handler(this, nullptr);
    if (control_host_)
        control_host_->stop();
    detail::retire_standalone_editor(*window, *bridge);
    stop();
    return true;
}

} // namespace pulp::format
