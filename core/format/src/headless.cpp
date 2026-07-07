// Headless processing adapter implementation
// Simplest possible host — no threading, no UI, no device I/O

#include <pulp/format/headless.hpp>
#include <pulp/format/plugin_state_io.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>

#include <array>
#include <utility>
#include <vector>

namespace pulp::format {
namespace {

class ScopedProcessorParamEvents {
public:
    ScopedProcessorParamEvents(Processor& processor,
                               const state::ParameterEventQueue& events)
        : processor_(processor) {
        processor_.set_param_events(&events);
    }

    ~ScopedProcessorParamEvents() {
        processor_.set_param_events(nullptr);
    }

private:
    Processor& processor_;
};

} // namespace

namespace {

template <typename SampleType>
ProcessBuffersT<SampleType> make_process_buffers(
    audio::BufferView<SampleType>& output,
    const audio::BufferView<const SampleType>& input,
    std::array<ProcessBusBufferView<const SampleType>, 1>& input_buses,
    std::array<ProcessBusBufferView<SampleType>, 1>& output_buses) {
    input_buses = {{
        {
            .info = {"Main In", 0, BusDirection::Input, BusRole::Main,
                     static_cast<int>(input.num_channels()), false, !input.empty()},
            .buffer = input,
        },
    }};
    output_buses = {{
        {
            .info = {"Main Out", 0, BusDirection::Output, BusRole::Main,
                     static_cast<int>(output.num_channels()), false, !output.empty()},
            .buffer = output,
        },
    }};
    return ProcessBuffersT<SampleType>{
        .inputs = ProcessBusBufferSet<const SampleType>(input_buses),
        .outputs = ProcessBusBufferSet<SampleType>(output_buses),
    };
}

} // namespace

RenderSpeedHint render_speed_hint_for(double ratio) {
    if (ratio > 1.0) return RenderSpeedHint::FasterThanRealtime;
    if (ratio < 1.0) return RenderSpeedHint::SlowerThanRealtime;
    return RenderSpeedHint::Unknown;
}

HeadlessHost::HeadlessHost(ProcessorFactory factory) {
    if (!factory) return;
    processor_ = factory();
    if (processor_) {
        processor_->set_state_store(&store_);
        processor_->define_parameters(store_);
        descriptor_ = processor_->descriptor();
    }
}

void HeadlessHost::prepare(double sample_rate, int max_buffer_size,
                            int input_channels, int output_channels) {
    static_cast<void>(try_prepare(
        sample_rate, max_buffer_size, input_channels, output_channels, {}));
}

bool HeadlessHost::try_prepare(double sample_rate, int max_buffer_size,
                               int input_channels, int output_channels,
                               PrepareResourceLimits resource_limits) {
    last_prepare_limit_failure_ = PrepareResourceLimit::None;
    if (!processor_) return false;

    PrepareContext ctx;
    ctx.sample_rate = sample_rate;
    ctx.max_buffer_size = max_buffer_size;
    ctx.input_channels = input_channels;
    ctx.output_channels = output_channels;
    ctx.resource_limits = resource_limits;

    const auto failure = processor_->check_prepare_resource_limits(ctx);
    if (failure != PrepareResourceLimit::None) {
        last_prepare_limit_failure_ = failure;
        return false;
    }

    sample_rate_ = sample_rate;
    processor_->prepare(ctx);
    processor_->prepare_f64_fallback_scratch(ctx);
    return true;
}

void HeadlessHost::process(audio::BufferView<float>& output,
                            const audio::BufferView<const float>& input) {
    midi::MidiBuffer midi_in, midi_out;
    process(output, input, midi_in, midi_out);
}

void HeadlessHost::process(audio::BufferView<float>& output,
                            const audio::BufferView<const float>& input,
                            ProcessContext context) {
    midi::MidiBuffer midi_in, midi_out;
    process(output, input, midi_in, midi_out, std::move(context));
}

void HeadlessHost::process(audio::BufferView<float>& output,
                            const audio::BufferView<const float>& input,
                            midi::MidiBuffer& midi_in,
                            midi::MidiBuffer& midi_out) {
    ProcessContext ctx;
    ctx.process_mode = ProcessMode::Offline;
    ctx.render_speed_hint = RenderSpeedHint::FasterThanRealtime;
    process(output, input, midi_in, midi_out, std::move(ctx));
}

void HeadlessHost::process(audio::BufferView<float>& output,
                            const audio::BufferView<const float>& input,
                            const state::ParameterEventQueue& param_events) {
    midi::MidiBuffer midi_in, midi_out;
    ProcessContext ctx;
    ctx.process_mode = ProcessMode::Offline;
    ctx.render_speed_hint = RenderSpeedHint::FasterThanRealtime;
    process(output, input, midi_in, midi_out, param_events, std::move(ctx));
}

void HeadlessHost::process(audio::BufferView<float>& output,
                            const audio::BufferView<const float>& input,
                            midi::MidiBuffer& midi_in,
                            midi::MidiBuffer& midi_out,
                            ProcessContext context) {
    if (!processor_) return;

    if (context.sample_rate <= 0.0) context.sample_rate = sample_rate_;
    if (context.num_samples <= 0) {
        context.num_samples = static_cast<int>(output.num_samples());
    }
    processor_->set_param_events(nullptr);
    std::array<ProcessBusBufferView<const float>, 1> input_buses;
    std::array<ProcessBusBufferView<float>, 1> output_buses;
    auto process_buffers =
        make_process_buffers(output, input, input_buses, output_buses);
    pulp::runtime::ScopedNoAlloc no_alloc_guard;
    processor_->process(process_buffers, midi_in, midi_out, context);
}

void HeadlessHost::process(audio::BufferView<float>& output,
                            const audio::BufferView<const float>& input,
                            midi::MidiBuffer& midi_in,
                            midi::MidiBuffer& midi_out,
                            const state::ParameterEventQueue& param_events,
                            ProcessContext context) {
    if (!processor_) return;

    if (context.sample_rate <= 0.0) context.sample_rate = sample_rate_;
    if (context.num_samples <= 0) {
        context.num_samples = static_cast<int>(output.num_samples());
    }
    ScopedProcessorParamEvents scoped_param_events(*processor_, param_events);
    std::array<ProcessBusBufferView<const float>, 1> input_buses;
    std::array<ProcessBusBufferView<float>, 1> output_buses;
    auto process_buffers =
        make_process_buffers(output, input, input_buses, output_buses);
    pulp::runtime::ScopedNoAlloc no_alloc_guard;
    processor_->process(process_buffers, midi_in, midi_out, context);
}

void HeadlessHost::process_f64(audio::BufferView<double>& output,
                               const audio::BufferView<const double>& input) {
    midi::MidiBuffer midi_in, midi_out;
    process_f64(output, input, midi_in, midi_out);
}

void HeadlessHost::process_f64(audio::BufferView<double>& output,
                               const audio::BufferView<const double>& input,
                               ProcessContext context) {
    midi::MidiBuffer midi_in, midi_out;
    process_f64(output, input, midi_in, midi_out, std::move(context));
}

void HeadlessHost::process_f64(audio::BufferView<double>& output,
                               const audio::BufferView<const double>& input,
                               midi::MidiBuffer& midi_in,
                               midi::MidiBuffer& midi_out) {
    ProcessContext ctx;
    ctx.process_mode = ProcessMode::Offline;
    ctx.render_speed_hint = RenderSpeedHint::FasterThanRealtime;
    process_f64(output, input, midi_in, midi_out, std::move(ctx));
}

void HeadlessHost::process_f64(
    audio::BufferView<double>& output,
    const audio::BufferView<const double>& input,
    const state::ParameterEventQueue& param_events) {
    midi::MidiBuffer midi_in, midi_out;
    ProcessContext ctx;
    ctx.process_mode = ProcessMode::Offline;
    ctx.render_speed_hint = RenderSpeedHint::FasterThanRealtime;
    process_f64(output, input, midi_in, midi_out, param_events, std::move(ctx));
}

void HeadlessHost::process_f64(audio::BufferView<double>& output,
                               const audio::BufferView<const double>& input,
                               midi::MidiBuffer& midi_in,
                               midi::MidiBuffer& midi_out,
                               ProcessContext context) {
    if (!processor_) return;

    if (context.sample_rate <= 0.0) context.sample_rate = sample_rate_;
    if (context.num_samples <= 0) {
        context.num_samples = static_cast<int>(output.num_samples());
    }
    processor_->set_param_events(nullptr);
    std::array<ProcessBusBufferView<const double>, 1> input_buses;
    std::array<ProcessBusBufferView<double>, 1> output_buses;
    auto process_buffers =
        make_process_buffers(output, input, input_buses, output_buses);
    pulp::runtime::ScopedNoAlloc no_alloc_guard;
    processor_->process_f64(process_buffers, midi_in, midi_out, context);
}

void HeadlessHost::process_f64(
    audio::BufferView<double>& output,
    const audio::BufferView<const double>& input,
    midi::MidiBuffer& midi_in,
    midi::MidiBuffer& midi_out,
    const state::ParameterEventQueue& param_events,
    ProcessContext context) {
    if (!processor_) return;

    if (context.sample_rate <= 0.0) context.sample_rate = sample_rate_;
    if (context.num_samples <= 0) {
        context.num_samples = static_cast<int>(output.num_samples());
    }
    ScopedProcessorParamEvents scoped_param_events(*processor_, param_events);
    std::array<ProcessBusBufferView<const double>, 1> input_buses;
    std::array<ProcessBusBufferView<double>, 1> output_buses;
    auto process_buffers =
        make_process_buffers(output, input, input_buses, output_buses);
    pulp::runtime::ScopedNoAlloc no_alloc_guard;
    processor_->process_f64(process_buffers, midi_in, midi_out, context);
}

std::optional<audio::AudioFileData> HeadlessHost::render_offline(
    const audio::AudioFileData& input,
    const audio::OfflineRenderOptions& options) {
    if (!processor_) return std::nullopt;

    return audio::offline_render(
        input,
        [&](const float* in, float* out, int channels,
            const audio::OfflineRenderBlockContext& context) {
            audio::Buffer<float> input_block(
                static_cast<std::size_t>(channels),
                static_cast<std::size_t>(context.frames));
            audio::Buffer<float> output_block(
                static_cast<std::size_t>(channels),
                static_cast<std::size_t>(context.frames));

            for (int frame = 0; frame < context.frames; ++frame) {
                for (int channel = 0; channel < channels; ++channel) {
                    input_block.channel(static_cast<std::size_t>(channel))
                        [static_cast<std::size_t>(frame)] =
                            in[static_cast<std::size_t>(frame)
                               * static_cast<std::size_t>(channels)
                               + static_cast<std::size_t>(channel)];
                }
            }

            std::vector<const float*> input_ptrs(
                static_cast<std::size_t>(channels), nullptr);
            for (int channel = 0; channel < channels; ++channel) {
                input_ptrs[static_cast<std::size_t>(channel)] =
                    input_block.channel(static_cast<std::size_t>(channel)).data();
            }
            audio::BufferView<const float> input_view(
                input_ptrs.data(),
                static_cast<std::size_t>(channels),
                static_cast<std::size_t>(context.frames));
            auto output_view = output_block.view();

            midi::MidiBuffer midi_in, midi_out;
            ProcessContext process_context;
            process_context.sample_rate = context.sample_rate;
            process_context.num_samples = context.frames;
            process_context.process_mode = ProcessMode::Offline;
            process_context.render_speed_hint =
                render_speed_hint_for(context.render_speed_ratio);
            process_context.tempo_bpm = context.tempo_bpm;
            process_context.position_beats = context.position_beats;
            process_context.position_samples =
                static_cast<int64_t>(context.sample_position);

            process(output_view, input_view, midi_in, midi_out,
                    std::move(process_context));

            for (int frame = 0; frame < context.frames; ++frame) {
                for (int channel = 0; channel < channels; ++channel) {
                    out[static_cast<std::size_t>(frame)
                        * static_cast<std::size_t>(channels)
                        + static_cast<std::size_t>(channel)] =
                        output_block.channel(static_cast<std::size_t>(channel))
                            [static_cast<std::size_t>(frame)];
                }
            }
        },
        options);
}

void HeadlessHost::release() {
    if (processor_) processor_->release();
}

std::vector<uint8_t> HeadlessHost::save_state() const {
    if (!processor_) return {};
    return plugin_state_io::serialize(store_, *processor_);
}

bool HeadlessHost::load_state(std::span<const uint8_t> data) {
    if (!processor_) return false;
    return plugin_state_io::deserialize(data, store_, *processor_);
}

} // namespace pulp::format
