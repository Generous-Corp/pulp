#include <pulp/format/processor_block_adapter.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>

#include <array>

namespace pulp::format {
namespace {

void clear_midi_buffer(midi::MidiBuffer& buffer) {
    buffer.clear();
    buffer.clear_sysex();
    buffer.attach_ump(nullptr);
}

BusBuffer* active_main_output(ProcessBlock& block) noexcept {
    if (!block.buses) return nullptr;
    for (auto& bus : block.buses->buses()) {
        if (bus.active && bus.direction == BusDirection::Output &&
            bus.role == BusRole::Main && !bus.empty()) {
            return &bus;
        }
    }
    return nullptr;
}

ProcessContext context_for_block(const ProcessBlock& block) noexcept {
    ProcessContext context;
    if (block.transport) context = *block.transport;
    if (context.sample_rate <= 0.0) context.sample_rate = block.sample_rate;
    if (context.num_samples <= 0) {
        context.num_samples = static_cast<int>(block.frame_count);
    }
    return context;
}

class ScopedProcessorSidecars {
public:
    explicit ScopedProcessorSidecars(Processor& processor) noexcept
        : processor_(processor),
          previous_sidechain_(processor.sidechain_input()),
          previous_mpe_(processor.mpe_input()),
          previous_ump_(processor.ump_input()),
          previous_params_(processor.param_events()) {}

    ScopedProcessorSidecars(const ScopedProcessorSidecars&) = delete;
    ScopedProcessorSidecars& operator=(const ScopedProcessorSidecars&) = delete;

    ~ScopedProcessorSidecars() {
        processor_.set_sidechain(previous_sidechain_);
        processor_.set_mpe_input(previous_mpe_);
        processor_.set_ump_input(previous_ump_);
        processor_.set_param_events(previous_params_);
    }

private:
    Processor& processor_;
    const audio::BufferView<const float>* previous_sidechain_ = nullptr;
    const midi::MpeBuffer* previous_mpe_ = nullptr;
    const midi::UmpBuffer* previous_ump_ = nullptr;
    const state::ParameterEventQueue* previous_params_ = nullptr;
};

} // namespace

bool process_processor_block(Processor& processor,
                             ProcessBlock& block,
                             ProcessorBlockAdapterScratch& scratch) noexcept {
    if (!block.validate()) return false;

    auto* output_bus = active_main_output(block);
    if (!output_bus) return false;

    auto* events = block.events;
    midi::MidiBuffer* midi_in = events && events->midi_in
        ? events->midi_in
        : &scratch.fallback_midi_in;
    midi::MidiBuffer* midi_out = events && events->midi_out
        ? events->midi_out
        : &scratch.fallback_midi_out;

    if (midi_in == &scratch.fallback_midi_in) clear_midi_buffer(*midi_in);
    if (midi_out == &scratch.fallback_midi_out) clear_midi_buffer(*midi_out);

    const state::ParameterEventQueue* params = nullptr;
    if (events) {
        if (events->parameter_events) {
            params = events->parameter_events;
        } else {
            scratch.empty_parameter_events.clear();
            params = &scratch.empty_parameter_events;
        }
    }

    const midi::MpeBuffer* mpe = events ? events->mpe_input : nullptr;
    const midi::UmpBuffer* ump = events ? events->ump_input : nullptr;
    const auto context = context_for_block(block);

    try {
        ScopedProcessorSidecars sidecars(processor);
        processor.set_mpe_input(mpe);
        processor.set_ump_input(ump);
        processor.set_param_events(params);
        runtime::ScopedNoAlloc no_alloc_guard;
        std::array<ProcessBusBufferView<const float>, BusBufferSet::kMaxBuses>
            input_views{};
        std::array<ProcessBusBufferView<float>, BusBufferSet::kMaxBuses>
            output_views{};
        std::size_t input_count = 0;
        std::size_t output_count = 0;
        for (const auto& bus : block.buses->buses()) {
            if (bus.direction == BusDirection::Input) {
                if (input_count >= input_views.size()) return false;
                const auto index = input_count++;
                input_views[index] = {
                    .info = {bus.name, index, BusDirection::Input, bus.role,
                             static_cast<int>(bus.input.num_channels()), false,
                             bus.active},
                    .buffer = bus.input,
                };
            } else {
                if (output_count >= output_views.size()) return false;
                const auto index = output_count++;
                output_views[index] = {
                    .info = {bus.name, index, BusDirection::Output, bus.role,
                             static_cast<int>(bus.output.num_channels()), false,
                             bus.active},
                    .buffer = bus.output,
                };
            }
        }
        ProcessBuffers rich{
            .inputs = ProcessBusBufferSet<const float>(
                std::span(input_views.data(), input_count)),
            .outputs = ProcessBusBufferSet<float>(
                std::span(output_views.data(), output_count)),
        };
        processor.process(rich, *midi_in, *midi_out, context);
    } catch (...) {
        return false;
    }
    return true;
}

} // namespace pulp::format
