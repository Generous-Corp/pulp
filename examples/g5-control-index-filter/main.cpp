#include <pulp/format/processor.hpp>
#include <pulp/host/control_index_nodes.hpp>
#include <pulp/host/graph_serializer.hpp>
#include <pulp/host/signal_graph.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace {

class IndexedLowpass final : public pulp::format::Processor {
  public:
    static constexpr pulp::state::ParamID kCoefficient = 27;

    pulp::format::PluginDescriptor descriptor() const override {
        pulp::format::PluginDescriptor d;
        d.name = "G5 indexed lowpass";
        d.manufacturer = "Pulp";
        d.bundle_id = "com.pulp.example.g5-indexed-lowpass";
        d.version = "1";
        d.category = pulp::format::PluginCategory::Effect;
        d.input_buses = {{"In", 1, false}};
        d.output_buses = {{"Out", 1, false}};
        d.node_capabilities.consumes_audio_rate_modulations = true;
        return d;
    }

    void define_parameters(pulp::state::StateStore& store) override {
        pulp::state::ParamInfo p;
        p.id = kCoefficient;
        p.name = "Coefficient";
        p.range = {0.0f, 1.0f, 0.0f};
        p.rate = pulp::state::ParamRate::AudioRate;
        p.modulatable = true;
        store.add_parameter(p);
    }

    void prepare(const pulp::format::PrepareContext&) override {
        previous_ = 0.0f;
    }

    void process(pulp::audio::BufferView<float>& output,
                 const pulp::audio::BufferView<const float>&, pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&, const pulp::format::ProcessContext&) override {
        output.clear();
    }

    bool process_block(pulp::format::ProcessBlock& block) override {
        const auto* input = block.buses->first(pulp::format::BusDirection::Input);
        auto* output = block.buses->first(pulp::format::BusDirection::Output);
        if (!input || !output || input->input.num_channels() != 1 ||
            output->output.num_channels() != 1 || !block.events ||
            block.events->audio_rate_modulations.size() != 1)
            return false;
        const auto& modulation = block.events->audio_rate_modulations.front();
        if (modulation.param_id != kCoefficient || modulation.values.size() != block.frame_count)
            return false;
        for (std::uint32_t i = 0; i < block.frame_count; ++i) {
            previous_ += modulation.values[i] * (input->input.channel_ptr(0)[i] - previous_);
            output->output.channel_ptr(0)[i] = previous_;
        }
        return true;
    }

  private:
    float previous_ = 0.0f;
};

constexpr std::array<float, 4> kCoefficients{0.0f, 0.25f, 0.5f, 1.0f};
constexpr std::array<float, 16> kIndices{0, 1, 2, 3, 0, 2, 1, 3, 3, 1, 2, 0, 1, 3, 2, 0};

struct Render {
    bool built = false;
    bool rejected = false;
    std::array<float, 16> samples{};
};

