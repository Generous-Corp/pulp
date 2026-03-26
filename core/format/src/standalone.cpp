#include "standalone.hpp"
#include <pulp/view/auto_ui.hpp>
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

        ProcessContext proc_ctx;
        proc_ctx.sample_rate = ctx.sample_rate;
        proc_ctx.num_samples = ctx.buffer_size;
        proc_ctx.position_samples = static_cast<int64_t>(ctx.sample_position);

        processor_->process(output, input, midi_in, midi_out, proc_ctx);
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

bool StandaloneApp::run_with_editor(bool use_gpu) {
    if (!start()) return false;

    if (!processor_ || !processor_->has_editor()) {
        runtime::log_error("Standalone: processor has no editor");
        stop();
        return false;
    }

    // Build AutoUi view tree from parameters
    auto root = view::AutoUi::build(store_);
    if (!root) {
        runtime::log_error("Standalone: AutoUi::build() failed");
        stop();
        return false;
    }

    auto [w, h] = processor_->editor_size();
    auto desc = processor_->descriptor();

    view::WindowOptions opts;
    opts.title = desc.name + " — Standalone";
    opts.width = static_cast<float>(w);
    opts.height = static_cast<float>(h);
    opts.resizable = true;
    opts.use_gpu = use_gpu;

    auto window = view::WindowHost::create(*root, opts);
    if (!window) {
        runtime::log_error("Standalone: WindowHost::create() failed");
        stop();
        return false;
    }

    window->set_close_callback([this]() { stop(); });
    window->show();

    runtime::log_info("Standalone: editor window open ({}x{}, gpu={})",
                      w, h, use_gpu);

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
