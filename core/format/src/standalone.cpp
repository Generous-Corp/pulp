#include <pulp/format/standalone.hpp>
#include <pulp/format/audio_workgroup_client.hpp>
#include <pulp/format/detail/delayed_action.hpp>
#include <pulp/format/detail/screenshot_capture.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/format/detail/standalone_editor_chrome.hpp>
#include <pulp/format/editor_ui.hpp>
#include <pulp/format/settings_panel.hpp>
#include <pulp/format/view_bridge.hpp>
#include <pulp/platform/file_dialog.hpp>
#include <pulp/signal/scoped_flush_denormals.hpp>
#include <pulp/state/properties_file.hpp>
#include <pulp/view/window_host.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string_view>

// The dev inspector (Cmd+I overlay) is gated behind the PULP_ENABLE_INSPECTOR
// compile flag (root CMake option, default ON for dev/examples builds;
// release/standalone-ship builds set it OFF) so a shipped standalone app does
// not expose the developer inspector to end users. It additionally requires
// PULP_HAS_INSPECT (GPU + desktop, the link gate) and a non-Android platform.
// PULP_STANDALONE_INSPECTOR folds all three into one condition used by every
// inspector block below.
#if !defined(PULP_ENABLE_INSPECTOR)
#define PULP_ENABLE_INSPECTOR 1
#endif
#if !defined(__ANDROID__) && defined(PULP_HAS_INSPECT) && PULP_ENABLE_INSPECTOR
#define PULP_STANDALONE_INSPECTOR 1
#else
#define PULP_STANDALONE_INSPECTOR 0
#endif

#if PULP_STANDALONE_INSPECTOR
#include <pulp/inspect/inspector_overlay.hpp>
#endif
#if PULP_ENABLE_AUDIO_PROBES
#include <pulp/audio/audio_probe_json.hpp>
#include <pulp/format/detail/standalone_audio_capture_rolling_wav.hpp>
#include <pulp/format/detail/standalone_audio_capture_wav.hpp>
#include <pulp/format/detail/standalone_audio_probe_json.hpp>
#include <pulp/format/detail/standalone_audio_scope_json.hpp>
#include <pulp/view/audio_inspector_window.hpp>
#include <pulp/view/command_registry.hpp>
#endif
#include <pulp/runtime/log.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>
#include <pulp/runtime/system.hpp>
#include <pulp/runtime/trace_session.hpp>

