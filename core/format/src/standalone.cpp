#include <pulp/format/standalone.hpp>
#include <pulp/format/detail/screenshot_capture.hpp>
#include <pulp/format/detail/standalone_editor_chrome.hpp>
#include <pulp/format/editor_ui.hpp>
#include <pulp/format/settings_panel.hpp>
#include <pulp/format/view_bridge.hpp>
#include <pulp/view/window_host.hpp>
#if !defined(__ANDROID__) && defined(PULP_HAS_INSPECT)
#include <pulp/inspect/inspector_overlay.hpp>
#endif
#include <pulp/runtime/log.hpp>
#include <pulp/runtime/system.hpp>

namespace pulp::format {

StandaloneApp::StandaloneApp(ProcessorFactory factory)
    : factory_(factory)
{
}

StandaloneApp::~StandaloneApp() {
    stop();
}

bool StandaloneApp::start() {
    // Create the processor once and reuse it across audio reconfigurations
    // (apply_config soft-restart). Recreating it on every settings change would
    // dangle an editor ViewBridge holding a Processor& (#2693); parameters are
    // defined a single time so the StateStore isn't re-registered on restart.
    if (!processor_) {
        processor_ = factory_();
        if (!processor_) {
            runtime::log_error("Standalone: failed to create processor");
            return false;
        }
        processor_->set_state_store(&store_);
        processor_->define_parameters(store_);
    }

    auto desc = processor_->descriptor();
    runtime::log_info("Standalone: starting '{}'", desc.name);

    // Set up audio
    audio_system_ = audio::create_audio_system();
    audio_device_ = audio_system_->create_device(config_.audio_device_id);
    if (!audio_device_) {
        runtime::log_error("Standalone: failed to create audio device");
        return false;
    }

    audio::DeviceConfig audio_config;
    audio_config.device_id = config_.audio_device_id;
    audio_config.sample_rate = config_.sample_rate;
    audio_config.buffer_size = config_.buffer_size;
    audio_config.output_channels = config_.output_channels;
    audio_config.input_channels = config_.input_channels;

    if (!audio_device_->open(audio_config)) {
        runtime::log_error("Standalone: failed to open audio device");
        return false;
    }

    // Prepare processor
    PrepareContext prep;
    prep.sample_rate = config_.sample_rate;
    prep.max_buffer_size = config_.buffer_size;
    prep.input_channels = config_.input_channels;
    prep.output_channels = config_.output_channels;
    processor_->prepare(prep);

    // Pre-allocate test signal buffer and pointer arrays (no audio-thread allocation)
    test_signal_.set_sample_rate(config_.sample_rate);
    int test_ch = std::max(config_.input_channels, config_.output_channels);
    if (test_ch < 2) test_ch = 2;
    test_buffer_.resize(static_cast<size_t>(test_ch), static_cast<size_t>(config_.buffer_size));
    test_ptrs_.resize(static_cast<size_t>(test_ch));
    for (int c = 0; c < test_ch; ++c)
        test_ptrs_[static_cast<size_t>(c)] = test_buffer_.view().channel_ptr(static_cast<size_t>(c));
    meter_ptrs_.resize(static_cast<size_t>(std::max(test_ch, config_.input_channels)));

    // Pre-allocate silence buffer for when no input device is available
    int silence_ch = std::max(config_.output_channels, 2);
    silence_buffer_.resize(static_cast<size_t>(silence_ch), static_cast<size_t>(config_.buffer_size));
    silence_ptrs_.resize(static_cast<size_t>(silence_ch));
    for (int c = 0; c < silence_ch; ++c)
        silence_ptrs_[static_cast<size_t>(c)] = silence_buffer_.view().channel_ptr(static_cast<size_t>(c));

    // Set up MIDI input (optional)
    if (desc.accepts_midi) {
        midi_system_ = midi::create_midi_system();
        auto inputs = midi_system_->enumerate_inputs();
        if (!inputs.empty()) {
            midi_input_ = midi_system_->create_input();
            auto midi_port_id = config_.midi_input_id.empty()
                ? inputs[0].id : config_.midi_input_id;
            midi_input_->open(midi_port_id, [this](const midi::MidiEvent& event) {
                std::lock_guard lock(midi_mutex_);
                pending_midi_.add(event);
            });
            if (midi_input_->is_open()) {
                runtime::log_info("Standalone: MIDI input connected");
            }
        }
    }

    // Start audio
    running_.store(true);
    auto ok = audio_device_->start([this](
        const audio::BufferView<const float>& input,
        audio::BufferView<float>& output,
        const audio::CallbackContext& ctx)
    {
        // Collect pending MIDI
        midi::MidiBuffer midi_in, midi_out;
        {
            std::lock_guard lock(midi_mutex_);
            midi_in = std::move(pending_midi_);
            pending_midi_.clear();
        }

        // Determine actual input: test signal overrides hardware input
        const audio::BufferView<const float>* actual_input = &input;
        audio::BufferView<const float> test_input_view;
        if (test_signal_.is_active()) {
            int ch = static_cast<int>(test_ptrs_.size());
            // Use pre-allocated pointer array — no allocation on audio thread
            test_signal_.fill(test_ptrs_.data(), ch, ctx.buffer_size);
            test_input_view = audio::BufferView<const float>(
                const_cast<const float* const*>(test_ptrs_.data()),
                test_ptrs_.size(), static_cast<size_t>(ctx.buffer_size));
            actual_input = &test_input_view;
        }

        // Provide silence when no input is available (prevents null-channel crash)
        audio::BufferView<const float> silence_view;
        if (actual_input->num_channels() == 0) {
            silence_view = audio::BufferView<const float>(
                silence_ptrs_.data(), silence_ptrs_.size(),
                static_cast<size_t>(ctx.buffer_size));
            actual_input = &silence_view;
        }

        // Push input meter data using pre-allocated pointer array
        if (actual_input->num_channels() > 0) {
            size_t meter_ch = actual_input->num_channels();
            for (size_t c = 0; c < meter_ch && c < meter_ptrs_.size(); ++c)
                meter_ptrs_[c] = actual_input->channel_ptr(c);
            input_meter_bridge_.analyze_and_push(
                meter_ptrs_.data(),
                static_cast<int>(meter_ch),
                ctx.buffer_size);
        }

        ProcessContext proc_ctx;
        proc_ctx.sample_rate = ctx.sample_rate;
        proc_ctx.num_samples = ctx.buffer_size;
        proc_ctx.position_samples = static_cast<int64_t>(ctx.sample_position);

        processor_->process(output, *actual_input, midi_in, midi_out, proc_ctx);
    });

    if (!ok) {
        runtime::log_error("Standalone: failed to start audio");
        running_.store(false);
        return false;
    }

    auto device_info = audio_device_->info();
    runtime::log_info("Standalone: running on '{}' at {} Hz, buffer {}",
        device_info.name, config_.sample_rate, config_.buffer_size);
    return true;
}

bool StandaloneApp::apply_config(const StandaloneConfig& new_config) {
    bool was_running = running_.load();
    // Soft restart: tear down only the audio/MIDI devices and rebuild them for
    // the new config, KEEPING the processor instance (start() reuses it and
    // re-prepare()s it). A full stop()+start() would recreate the processor and
    // dangle an editor ViewBridge holding a Processor& (#2693).
    if (was_running) stop_audio_keep_processor();
    config_ = new_config;
    if (was_running) return start();
    return true;
}

bool StandaloneApp::run_with_editor(bool use_gpu) {
    const auto effective_config = detail::standalone_config_from_environment(config_);
    if (detail::standalone_headless_requires_screenshot(effective_config)) {
        runtime::log_error(
            "Standalone: headless/CI mode requires a screenshot path; "
            "set StandaloneConfig::screenshot_path or PULP_SCREENSHOT");
        return false;
    }

    if (!start()) return false;

    if (!processor_ || !processor_->has_editor()) {
        runtime::log_error("Standalone: processor has no editor");
        stop();
        return false;
    }

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
        std::move(root), effective_config, audio_system_.get(), midi_system_.get(), &input_meter_bridge_,
        detail::StandaloneSettingsActions{
            .apply_config = [this](const StandaloneConfig& cfg) {
                return apply_config(cfg);
            },
            .rebind_after_apply = [this](SettingsPanel& settings_panel) {
                settings_panel.bind_systems(audio_system_.get(), midi_system_.get());
            },
            .on_test_signal_changed = [this](const TestSignalConfig& cfg) {
                test_signal_.set_config(cfg);
            },
            .on_file_load = [this](const std::string& path) {
                test_signal_.load_file(path);
            },
            .on_file_transport = [this](bool play, bool loop) {
                test_signal_.set_loop(loop);
                if (play) test_signal_.play(); else test_signal_.stop();
            },
        });
    auto* settings_ptr = chrome.settings_panel();
    auto& window_root = chrome.window_root();

