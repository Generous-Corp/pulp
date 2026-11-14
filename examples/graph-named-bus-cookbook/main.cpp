#include <pulp/audio/buffer.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/host/baked_graph_processor.hpp>
#include <pulp/host/offline_signal_graph_host.hpp>
#include <pulp/host/signal_graph_runtime.hpp>

#include <array>
#include <cmath>
#include <iostream>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace {

using namespace pulp;
using namespace pulp::format;
using namespace pulp::host;

constexpr std::size_t kFrames = 32;
constexpr std::size_t kImpulseFrame = 8;

class NamedBusPlacement final : public Processor {
  public:
    // A routed graph node hands its Processor one input and one output bus both
    // named "main", whatever the descriptor declares. By default the processor
    // falls back to its main buses when a declared name is absent, which is what
    // lets it run unchanged in a graph. `names_only` removes the fallback so the
    // recipe can show what a name-only lookup does inside a graph (silence).
    explicit NamedBusPlacement(bool names_only = false) : names_only_(names_only) {}

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
        if (!names_only_) {
            if (!source)
                source = buses.inputs.main();
            if (!front)
                front = buses.outputs.main();
        }
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

  private:
    bool names_only_;
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

// `front_out`, when set, receives the buffer bound to output bus slot 0 so the
// graph path can be compared against the harness sample for sample.
bool run_layout(std::size_t index, bool swap_names, audio::Buffer<float>* front_out = nullptr) {
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
    if (front_out) {
        *front_out = audio::Buffer<float>(2, kFrames);
        for (std::size_t ch = 0; ch < 2; ++ch)
            for (std::size_t frame = 0; frame < kFrames; ++frame)
                front_out->channel(ch)[frame] = front.channel(ch)[frame];
    }
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

bool identical(const audio::Buffer<float>& a, const audio::Buffer<float>& b) {
    if (a.num_channels() != b.num_channels() || a.num_samples() != b.num_samples())
        return false;
    for (std::size_t ch = 0; ch < a.num_channels(); ++ch)
        for (std::size_t frame = 0; frame < a.num_samples(); ++frame)
            if (a.channel(ch)[frame] != b.channel(ch)[frame])
                return false;
    return true;
}

// The same processor as a SignalGraph node: Input(1) -> NamedBusPlacement ->
// Output(2). A Processor node exposes its MAIN output bus as graph ports (one
// per channel, named "audio", "out1"); the aux "Rear" bus has no graph port.
// `swap_ports` crosses the two main-bus channels on the way to the Output node.
struct GraphRun {
    bool ok = false;
    audio::Buffer<float> audio;
    NodeId processor = 0;
    bool bake_refused = false;
    LowerRejectReason bake_reason = LowerRejectReason::None;
    NodeId bake_offender = 0;
};

GraphRun run_graph(bool swap_ports, bool names_only = false) {
    GraphRun result;
    SignalGraph graph;
    const NodeId in = graph.add_input_node(1);
    const NodeId proc = graph.add_processor_node(std::make_unique<NamedBusPlacement>(names_only));
    const NodeId out = graph.add_output_node(2);
    if (!in || !proc || !out)
        return result;
    result.processor = proc;
    const auto* node = graph.node(proc);
    if (!node || node->input_ports.size() != 1 || node->output_ports.size() != 2 ||
        node->output_ports[0].name != "audio" || node->output_ports[1].name != "out1")
        return result;
    if (!graph.connect(in, 0, proc, 0) ||
        !graph.connect(proc, node->output_ports[0].index, out, swap_ports ? 1 : 0) ||
        !graph.connect(proc, node->output_ports[1].index, out, swap_ports ? 0 : 1))
        return result;

    OfflineSignalGraphHost host(graph);
    if (!host.prepare({.sample_rate = 48000.0,
                       .block_frames = 16,
                       .input_channels = 1,
                       .output_channels = 2}))
        return result;
    std::vector<float> stimulus(kFrames, 0.0f);
    stimulus[kImpulseFrame] = 1.0f;
    float* input_ptrs[] = {stimulus.data()};
    audio::BufferView<const float> input(input_ptrs, 1, stimulus.size());
    auto rendered = host.render({.frame_count = stimulus.size(), .input = input});
    if (!rendered.ok)
        return result;
    result.audio = std::move(rendered.audio);

    // Baked-artifact boundary: a Processor node is a hosted Plugin-type node, so
    // bake() must refuse it rather than silently dropping the bus layout.
    const auto baked = bake(graph);
    result.bake_refused = !baked.accepted && baked.processor == nullptr;
    result.bake_reason = baked.reason;
    result.bake_offender = baked.offending_node;
    result.ok = true;
    return result;
}

bool check_graph() {
    audio::Buffer<float> harness_front;
    if (!run_layout(1, false, &harness_front))
        return false;
    const auto graph = run_graph(false);
    if (!graph.ok || !identical(graph.audio, harness_front) || !matches(graph.audio, 0.75f, 0.25f))
        return false;
    // Name-only lookup finds neither "Source" nor "Front" on the graph's "main"
    // buses and renders silence: the graph does not forward descriptor names.
    const auto names_only = run_graph(false, true);
    if (!names_only.ok || !matches(names_only.audio, 0.0f, 0.0f))
        return false;
    return graph.bake_refused &&
           graph.bake_reason == LowerRejectReason::HostedPluginNotSelfContained &&
           graph.bake_offender == graph.processor;
}

// True when the graph path DETECTS both planted defects: the harness fed
// swapped bus names, and a graph with its main-bus channels crossed, must each
// differ from the correct graph output.
bool planted_graph_defects_detected() {
    const auto good = run_graph(false);
    audio::Buffer<float> swapped_names;
    // The swapped-name harness fails its own oracle by design; only its slot-0
    // audio is wanted here.
    run_layout(1, true, &swapped_names);
    if (!good.ok || swapped_names.num_channels() != 2)
        return false;
    const auto crossed = run_graph(true);
    return !identical(good.audio, swapped_names) && crossed.ok &&
           !identical(good.audio, crossed.audio) && matches(crossed.audio, 0.25f, 0.75f);
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--planted-swap") {
        // Exit 0 only when the oracle rejects the swapped bus names, and say so.
        if (run_layout(1, true)) {
            std::cerr << "planted swap was not detected\n";
            return 1;
        }
        std::cout << "named-bus planted defect detected: swapped-names\n";
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--planted-graph-swap") {
        if (!planted_graph_defects_detected()) {
            std::cerr << "planted graph swap was not detected\n";
            return 1;
        }
        std::cout << "named-bus graph planted defect detected: swapped-names\n";
        return 0;
    }
    NamedBusPlacement processor;
    if (!check_graph()) {
        std::cerr << "named-bus graph/harness parity or bake boundary failed\n";
        return 1;
    }
    if (!run_layout(0, false) || !run_layout(1, false) || run_layout(1, true) ||
        processor.is_bus_layout_supported({{1}, {0, 2}})) {
        std::cerr << "named-bus layout cookbook failed\n";
        return 1;
    }
    std::cout << "named-bus layouts, graph parity, bake refusal and planted controls passed\n";
    return 0;
}