namespace pulp::format {

namespace {

bool rate_matches(double a, double b) {
    return std::abs(a - b) < 1.0;
}

void constrain_audio_config(StandaloneConfig& config) {
    if (!config.allowed_sample_rates.empty()) {
        const auto it = std::find_if(config.allowed_sample_rates.begin(),
                                     config.allowed_sample_rates.end(),
                                     [&](double allowed) {
                                         return rate_matches(config.sample_rate, allowed);
                                     });
        if (it == config.allowed_sample_rates.end())
            config.sample_rate = config.allowed_sample_rates.front();
    }

    if (!config.allowed_buffer_sizes.empty()) {
        const auto it = std::find(config.allowed_buffer_sizes.begin(),
                                  config.allowed_buffer_sizes.end(),
                                  config.buffer_size);
        if (it == config.allowed_buffer_sizes.end())
            config.buffer_size = config.allowed_buffer_sizes.front();
    }
}

}  // namespace

StandaloneApp::StandaloneApp(ProcessorFactory factory)
    : factory_(factory)
{
}

StandaloneApp::~StandaloneApp() {
    stop();
}

void StandaloneApp::prepare_render_state() {
    // The device's nominal buffer_size is NOT the largest block the audio
    // callback can deliver. When the hardware runs at a different sample rate
    // than the app, CoreAudio (and other backends) insert a resampler that
    // pulls the render callback in variable, larger-than-nominal blocks — up to
    // the host's MaximumFramesPerSlice (4096 by default on macOS). Size the
    // processor and every scratch buffer to this MAX, not the nominal buffer,
    // so an oversized pull can never trip Processor::process()'s
    // `num_samples <= max_block` assert or overflow a pre-allocated buffer.
    // (Diagnosed from a standalone SIGABRT in RealtimePitchTimeProcessor::process
    // when the output device's rate differed from the app's configured rate.)
    constexpr int kMaxCallbackBlockFloor = 4096;
    max_callback_block_ = std::max(config_.buffer_size, kMaxCallbackBlockFloor);

    // Prepare processor
    PrepareContext prep;
    prep.sample_rate = config_.sample_rate;
    prep.max_buffer_size = max_callback_block_;
    prep.input_channels = config_.input_channels;
    prep.output_channels = config_.output_channels;
    processor_->prepare(prep);

    while (hardware_midi_queue_.try_pop()) {}
    hardware_midi_queue_.reset_overflow_count();
    midi_in_.reserve(detail::kStandaloneMidiBufferEventCapacity);
    midi_out_.reserve(detail::kStandaloneMidiBufferEventCapacity);
    midi_in_.set_realtime_capacity_limit(true);
    midi_out_.set_realtime_capacity_limit(true);

    // Pre-allocate test signal buffer and pointer arrays (no audio-thread allocation)
    test_signal_.set_sample_rate(config_.sample_rate);
    int test_ch = std::max(config_.input_channels, config_.output_channels);
    if (test_ch < 2) test_ch = 2;
    test_buffer_.resize(static_cast<size_t>(test_ch), static_cast<size_t>(max_callback_block_));
    test_ptrs_.resize(static_cast<size_t>(test_ch));
    for (int c = 0; c < test_ch; ++c)
        test_ptrs_[static_cast<size_t>(c)] = test_buffer_.view().channel_ptr(static_cast<size_t>(c));
    meter_ptrs_.resize(static_cast<size_t>(std::max(test_ch, config_.input_channels)));
    direct_output_ptrs_.resize(static_cast<size_t>(std::max(config_.output_channels, 2)));
    output_meter_ptrs_.resize(static_cast<size_t>(std::max(config_.output_channels, 2)));

    // Pre-allocate silence buffer for when no input device is available
    int silence_ch = std::max(config_.output_channels, 2);
    silence_buffer_.resize(static_cast<size_t>(silence_ch), static_cast<size_t>(max_callback_block_));
    silence_ptrs_.resize(static_cast<size_t>(silence_ch));
    for (int c = 0; c < silence_ch; ++c)
        silence_ptrs_[static_cast<size_t>(c)] = silence_buffer_.view().channel_ptr(static_cast<size_t>(c));

#if PULP_ENABLE_AUDIO_PROBES
    // Prepare the realtime output-boundary probe BEFORE the audio callback
    // starts. This is the only place it allocates. Probe-enabled standalone
    // builds keep a small last-N multichannel ring so the developer Audio
    // Inspector can paint a live waveform (channel 0 drives the Signal trace)
    // whether it was opened from PULP_AUDIO_INSPECTOR at launch or toggled
    // later by command. The ring is sized to the panel's display capacity so
    // one UI tick fills the trace.
    audio::AudioProbe::CaptureConfig probe_capture;
    constexpr int kMaxScopeWindowSamples = 16384;
    const int scope_capture_frames = config_.audio_scope_json_path.empty()
        ? 0
        : std::clamp(config_.audio_scope_window_samples, 1, kMaxScopeWindowSamples);
    // capture-to-WAV shares the one probe ring; size it to whichever consumer
    // (inspector display / scope / capture-wav) needs the most. A 0 capture-wav
    // frame request means "as much as the ring holds" → the cap.
    if (!config_.audio_capture_wav_path.empty()
        && config_.audio_capture_wav_frames > detail::kMaxCaptureWindowSamples) {
        runtime::log_info(
            "Standalone: --audio-capture-frames {} exceeds the {}-sample cap; clamping",
            config_.audio_capture_wav_frames, detail::kMaxCaptureWindowSamples);
    }
    const int capture_wav_frames = config_.audio_capture_wav_path.empty()
        ? 0
        : (config_.audio_capture_wav_frames > 0
               ? std::clamp(config_.audio_capture_wav_frames, 1,
                            detail::kMaxCaptureWindowSamples)
               : detail::kMaxCaptureWindowSamples);
    probe_capture.capture_frames = std::max({view::AudioWaveformView::kCapacity,
                                             scope_capture_frames,
                                             capture_wav_frames});
    output_probe_.prepare(config_.output_channels, max_callback_block_,
                          config_.sample_rate,
                          audio::AudioProbeStage::kStandaloneOutputBoundary,
                          probe_capture);
    output_probe_ptrs_.assign(static_cast<size_t>(std::max(config_.output_channels, 0)),
                              nullptr);

    // Rolling last-N capture is independent of the probe FIFO above: its own
    // float-storage ring, fed from the same audio-thread output tap. Sized to the
    // requested window (0 = the shared capture cap). Gated so the audio thread
    // does zero extra work when --audio-capture-rolling is off.
    rolling_capture_active_ = !config_.audio_capture_rolling_path.empty();
    if (rolling_capture_active_) {
        const int rolling_frames = config_.audio_capture_rolling_frames > 0
            ? std::clamp(config_.audio_capture_rolling_frames, 1,
                         detail::kMaxCaptureWindowSamples)
            : detail::kMaxCaptureWindowSamples;
        if (config_.audio_capture_rolling_frames > detail::kMaxCaptureWindowSamples) {
            runtime::log_info(
                "Standalone: --audio-capture-rolling-frames {} exceeds the {}-sample cap; clamping",
                config_.audio_capture_rolling_frames, detail::kMaxCaptureWindowSamples);
        }
        audio::RollingAudioCaptureBufferConfig rolling_config;
        rolling_config.num_channels =
            static_cast<std::uint32_t>(std::max(config_.output_channels, 0));
        rolling_config.max_frames = static_cast<std::uint64_t>(rolling_frames);
        rolling_capture_active_ = rolling_capture_.prepare(rolling_config);
    }
#endif
}

void StandaloneApp::render_audio_block(
    const audio::BufferView<const float>& input,
    audio::BufferView<float>& output,
    const audio::CallbackContext& ctx) {
        // Hard guard: the processor and all scratch buffers were prepared for at
        // most `max_callback_block_` frames (see start()). A block beyond that —
        // which would indicate a backend not honoring MaximumFramesPerSlice —
        // must never reach process(), or it trips the `num_samples <= max_block`
        // assert / overflows the buffers. Emit silence and warn once rather than
        // crash the audio device on a real user's machine.
        if (ctx.buffer_size > max_callback_block_) {
            for (size_t c = 0; c < output.num_channels(); ++c) {
                float* dst = output.channel_ptr(c);
                std::fill(dst, dst + output.num_samples(), 0.0f);
            }
            static bool warned = false;
            if (!warned) {
                warned = true;
                runtime::log_error(
                    "Standalone: audio block {} exceeds prepared max {} — "
                    "emitting silence (report this device)",
                    ctx.buffer_size, max_callback_block_);
            }
            // Keep the transport clock monotonic across the dropped block.
            // The normal path advances by ctx.buffer_size after process();
            // skipping it here would lag transport position (and the MIDI
            // timeline derived from it) by exactly the silenced frames.
            if (config_.transport_playing) {
                transport_position_samples_.fetch_add(
                    ctx.buffer_size, std::memory_order_relaxed);
            }
            return;
        }

        // Collect hardware MIDI through the lock-free input queue. UI /
        // virtual-keyboard / scripting MIDI is delivered separately via
        // `ui_midi_collector_`, which is also lock-free and sample-accurate
        // within the current block.
        midi::MidiBuffer& midi_in = midi_in_;
        midi::MidiBuffer& midi_out = midi_out_;
        midi_in.clear();
        midi_in.clear_sysex();
        midi_out.clear();
        midi_out.clear_sysex();
        detail::drain_standalone_midi_input(hardware_midi_queue_, midi_in);

        // Drain UI-thread MIDI into this block at the correct sample offsets.
        // The standalone host treats its own audio clock as the master
        // timeline: block_start_seconds is
        // `transport_position_samples / sample_rate`.
        const int64_t block_start_samples =
            transport_position_samples_.load(std::memory_order_relaxed);
        const double block_start_seconds =
            ctx.sample_rate > 0.0
                ? static_cast<double>(block_start_samples) / ctx.sample_rate
                : 0.0;
        ui_midi_collector_.drain_into(midi_in, block_start_seconds,
                                      ctx.buffer_size, ctx.sample_rate);
        midi_in.sort();

#if PULP_ENABLE_AUDIO_PROBES
        auto analyze_output_probe = [&]() noexcept {
            const size_t out_ch = output.num_channels();
            for (size_t c = 0; c < out_ch && c < output_probe_ptrs_.size(); ++c)
                output_probe_ptrs_[c] = output.channel_ptr(c);
            const size_t probe_ch = std::min(out_ch, output_probe_ptrs_.size());
            const audio::BufferView<const float> out_view(
                output_probe_ptrs_.data(), probe_ch, output.num_samples());
            output_probe_.analyze_output(out_view);
            // Same tap feeds the rolling last-N capture (RT-safe append).
            if (rolling_capture_active_) {
                rolling_capture_.append(out_view, output.num_samples());
                rolling_capture_channels_.store(static_cast<int>(probe_ch),
                                                std::memory_order_relaxed);
            }
        };
#endif

        if (test_signal_.is_active() && config_.route_test_signal_to_output) {
            const size_t out_ch = std::min(output.num_channels(), direct_output_ptrs_.size());
            for (size_t c = 0; c < out_ch; ++c)
                direct_output_ptrs_[c] = output.channel_ptr(c);
            test_signal_.fill(direct_output_ptrs_.data(),
                              static_cast<int>(out_ch),
                              ctx.buffer_size);
            for (size_t c = 0; c < out_ch && c < output_meter_ptrs_.size(); ++c)
                output_meter_ptrs_[c] = output.channel_ptr(c);
            if (out_ch > 0) {
                output_meter_bridge_.analyze_and_push(
                    output_meter_ptrs_.data(),
                    static_cast<int>(out_ch),
                    ctx.buffer_size);
            }
#if PULP_ENABLE_AUDIO_PROBES
            analyze_output_probe();
#endif
            if (config_.transport_playing) {
                transport_position_samples_.fetch_add(
                    ctx.buffer_size, std::memory_order_relaxed);
            }
            return;
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

        // Populate the transport-related fields on ProcessContext from the
        // standalone's built-in tempo source.
        // The driver has no DAW providing transport, so it behaves like
        // one: tempo + time-signature are the user-chosen config values,
        // `position_beats` advances from the rolling sample clock at the
        // configured tempo, and `is_playing` mirrors the user's
        // play/stop toggle.
        ProcessContext proc_ctx;
        proc_ctx.sample_rate = ctx.sample_rate;
        proc_ctx.num_samples = ctx.buffer_size;
        proc_ctx.process_mode = ProcessMode::Realtime;
        proc_ctx.render_speed_hint = RenderSpeedHint::Realtime;
        proc_ctx.position_samples = block_start_samples;
        proc_ctx.is_playing = config_.transport_playing;
        proc_ctx.is_recording = false;
        proc_ctx.tempo_bpm = config_.tempo_bpm;
        proc_ctx.time_sig_numerator = config_.time_sig_numerator;
        proc_ctx.time_sig_denominator = config_.time_sig_denominator;
        if (config_.tempo_bpm > 0.0 && ctx.sample_rate > 0.0) {
            const double seconds_per_beat = 60.0 / config_.tempo_bpm;
            const double samples_per_beat = seconds_per_beat * ctx.sample_rate;
            if (samples_per_beat > 0.0) {
                proc_ctx.position_beats =
                    static_cast<double>(block_start_samples) / samples_per_beat;
            }
        }
        // Derive `bar`, then diff against the previous block for the transport
        // change flags. The standalone driver is a transport, so a processor
        // running here must see the same `tempo_changed` / `transport_changed` /
        // `transport_started` / `transport_jump` signals it would see under a
        // DAW — otherwise a tempo-synced generator behaves differently in the
        // standalone build than in the plugin builds, and the standalone is
        // exactly where those generators get developed. Shared with the VST3 /
        // AU / CLAP adapters so there is one definition of "the transport
        // started". Stateful; updates `playhead_prev_` in place.
        detail::derive_bar_from_beats(proc_ctx);
        detail::compute_playhead_changes(proc_ctx, playhead_prev_);

        {
            // Flush denormals to zero for the DSP callback so quiet tails in
            // recursive filter/reverb state can't stall the audio thread, then
            // restore the host's FP mode. See docs/guides/dsp-threading.md
            // "Numeric mode".
            pulp::signal::ScopedFlushDenormals flush_denormals;
            // Audio-thread render must neither allocate nor take a blocking
            // lock. ScopedNoAlloc scopes just the process() call (the meter
            // push below runs outside it) so the RT interposition guard traps a
            // regression in test builds.
            pulp::runtime::ScopedNoAlloc no_alloc_guard;
            processor_->process(output, *actual_input, midi_in, midi_out, proc_ctx);
        }

        const size_t out_ch = std::min(output.num_channels(), output_meter_ptrs_.size());
        for (size_t c = 0; c < out_ch; ++c)
            output_meter_ptrs_[c] = output.channel_ptr(c);
        if (out_ch > 0) {
            output_meter_bridge_.analyze_and_push(
                output_meter_ptrs_.data(),
                static_cast<int>(out_ch),
                ctx.buffer_size);
        }
#if PULP_ENABLE_AUDIO_PROBES
        // Tap the processor's output immediately after render and before
        // returning to the device callback. RT-safe: scalar-only, no
        // allocation, no FFT. This is the boundary where "UI works, no sound"
        // reports separate processor silence from output-boundary silence.
        // Fill the pre-allocated const pointer array (no audio-thread
        // allocation), then wrap it in a const view for analyze_output().
        analyze_output_probe();
#endif

        // Advance the rolling sample clock so the next block reads a
        // monotonic timeline. Done after process() so the in-block
        // transport state is consistent with what the plugin saw.
        if (config_.transport_playing) {
            transport_position_samples_.fetch_add(
                ctx.buffer_size, std::memory_order_relaxed);
        }
}

bool StandaloneApp::start() {
    // Create the processor once and reuse it across audio reconfigurations
    // (apply_config soft-restart). Recreating it on every settings change would
    // dangle an editor ViewBridge holding a Processor&; parameters are
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

    // No-op in the default OFF build; in a PULP_TRACING=ON dev build this logs a
    // single reminder that the tracing ring is compiled in and must not ship.
    runtime::log_tracing_reminder();

    // Restore the user's last-used audio/MIDI selection (default on; developer can opt out
    // via StandaloneConfig::persist_settings). Overlays persisted keys onto the configured
    // defaults, so the first launch (no saved file) keeps exactly what the app configured.
    if (config_.persist_settings && !persisted_config_loaded_) {
        config_ = load_persisted_config(desc.name, config_);
        persisted_config_loaded_ = true;  // don't re-overlay on soft restarts (apply_config)
    }

    const bool processor_has_audio_input = !desc.input_buses.empty();
    config_.supports_audio_input = config_.supports_audio_input && processor_has_audio_input;
    if (!config_.supports_audio_input) {
        config_.input_channels = 0;
        // An instrument has no input bus to inject the test tone into, so the
        // audio-settings test signal must go straight to the OUTPUT — otherwise it
        // feeds a non-existent input and the user sees the meter LED but hears
        // nothing. (Effects keep the default input-injection so the tone is
        // processed by the effect.)
        config_.route_test_signal_to_output = true;
    }
    constrain_audio_config(config_);

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

    // CoreAudio exposes its IO-thread workgroup only after open(). An embedded
    // parallel renderer (notably SequenceProcessor) receives it before prepare
    // starts auxiliary workers, without widening Processor's node ABI.
    if (auto* client = dynamic_cast<AudioWorkgroupClient*>(processor_.get())) {
        bind_audio_device_workgroup(*client, audio_device_.get());
    }

    // Only remember a CONCRETE device id when the user explicitly pinned one. For
    // the default-following case keep audio_device_id empty so the next launch (and
    // the live default-device listener) keep tracking the system default output —
    // overwriting it with the resolved id here would pin the app to whatever was
    // default at launch and it would only "follow" on relaunch.
    if (!config_.audio_device_id.empty())
        config_.audio_device_id = audio_device_->info().id;
    config_.sample_rate = audio_device_->sample_rate();
    config_.buffer_size = audio_device_->buffer_size();

    prepare_render_state();

    // Set up MIDI input (optional)
    if (desc.accepts_midi) {
        midi_system_ = midi::create_midi_system();
        auto inputs = midi_system_->enumerate_inputs();
        if (!inputs.empty()) {
            midi_input_ = midi_system_->create_input();
            auto midi_port_id = config_.midi_input_id.empty()
                ? inputs[0].id : config_.midi_input_id;
            midi_input_->open(midi_port_id, [this](const midi::MidiEvent& event) {
                (void)hardware_midi_queue_.try_push(event);
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
        render_audio_block(input, output, ctx);
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
    // dangle an editor ViewBridge holding a Processor&.
    if (was_running) stop_audio_keep_processor();
    config_ = new_config;
    constrain_audio_config(config_);
    bool ok = true;
    if (was_running) ok = start();
    // Remember the user's device/rate/buffer selection across launches (default on),
    // but only after a successful (re)start — otherwise a failed apply would persist
    // a broken selection that the next launch restores.
    if (ok && config_.persist_settings && processor_)
        save_persisted_config(processor_->descriptor().name, config_);
    return ok;
}

bool StandaloneApp::run_with_editor(bool use_gpu) {
    const auto effective_config = detail::standalone_config_from_environment(config_);
#if !PULP_ENABLE_AUDIO_PROBES
    if (detail::standalone_probe_json_requested_but_disabled(effective_config)) {
        runtime::log_error(
            "Standalone: audio probe/scope JSON requested but "
            "PULP_ENABLE_AUDIO_PROBES=OFF");
        return false;
    }
#endif
    if (detail::standalone_headless_requires_screenshot(effective_config)) {
        runtime::log_error(
            "Standalone: headless/CI mode requires a screenshot path; "
            "set StandaloneConfig::screenshot_path or PULP_SCREENSHOT");
        return false;
    }

    config_ = effective_config;
    if (!start()) return false;

    if (!processor_ || !processor_->has_editor()) {
        runtime::log_error("Standalone: processor has no editor");
        stop();
        return false;
    }

    // Opt into the platform's built-in file-dialog backend so editor file
    // pickers work natively. No-op on macOS (compiled-in impl); on Linux this
    // installs the xdg-desktop-portal bridge when libdbus is available.
    platform::FileDialog::install_native_backend();

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
        },
        processor_->settings_sections(),  // plugin-contributed Settings tabs (e.g. Models)
        &output_meter_bridge_);
    auto* settings_ptr = chrome.settings_panel();
    auto& window_root = chrome.window_root();

    // Build WindowOptions from the bridge's cached ViewSize hints so
    // min_width/min_height propagate to platform window hosts that honor them.
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
    if (auto* tab_panel = chrome.tab_panel()) {
        view::WindowHost* host = window.get();
        const float editor_w = static_cast<float>(size_hints.preferred_width);
        const float editor_h = static_cast<float>(size_hints.preferred_height);
        const float settings_h =
            std::max(editor_h, static_cast<float>(SettingsPanel::preferred_height()));
        tab_panel->on_tab_change = [host, tab_panel, editor_w, editor_h, settings_h](int index) {
            const bool settings = index == tab_panel->find_tab("Settings");
            const float w = editor_w;
            const float h = settings ? settings_h : editor_h;
            host->set_fixed_aspect_ratio(w / h);
            host->set_design_viewport(w, h);
            host->request_content_size(w, h);
        };
    }

    // Editor-INITIATED resize: let the editor ask the standalone window to
    // resize itself (e.g. a chrome-hiding mode wanting a smaller shape). Drives
    // the SAME design-viewport + content-size path the Settings tab uses above,
    // and updates the bridge's reported hints so the new aspect sticks. Cleared
    // in the close callback before the window / bridge it captures die.
    {
        view::WindowHost* resize_host = window.get();
        ViewBridge* resize_bridge = bridge.get();
        processor_->set_editor_resize_handler(
            [resize_host, resize_bridge](uint32_t w, uint32_t h) -> bool {
                if (w == 0 || h == 0) return false;
                if (resize_bridge) resize_bridge->set_preferred_size(w, h);
                const float fw = static_cast<float>(w);
                const float fh = static_cast<float>(h);
                resize_host->set_fixed_aspect_ratio(fw / fh);
                resize_host->set_design_viewport(fw, fh);
                resize_host->request_content_size(fw, fh);
                return true;
            });
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
        // Drop the editor→host resize handler before the window / bridge it
        // captures are torn down by stop().
        if (processor_) processor_->set_editor_resize_handler(nullptr);
        if (bridge_raw) bridge_raw->close();
        stop();
    });

#if PULP_STANDALONE_INSPECTOR
    // Create inspector overlay — activated via Cmd+I / Ctrl+I
    auto* inspector_host = &window_root;
    auto inspector = std::make_unique<inspect::InspectorOverlay>(*inspector_host);
    auto* inspector_ptr = inspector.get();
    inspect::install_inspector_hooks(*inspector);
#endif

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
        audio_inspector_.reset();
        command_registry_ = std::make_unique<view::CommandRegistry>();
        audio_inspector_ = std::make_unique<view::AudioInspectorWindow>(
            nullptr, window.get(), view::AudioInspectorWindow::HostFactory{},
            desc.name);
        audio_inspector_->set_probe(&output_probe_);
        audio_inspector_->register_command_handler(*command_registry_);
        view::route_global_keys(window_root, *command_registry_);
        audio_inspector_ptr = audio_inspector_.get();
    }
#endif

    // Wire inspector into the idle callback to push overlay paint each frame.
    // The inspector uses View::overlay_queue() for rendering and intercepts
    // key events through the root view's on_global_click callback for Cmd+I.
    //
    // Keep pre-screenshot idle work in a composable callback so the headless
    // screenshot path can extend it instead of clobbering it with a second
    // set_idle_callback.
    std::function<void()> pre_screenshot_idle;
#if PULP_STANDALONE_INSPECTOR
    if (scripted_ui_ptr) {
        pre_screenshot_idle = [scripted_ui_ptr, settings_ptr, inspector_ptr, inspector_host] {
            if (scripted_ui_ptr) scripted_ui_ptr->poll();
            if (settings_ptr) settings_ptr->poll();
            if (inspector_ptr->is_active()) {
                // Enqueue on the window root's own queue (S11): paint_overlays
                // drains the painting root's queue, and inspector_host IS that
                // root, so the inspector overlay paints exactly once.
                inspector_host->interaction().overlay_queue.push_back({
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
                inspector_host->interaction().overlay_queue.push_back({
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
    pre_screenshot_idle = detail::make_standalone_idle_callback(
        scripted_ui_ptr
            ? std::function<void()>{[scripted_ui_ptr] { scripted_ui_ptr->poll(); }}
            : std::function<void()>{},
        settings_ptr
            ? std::function<void()>{[settings_ptr] { settings_ptr->poll(); }}
            : std::function<void()>{});
    window->set_idle_callback(pre_screenshot_idle);
#endif

    // Live editor reload (1.9): when the hosted processor's logic hot-swaps
    // (ReloadableShell), rebuild the OPEN editor in place and request a repaint —
    // the same path the plugin editor pump uses (make_scripted_idle_pump), so the
    // standalone dev window shows the swapped UI live too. Composed over whatever
    // idle work the build wired above; cheap no-op for non-reloadable processors.
    {
        auto prev_idle = pre_screenshot_idle;
        pre_screenshot_idle = [prev_idle, bridge_raw] {
            if (prev_idle) prev_idle();
            if (bridge_raw && bridge_raw->poll_editor_reload()) {
                if (auto* v = bridge_raw->view()) v->request_repaint();
            }
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
            if (prior_idle) prior_idle();
            audio_inspector_ptr->poll();
        };
        window->set_idle_callback(pre_screenshot_idle);
    }

    // Open the Audio Inspector when PULP_AUDIO_INSPECTOR is set (mirrors the
    // PULP_INSPECTOR layout-inspector block above). The capture ring was sized
    // in start() under the same env, so the window paints a live waveform.
    if (audio_inspector_ptr && runtime::get_env("PULP_AUDIO_INSPECTOR")) {
        audio_inspector_ptr->show();
        runtime::log_info(
            "Standalone: audio inspector enabled via PULP_AUDIO_INSPECTOR env var");
    }
#endif

    if (!opts.initially_hidden)
        window->show();

    detail::log_standalone_window_open(w, h, use_gpu, bridge->uses_script_ui(), chrome);

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
            ? effective_config.screenshot_frame_delay : 30;
        cap.path = effective_config.screenshot_path;
        auto* editor_view = bridge->view();
#if PULP_ENABLE_AUDIO_PROBES
        // Sibling capture of the Audio Inspector's OWN window surface, written
        // next to the main screenshot as "<stem>.audio-inspector.png" when the
        // inspector is open. Lets a headless run prove the live panel loaded
        // (PULP_AUDIO_INSPECTOR=1 --screenshot X.png yields X.audio-inspector.png).
        const std::string inspector_png_path = [&] {
            return detail::audio_inspector_screenshot_path(
                effective_config.screenshot_path);
        }();
#endif
        cap.capture_fn = [host, editor_view, w, h
#if PULP_ENABLE_AUDIO_PROBES
                          , audio_inspector_ptr, inspector_png_path,
                          write_probe_json, write_scope_json, write_capture_wav,
                          write_capture_rolling,
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
                        runtime::log_info(
                            "Standalone: audio inspector screenshot written to {}",
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
            {effective_config.audio_probe_json_path, write_probe_json,
             "audio-probe-json"},
            {effective_config.audio_scope_json_path, write_scope_json,
             "audio-scope-json"},
            {effective_config.audio_capture_wav_path, write_capture_wav,
             "audio-capture-wav"},
            {effective_config.audio_capture_rolling_path, write_capture_rolling,
             "audio-capture-rolling"},
        }};
        for (const auto& dump : dumps) {
            if (dump.path.empty()) continue;
            auto* host = window.get();
            detail::DelayedAction cap;
            cap.delay = effective_config.screenshot_frame_delay > 0
                ? effective_config.screenshot_frame_delay : 30;
            cap.action_fn = [write_fn = dump.write_fn, path = dump.path] {
                write_fn(path);
            };
            cap.close_fn = [host] { host->request_close(); };
            auto prior = pre_screenshot_idle;
            host->set_idle_callback([prior, cap = std::move(cap)]() mutable {
                if (prior) prior();
                cap();
            });
            runtime::log_info(
                "Standalone: {} mode armed — will dump to {} after {} frames",
                dump.label, dump.path, cap.delay);
            break;
        }
    }
#endif

    // Blocks until the window is closed. Most window-close paths have
    // already fired bridge->close(), but application-quit paths can return
    // from the event loop without going through that callback. Close here
    // while the processor is still alive; the call is idempotent.
    window->run_event_loop();

    bridge->close();
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
        auto* client = processor_
            ? dynamic_cast<AudioWorkgroupClient*>(processor_.get())
            : nullptr;
        close_audio_device_after_workgroup_release(client, *audio_device_);
        audio_device_.reset();
    }
}

void StandaloneApp::stop() {
    stop_audio_keep_processor();
#if PULP_ENABLE_AUDIO_PROBES
    // Tear the Audio Inspector down before the processor / probe so its raw
    // `output_probe_` pointer never dangles. Destroying the window first also
    // removes its handler from `command_registry_` (RAII in its destructor);
    // destroy the registry after, so the removal has a live registry to target.
    audio_inspector_.reset();
    command_registry_.reset();
#endif
    if (processor_) {
        processor_->release();
        processor_.reset();
    }
}

// ── Persisted-config helpers ────────────────────────────────────────────────
//
// Keys live under the `standalone.*` namespace in the user properties file so
// they don't collide with plugin-owned state. The format is the simple
// scalar-per-key shape the existing PropertiesFile API supports — we don't
// need nested JSON for these handful of fields.

namespace {

constexpr std::string_view kKeyAudioDevice    = "standalone.audio_device_id";
constexpr std::string_view kKeyMidiInput      = "standalone.midi_input_id";
constexpr std::string_view kKeySampleRate     = "standalone.sample_rate";
constexpr std::string_view kKeyBufferSize     = "standalone.buffer_size";
constexpr std::string_view kKeyOutputChannels = "standalone.output_channels";
constexpr std::string_view kKeyInputChannels  = "standalone.input_channels";
constexpr std::string_view kKeyTempo          = "standalone.tempo_bpm";
constexpr std::string_view kKeyTimeSigNum     = "standalone.time_sig_num";
constexpr std::string_view kKeyTimeSigDen     = "standalone.time_sig_den";
constexpr std::string_view kKeyPlaying        = "standalone.transport_playing";

}  // namespace

StandaloneConfig StandaloneApp::load_persisted_config(std::string_view app_name,
                                                      StandaloneConfig cfg) {
    if (app_name.empty()) return cfg;

    state::ApplicationProperties props(app_name);
    props.load();
    const auto& user = props.user_settings();

    if (auto v = user.get_string(kKeyAudioDevice))    cfg.audio_device_id = *v;
    if (auto v = user.get_string(kKeyMidiInput))      cfg.midi_input_id   = *v;
    if (auto v = user.get_double(kKeySampleRate))     cfg.sample_rate     = *v;
    if (auto v = user.get_int(kKeyBufferSize))        cfg.buffer_size     = static_cast<int>(*v);
    if (auto v = user.get_int(kKeyOutputChannels))    cfg.output_channels = static_cast<int>(*v);
    if (auto v = user.get_int(kKeyInputChannels))     cfg.input_channels  = static_cast<int>(*v);
    if (auto v = user.get_double(kKeyTempo))          cfg.tempo_bpm       = *v;
    if (auto v = user.get_int(kKeyTimeSigNum))        cfg.time_sig_numerator   = static_cast<int>(*v);
    if (auto v = user.get_int(kKeyTimeSigDen))        cfg.time_sig_denominator = static_cast<int>(*v);
    if (auto v = user.get_bool(kKeyPlaying))          cfg.transport_playing    = *v;
    return cfg;
}

bool StandaloneApp::save_persisted_config(std::string_view app_name,
                                          const StandaloneConfig& config) {
    if (app_name.empty()) return false;

    state::ApplicationProperties props(app_name);
    props.load();  // start from the existing file so we preserve unrelated keys
    auto& user = props.user_settings();

    user.set_string(kKeyAudioDevice, config.audio_device_id);
    user.set_string(kKeyMidiInput, config.midi_input_id);
    user.set_double(kKeySampleRate, config.sample_rate);
    user.set_int(kKeyBufferSize, config.buffer_size);
    user.set_int(kKeyOutputChannels, config.output_channels);
    user.set_int(kKeyInputChannels, config.input_channels);
    user.set_double(kKeyTempo, config.tempo_bpm);
    user.set_int(kKeyTimeSigNum, config.time_sig_numerator);
    user.set_int(kKeyTimeSigDen, config.time_sig_denominator);
    user.set_bool(kKeyPlaying, config.transport_playing);

    // ApplicationProperties::save() walks both user + common files and
    // returns void; check the user file's persisted path is non-empty as
    // a proxy for "the platform let us pick a location to write to".
    props.save();
    return !user.path().empty();
}

} // namespace pulp::format