    // Build WindowOptions from the bridge's cached ViewSize hints so
    // min_width/min_height propagate to platform window hosts that
    // honor them (#1362).
    auto opts = detail::make_standalone_window_options(
        size_hints, chrome, desc.name + " — Standalone", use_gpu);
    opts.initially_hidden = effective_config.headless;

    auto window = view::WindowHost::create(window_root, opts);
    if (!window) {
        runtime::log_error("Standalone: WindowHost::create() failed");
        bridge->close();
        stop();
        return false;
    }

    // Window host is live — fire Processor::on_view_opened now.
    bridge->notify_attached();

    auto* bridge_raw = bridge.get();
    detail::attach_standalone_editor_bridge(*window, chrome, *bridge);

    auto* scripted_ui_ptr = bridge->scripted_ui();
    detail::install_scripted_ui_repaint_callback(scripted_ui_ptr, *window);

    // Close the ViewBridge *before* stop() tears down the Processor.
    // `bridge->close()` dispatches Processor::on_view_closed(*view),
    // which reads the host-side Processor; if `stop()` had already
    // reset processor_, the callback would fire on freed memory.
    window->set_close_callback([this, bridge_raw]() {
        if (bridge_raw) bridge_raw->close();
        stop();
    });

#if !defined(__ANDROID__) && defined(PULP_HAS_INSPECT)
    // Create inspector overlay — activated via Cmd+I / Ctrl+I
    auto* inspector_host = &window_root;
    auto inspector = std::make_unique<inspect::InspectorOverlay>(*inspector_host);
    auto* inspector_ptr = inspector.get();
    inspect::install_inspector_hooks(*inspector);
#endif

