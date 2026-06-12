// Headless processing adapter implementation
// Simplest possible host — no threading, no UI, no device I/O

#include <pulp/format/headless.hpp>
#include <pulp/format/plugin_state_io.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>

#include <array>

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

ProcessBuffers make_process_buffers(
    audio::BufferView<float>& output,
    const audio::BufferView<const float>& input,
    std::array<ProcessBusBufferView<const float>, 1>& input_buses,
    std::array<ProcessBusBufferView<float>, 1>& output_buses) {
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
    return {
        .inputs = ProcessBusBufferSet<const float>(input_buses),
        .outputs = ProcessBusBufferSet<float>(output_buses),
    };
}

} // namespace

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
