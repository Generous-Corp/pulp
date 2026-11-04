#include <pulp/audio/buffer.hpp>
#include <pulp/format/processor.hpp>

#include <array>
#include <cmath>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

namespace {

using namespace pulp;
using namespace pulp::format;

constexpr std::size_t kFrames = 32;
constexpr std::size_t kImpulseFrame = 8;

class NamedBusPlacement final : public Processor {
  public:
    PluginDescriptor descriptor() const override {
        PluginDescriptor result;
        result.name = "Named Bus Placement Cookbook";
        result.manufacturer = "Pulp";
        result.bundle_id = "com.pulp.graph-named-bus-cookbook";
        result.version = "1.0.0";
        result.category = PluginCategory::Effect;
        result.input_buses = {{"Source", 1}};
        result.output_buses = {{"Front", 2}, {"Rear", 2, true, BusRole::Aux}};
        result.supported_bus_layouts = {{{1}, {2, 0}, "Front only"},
                                        {{1}, {2, 2}, "Front and Rear"}};
        return result;
    }

    void define_parameters(state::StateStore&) override {}
    void prepare(const PrepareContext&) override {}

    void process(audio::BufferView<float>& output, const audio::BufferView<const float>& input,
                 midi::MidiBuffer&, midi::MidiBuffer&, const ProcessContext&) override {
        for (std::size_t frame = 0; frame < output.num_samples(); ++frame) {
            const float sample = input.num_channels() ? input.channel(0)[frame] : 0.0f;
            output.channel(0)[frame] = 0.75f * sample;
            if (output.num_channels() > 1)
                output.channel(1)[frame] = 0.25f * sample;
        }
    }

    void process(ProcessBuffers& buses, midi::MidiBuffer&, midi::MidiBuffer&,
                 const ProcessContext&) override {
        const auto* source = buses.inputs.find_by_name("Source");
        auto* front = buses.outputs.find_by_name("Front");
        auto* rear = buses.outputs.find_by_name("Rear");
        if (!source || !front || !source->active() || !front->active() ||
            !buses.layouts_match_descriptors() || !buses.active_buses_have_storage())
            return;

        for (std::size_t frame = 0; frame < front->buffer.num_samples(); ++frame) {
            const float sample = source->buffer.channel(0)[frame];
            front->buffer.channel(0)[frame] = 0.75f * sample;
            front->buffer.channel(1)[frame] = 0.25f * sample;
            if (rear && rear->active()) {
                rear->buffer.channel(0)[frame] = 0.25f * sample;
                rear->buffer.channel(1)[frame] = 0.75f * sample;
            }
        }
    }
};

bool matches(const audio::Buffer<float>& buffer, float left, float right) {
    if (buffer.num_channels() != 2 || buffer.num_samples() != kFrames)
        return false;
    for (std::size_t frame = 0; frame < kFrames; ++frame) {
        const float impulse = frame == kImpulseFrame ? 1.0f : 0.0f;
        if (std::abs(buffer.channel(0)[frame] - left * impulse) > 1.0e-6f ||
            std::abs(buffer.channel(1)[frame] - right * impulse) > 1.0e-6f)
            return false;
    }
    return true;
}

bool run_layout(std::size_t index, bool swap_names) {
    NamedBusPlacement processor;
    const auto descriptor = processor.descriptor();
    if (index >= descriptor.supported_bus_layouts.size())
        return false;
    const auto& layout = descriptor.supported_bus_layouts[index];
    if (!processor.is_bus_layout_supported({layout.inputs, layout.outputs}) ||
        layout.inputs != std::vector<int>{1} || layout.outputs.size() != 2)
        return false;

    PrepareContext prepare;
    prepare.sample_rate = 48000.0;
    prepare.max_buffer_size = static_cast<int>(kFrames);
    prepare.input_channels = layout.inputs.front();
    prepare.output_channels = layout.outputs.front();
    processor.prepare(prepare);

    audio::Buffer<float> input(1, kFrames);
    audio::Buffer<float> front(2, kFrames);
    audio::Buffer<float> rear(2, kFrames);
    input.clear();
    front.clear();
    rear.clear();
    input.channel(0)[kImpulseFrame] = 1.0f;
    const auto input_view = input.view();
    const float* source_channels[] = {input_view.channel_ptr(0)};
    audio::BufferView<const float> source(source_channels, 1, kFrames);

    std::array<ProcessBusBufferView<const float>, 1> input_buses{{
        {.info = {"Source", 0, BusDirection::Input, BusRole::Main, 1, false, true},
         .buffer = source},
    }};
    std::array<ProcessBusBufferView<float>, 2> output_buses{{
        {.info = {swap_names ? "Rear" : "Front", 0, BusDirection::Output, BusRole::Main, 2, false,
                  true},
         .buffer = front.view()},
        {.info = {swap_names ? "Front" : "Rear", 1, BusDirection::Output, BusRole::Aux,
                  layout.outputs[1], true, layout.outputs[1] != 0},
         .buffer = layout.outputs[1] ? rear.view() : audio::BufferView<float>{}},
    }};
    ProcessBuffers buses{.inputs = ProcessBusBufferSet<const float>(std::span(input_buses)),
                         .outputs = ProcessBusBufferSet<float>(std::span(output_buses))};
    if (!buses.layouts_match_descriptors() || !buses.active_buses_have_storage())
        return false;

    midi::MidiBuffer midi_in, midi_out;
    ProcessContext context;
    context.sample_rate = 48000.0;
    context.num_samples = static_cast<int>(kFrames);
    processor.process(buses, midi_in, midi_out, context);
    const bool oracle = matches(front, 0.75f, 0.25f) &&
                        matches(rear, index == 0 ? 0.0f : 0.25f, index == 0 ? 0.0f : 0.75f);
    if (swap_names)
        return oracle;
    const auto& input_info = input_buses[0].info;
    if (input_info.name != descriptor.input_buses[0].name || input_info.index != 0 ||
        input_info.direction != BusDirection::Input ||
        input_info.role != descriptor.input_buses[0].role ||
        input_info.declared_channels != layout.inputs[0] || !input_info.active)
        return false;
    for (std::size_t bus = 0; bus < output_buses.size(); ++bus) {
        const auto& info = output_buses[bus].info;
        const auto& declared = descriptor.output_buses[bus];
        if (info.name != declared.name || info.index != bus ||
            info.direction != BusDirection::Output || info.role != declared.role ||
            info.declared_channels != layout.outputs[bus] || info.optional != declared.optional ||
            info.active != (layout.outputs[bus] != 0))
            return false;
    }
    return oracle;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--planted-swap")
        return run_layout(1, true) ? 0 : 1;
    NamedBusPlacement processor;
    if (!run_layout(0, false) || !run_layout(1, false) || run_layout(1, true) ||
        processor.is_bus_layout_supported({{1}, {0, 2}})) {
        std::cerr << "named-bus layout cookbook failed\n";
        return 1;
    }
    std::cout << "named-bus layouts and planted controls passed\n";
    return 0;
}
