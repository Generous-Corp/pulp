#include <pulp/format/standalone.hpp>
#include <pulp/format/editor_ui.hpp>
#include <pulp/format/settings_panel.hpp>
#include <pulp/view/window_host.hpp>
#include <pulp/runtime/log.hpp>

namespace pulp::format {

StandaloneApp::StandaloneApp(ProcessorFactory factory)
    : factory_(factory)
{
}

StandaloneApp::~StandaloneApp() {
    stop();
}

bool StandaloneApp::start() {
    // Create processor
    processor_ = factory_();
    if (!processor_) {
        runtime::log_error("Standalone: failed to create processor");
        return false;
    }
    processor_->set_state_store(&store_);
    processor_->define_parameters(store_);

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
    if (was_running) stop();
    config_ = new_config;
    if (was_running) return start();
    return true;
}

bool StandaloneApp::run_with_editor(bool use_gpu) {
    if (!start()) return false;

    if (!processor_ || !processor_->has_editor()) {
        runtime::log_error("Standalone: processor has no editor");
        stop();
        return false;
    }

    std::string editor_error;
    auto editor_ui = build_editor_ui(store_, true, &editor_error);
    auto root = std::move(editor_ui.root);
    if (!root) {
        runtime::log_error("Standalone: failed to build editor UI ({})", editor_error);
        stop();
        return false;
    }

    auto [w, h] = processor_->editor_size();
    auto desc = processor_->descriptor();

    // Create settings panel
    auto settings_panel = std::make_unique<SettingsPanel>();
    auto* settings_ptr = settings_panel.get();
    settings_panel->bind_systems(audio_system_.get(), midi_system_.get());
    settings_panel->set_current_config(config_);
    settings_panel->set_input_meter_bridge(&input_meter_bridge_);
    settings_panel->set_callbacks({
        .on_config_apply = [this, settings_ptr](const StandaloneConfig& cfg) {
            if (apply_config(cfg)) {
                // Re-bind systems after restart (they may be recreated)
                settings_ptr->bind_systems(audio_system_.get(), midi_system_.get());
            }
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

    // Wrap editor and settings in a TabPanel
    auto tab_panel = std::make_unique<view::TabPanel>();
    tab_panel->flex().flex_grow = 1.0f;
    tab_panel->add_tab("Editor", std::move(root));
    tab_panel->add_tab("Settings", std::move(settings_panel));

    view::WindowOptions opts;
    opts.title = desc.name + " — Standalone";
    opts.width = static_cast<float>(w);
    opts.height = static_cast<float>(h) + 32.0f;  // Extra space for tab bar
    opts.resizable = true;
    opts.use_gpu = use_gpu;

    auto window = view::WindowHost::create(*tab_panel, opts);
    if (!window) {
        runtime::log_error("Standalone: WindowHost::create() failed");
        stop();
        return false;
    }

    if (editor_ui.scripted_ui) {
        editor_ui.scripted_ui->set_repaint_callback([&window] {
            if (window) window->repaint();
        });
        window->set_idle_callback([scripted_ui = editor_ui.scripted_ui.get(), settings_ptr] {
            if (scripted_ui) scripted_ui->poll();
            if (settings_ptr) settings_ptr->poll();
        });
    } else {
        // Even without scripted UI, poll settings for meter updates
        window->set_idle_callback([settings_ptr] {
            if (settings_ptr) settings_ptr->poll();
        });
    }

    window->set_close_callback([this]() { stop(); });
    window->show();

    runtime::log_info("Standalone: editor window open ({}x{}, gpu={}, mode={})",
                      w, h, use_gpu, editor_ui.uses_script_ui ? "scripted" : "autoui");

    // Blocks until the window is closed
    window->run_event_loop();

    stop();
    return true;
}

void StandaloneApp::stop() {
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
    if (processor_) {
        processor_->release();
        processor_.reset();
    }
}

} // namespace pulp::format