Render render(std::span<const int> partitions, bool connect_modulation, bool swap_index) {
    using namespace pulp::host;
    SignalGraph graph;
    if (!graph.register_custom_node_type(make_control_indexed_table_type()))
        return {};
    const auto source = graph.add_input_node(1);
    const auto index = graph.add_control_index_input_node();
    const auto table = graph.add_custom_node("pulp.control.indexed-table");
    const auto filter = graph.add_processor_node(std::make_unique<IndexedLowpass>());
    const auto sink = graph.add_output_node(1);
    if (!source || !index || !table || !filter || !sink ||
        !graph.connect(graph.node(index)->out("index"), graph.node(table)->in("index")) ||
        !graph.connect(source, 0, filter, 0) || !graph.connect(filter, 0, sink, 0) ||
        !graph.set_custom_node_state(table, make_control_table_state(kCoefficients)))
        return {};
    if (connect_modulation &&
        !graph.connect_control_modulation(graph.node(table)->out("value"), filter,
                                          IndexedLowpass::kCoefficient, 0.0f, 1.0f))
        return {};
    if (!graph.prepare(48000.0, 16))
        return {};

    Render result;
    result.built = true;
    std::array<float, 16> input_data{};
    input_data.fill(1.0f);
    auto indices = kIndices;
    if (swap_index)
        indices[1] = 0.0f;
    std::size_t cursor = 0;
    for (const int size : partitions) {
        if (size <= 0 || cursor + static_cast<std::size_t>(size) > indices.size())
            return {};
        const auto frames = static_cast<std::size_t>(size);
        const float* input_ptr[] = {input_data.data() + cursor};
        float* output_ptr[] = {result.samples.data() + cursor};
        pulp::audio::BufferView<const float> input(input_ptr, 1, frames);
        pulp::audio::BufferView<float> output(output_ptr, 1, frames);
        const auto status = graph.process_control_checked(
            output, input, {1, cursor, std::span<const float>(indices.data() + cursor, frames)});
        if (status == ControlProcessResult::RenderFailed)
            result.rejected = true;
        else if (status != ControlProcessResult::Accepted)
            return {};
        cursor += frames;
    }
    if (cursor != indices.size())
        return {};
    return result;
}

bool matches_oracle(const std::array<float, 16>& samples) {
    float previous = 0.0f;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        previous += kCoefficients[static_cast<std::size_t>(kIndices[i])] * (1.0f - previous);
        if (std::abs(samples[i] - previous) > 1e-6f)
            return false;
    }
    return true;
}

std::string serialized_fixture(bool typed) {
    using namespace pulp::host;
    SignalGraph graph;
    if (typed) {
        if (!graph.register_custom_node_type(make_control_indexed_table_type()))
            return {};
        const auto index = graph.add_control_index_input_node();
        const auto table = graph.add_custom_node("pulp.control.indexed-table");
        if (!index || !table ||
            !graph.connect(graph.node(index)->out("index"), graph.node(table)->in("index")))
            return {};
    } else {
        const auto source = graph.add_input_node(1);
        const auto filter = graph.add_processor_node(std::make_unique<IndexedLowpass>());
        const auto sink = graph.add_output_node(1);
        if (!source || !filter || !sink || !graph.connect(source, 0, filter, 0) ||
            !graph.connect(filter, 0, sink, 0))
            return {};
    }
    return GraphSerializer::to_json(graph);
}

} // namespace

int main(int argc, char** argv) {
    const std::string_view mode = argc == 2 ? std::string_view(argv[1]) : "";
    if (argc > 2 ||
        (mode != "" && mode != "--planted-missing-modulation" &&
         mode != "--planted-swapped-index" && mode != "--emit-v4" && mode != "--emit-v5"))
        return 2;
    if (mode == "--emit-v4" || mode == "--emit-v5") {
        const auto json = serialized_fixture(mode == "--emit-v5");
        if (json.empty())
            return 1;
        std::cout << json << '\n';
        return 0;
    }
    constexpr std::array<int, 1> whole{16};
    constexpr std::array<int, 3> split{3, 5, 8};
    if (mode == "--planted-missing-modulation") {
        const auto missing = render(whole, false, false);
        if (missing.built && missing.rejected) {
            std::cout << "G5 planted defect detected: missing-modulation\n";
            return 0;
        }
        return 1;
    }
    if (mode == "--planted-swapped-index") {
        const auto swapped = render(whole, true, true);
        if (swapped.built && !swapped.rejected && !matches_oracle(swapped.samples)) {
            std::cout << "G5 planted defect detected: swapped-index\n";
            return 0;
        }
        return 1;
    }
    const auto one = render(whole, true, false);
    const auto partitioned = render(split, true, false);
    if (!one.built || !partitioned.built || one.rejected || partitioned.rejected ||
        one.samples != partitioned.samples || !matches_oracle(one.samples))
        return 1;
    std::cout << "G5 control-index filter verified\n";
    return 0;
}