    // Wire inspector into the idle callback to push overlay paint each frame.
    // The inspector uses View::overlay_queue() for rendering and intercepts
    // key events through the root view's on_global_click callback for Cmd+I.
    //
    // pulp #468 — extract the pre-screenshot idle work into a std::function
    // so the headless screenshot block (further below) can compose with it
    // instead of clobbering via a second set_idle_callback.
    std::function<void()> pre_screenshot_idle;
#if !defined(__ANDROID__) && defined(PULP_HAS_INSPECT)
    if (scripted_ui_ptr) {
        pre_screenshot_idle = [scripted_ui_ptr, settings_ptr, inspector_ptr, inspector_host] {
            if (scripted_ui_ptr) scripted_ui_ptr->poll();
            if (settings_ptr) settings_ptr->poll();
            if (inspector_ptr->is_active()) {
                view::View::overlay_queue().push_back({
                    [inspector_ptr](canvas::Canvas& canvas) {
                        inspector_ptr->paint(canvas);
                    },
                    inspector_host
                });
            }
        };
        window->set_idle_callback(pre_screenshot_idle);
    } else {
        pre_screenshot_idle = [settings_ptr, inspector_ptr, inspector_host] {
            if (settings_ptr) settings_ptr->poll();
            if (inspector_ptr->is_active()) {
                view::View::overlay_queue().push_back({
                    [inspector_ptr](canvas::Canvas& canvas) {
                        inspector_ptr->paint(canvas);
                    },
                    inspector_host
                });
            }
        };
        window->set_idle_callback(pre_screenshot_idle);
    }

    // Enable inspector by default when PULP_INSPECTOR env var is set
    if (runtime::get_env("PULP_INSPECTOR")) {
        inspector_ptr->set_active(true);
        runtime::log_info("Standalone: inspector enabled via PULP_INSPECTOR env var");
    }
#else
    detail::install_standalone_idle_callback(*window, scripted_ui_ptr, settings_ptr);
#endif

    if (!opts.initially_hidden)
        window->show();

    detail::log_standalone_window_open(w, h, use_gpu, bridge->uses_script_ui(), chrome);

    // ── Headless one-shot screenshot (SDK-codified, pulp #468 follow-up) ──
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
            ? effective_config.screenshot_frame_delay : 30;
        cap.path = effective_config.screenshot_path;
        cap.capture_fn = [host] { return host->capture_back_buffer_png(); };
        cap.close_fn   = [host] { host->request_close(); };
        cap.on_error   = [out_path = effective_config.screenshot_path](const std::string& msg) {
            runtime::log_error("Standalone: screenshot {} ({})", msg, out_path);
        };
        // Compose with the pre-screenshot idle callback (inspector +
        // scripted_ui poll + settings poll). The wrap calls the prior
        // callback every frame, then on frame N captures + closes.
        auto prior = pre_screenshot_idle;  // may be empty (Android path)
        host->set_idle_callback([prior, cap = std::move(cap)]() mutable {
            if (prior) prior();
            cap();
        });
        runtime::log_info("Standalone: screenshot mode armed — will capture to {} after {} frames",
                          effective_config.screenshot_path, cap.delay);
    }

    // Blocks until the window is closed. The close callback above has
    // already fired bridge->close() (before stop() reset processor_),
    // so on_view_closed ran while both processor and released view were
    // still alive. Calling close() again here is a no-op because it's
    // idempotent after the first close() clears view_.
    window->run_event_loop();

    stop();
    return true;
}

void StandaloneApp::stop_audio_keep_processor() {
    running_.store(false);
    if (midi_input_) {
        midi_input_->close();
        midi_input_.reset();
    }
    if (audio_device_) {
        audio_device_->stop();
        audio_device_->close();
        audio_device_.reset();
    }
}

void StandaloneApp::stop() {
    stop_audio_keep_processor();
    if (processor_) {
        processor_->release();
        processor_.reset();
    }
}

} // namespace pulp::format
