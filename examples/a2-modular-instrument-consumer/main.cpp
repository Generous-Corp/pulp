#include <pulp/format/processor.hpp>
#include <pulp/host/control_index_nodes.hpp>
#include <pulp/host/graph_serializer.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/host/subgraph_persistence.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace pulp;
using namespace pulp::host;

constexpr state::ParamID kCoefficient = 27;
const ProcessorTypeKey kFilterType{"com.pulp.example.a2-strip-filter", 1};

// One-pole lowpass whose coefficient arrives as a dense Control modulation lane.
class StripFilter final : public format::Processor {
  public:
    format::PluginDescriptor descriptor() const override {
        format::PluginDescriptor d;
        d.name = "A2 strip filter";
        d.manufacturer = "Pulp";
        d.bundle_id = "com.pulp.example.a2-strip-filter";
        d.version = "1";
        d.category = format::PluginCategory::Effect;
        d.input_buses = {{"In", 1, false}};
        d.output_buses = {{"Out", 1, false}};
        d.node_capabilities.consumes_audio_rate_modulations = true;
        return d;
    }
    void define_parameters(state::StateStore& store) override {
        state::ParamInfo p;
        p.id = kCoefficient;
        p.name = "Coefficient";
        p.range = {0.0f, 1.0f, 0.0f};
        p.rate = state::ParamRate::AudioRate;
        p.modulatable = true;
        store.add_parameter(p);
    }
    void prepare(const format::PrepareContext&) override {
        previous_ = 0.0f;
    }
    void process(audio::BufferView<float>& out, const audio::BufferView<const float>&,
                 midi::MidiBuffer&, midi::MidiBuffer&, const format::ProcessContext&) override {
        out.clear();
    }
    bool process_block(format::ProcessBlock& block) override {
        const auto* in = block.buses->first(format::BusDirection::Input);
        auto* out = block.buses->first(format::BusDirection::Output);
        if (!in || !out || !block.events || block.events->audio_rate_modulations.size() != 1)
            return false;
        const auto& lane = block.events->audio_rate_modulations.front();
        if (lane.param_id != kCoefficient || lane.values.size() != block.frame_count)
            return false;
        for (std::uint32_t i = 0; i < block.frame_count; ++i) {
            previous_ += lane.values[i] * (in->input.channel_ptr(0)[i] - previous_);
            out->output.channel_ptr(0)[i] = previous_;
        }
        return true;
    }

  private:
    float previous_ = 0.0f;
};

// A stateful Custom member: scales its input by a trim kept in 4 bytes of
// opaque state. load_state accepts a finite trim in [0, 4] only.
constexpr std::string_view kTrimType = "com.pulp.example.a2.trim";

std::vector<std::uint8_t> trim_state(float value) {
    std::vector<std::uint8_t> bytes(sizeof(float));
    std::memcpy(bytes.data(), &value, sizeof(float));
    return bytes;
}

CustomNodeType trim_type(int version) {
    CustomNodeType type;
    type.type_id = std::string(kTrimType);
    type.version = version;
    type.num_input_ports = 1;
    type.num_output_ports = 1;
    type.create = [] { return static_cast<void*>(new float(1.0f)); };
    type.destroy = [](void* instance) { delete static_cast<float*>(instance); };
    type.process_instance = [](void* instance, audio::BufferView<float>& out,
                               const audio::BufferView<const float>& in, int frames) {
        const float trim = *static_cast<float*>(instance);
        for (int i = 0; i < frames; ++i)
            out.channel_ptr(0)[i] = in.channel_ptr(0)[i] * trim;
    };
    type.save_state = [](void* instance) { return trim_state(*static_cast<float*>(instance)); };
    type.load_state = [](void* instance, const std::vector<std::uint8_t>& bytes) {
        float value = 0.0f;
        if (bytes.size() != sizeof(float))
            return false;
        std::memcpy(&value, bytes.data(), sizeof(float));
        if (!std::isfinite(value) || value < 0.0f || value > 4.0f)
            return false;
        *static_cast<float*>(instance) = value;
        return true;
    };
    return type;
}

// The declaration: filter -> level -> trim, with one persisted parameter and
// one Custom member whose opaque state persists with the document.
SubgraphTemplate strip_template() {
    SubgraphTemplate t;
    t.identity = "com.pulp.example.a2.strip";
    SubgraphNodeSpec filter;
    filter.key = "filter";
    filter.type = NodeType::Plugin;
    filter.processor_type_id = kFilterType.type_id;
    filter.processor_type_version = kFilterType.type_version;
    filter.processor_descriptor_version = "1";
    filter.processor_factory = [](std::uint32_t) { return std::make_unique<StripFilter>(); };
    SubgraphNodeSpec trim;
    trim.key = "trim";
    trim.type = NodeType::Custom;
    trim.custom_type_id = std::string(kTrimType);
    trim.custom_type_version = 1;
    trim.custom_state = trim_state(1.0f);
    t.nodes = {filter, {"level", NodeType::Gain, 1, 1.0f}, trim};
    t.edges = {{"filter", 0, "level", 0}, {"level", 0, "trim", 0}};
    t.exposed_ports = {{"input", "filter", 0, false, PortKind::Audio},
                       {"output", "trim", 0, true, PortKind::Audio}};
    t.parameters = {{"level", "level", 1.0f, 0.0f, 1.0f}};
    return t;
}

constexpr std::array<float, 4> kCoefficients{0.0f, 0.25f, 0.5f, 1.0f};
constexpr std::array<float, 16> kIndices{0, 1, 2, 3, 0, 2, 1, 3, 3, 1, 2, 0, 1, 3, 2, 0};
constexpr float kLeadLevel = 0.5f;
constexpr float kEchoLevel = 0.75f;
constexpr float kEchoTrim = 0.5f;
using Samples = std::array<float, 16>;

// Two instances of one declaration in series, both driven by one Control lane.
std::string author() {
    SignalGraph g;
    if (!g.register_custom_node_type(make_control_indexed_table_type()) ||
        !g.register_custom_node_type(trim_type(1)))
        return {};
    const auto t = strip_template();
    auto lead = instantiate_subgraph(g, t, "lead", 0);
    auto echo = instantiate_subgraph(g, t, "echo", 1);
    const auto in = g.add_input_node(1), out = g.add_output_node(1);
    const auto index = g.add_control_index_input_node();
    const auto table = g.add_custom_node("pulp.control.indexed-table");
    if (!lead || !echo || !in || !out || !index || !table ||
        !g.set_custom_node_state(table, make_control_table_state(kCoefficients)) ||
        !g.connect(g.node(index)->out("index"), g.node(table)->in("index")) ||
        !g.connect(in, 0, lead.instance.ports.at("input").node, 0) ||
        !connect_subgraph_ports(g, lead.instance, "output", echo.instance, "input") ||
        !g.connect(echo.instance.ports.at("output").node, 0, out, 0) ||
        !g.set_custom_node_state(echo.instance.nodes.back(), trim_state(kEchoTrim)))
        return {};
    for (auto* strip : {&lead.instance, &echo.instance}) {
        auto state = strip->save_state();
        state.parameters["level"] = strip == &lead.instance ? kLeadLevel : kEchoLevel;
        if (!restore_subgraph_state(g, *strip, state) ||
            !g.connect_control_modulation(g.node(table)->out("value"), strip->nodes.front(),
                                          kCoefficient, 0.0f, 1.0f))
            return {};
    }
    return to_saved_subgraph_document(g, std::array{lead.instance, echo.instance});
}

// Which Custom trim versions the restoring application registers.
enum class TrimTypes { Current, None, CurrentAndNext };

SubgraphDocumentLoad restore(SignalGraph& g, const std::string& json, bool with_template,
                             TrimTypes trims = TrimTypes::Current) {
    static ProcessorRestoreRegistry registry;
    static const bool registered =
        registry.register_type(
            {kFilterType, 1, 1, 1, StripFilter{}.descriptor().node_capabilities, [] {
                 return format::ProcessorNodeInstance::create(std::make_unique<StripFilter>());
             }}) == ProcessorRestoreCode::Restored;
    (void)g.register_custom_node_type(make_control_indexed_table_type());
    if (trims != TrimTypes::None)
        (void)g.register_custom_node_type(trim_type(1));
    if (trims == TrimTypes::CurrentAndNext)
        (void)g.register_custom_node_type(trim_type(2));
    GraphLoadOptions options;
    options.processor_registry = registered ? &registry : nullptr;
    options.processor_allowlist = {kFilterType};
    const std::array templates{strip_template()};
    return from_saved_subgraph_document(
        g, json, with_template ? std::span(templates) : std::span<const SubgraphTemplate>{},
        options);
}

// Render a restored document; `skew` shifts one block's absolute start.
std::optional<Samples> render(const std::string& json, std::span<const int> blocks,
                              ControlProcessResult* refused = nullptr, std::uint64_t skew = 0) {
    SignalGraph g;
    if (!restore(g, json, true).ok() || !g.prepare(48000.0, 16))
        return std::nullopt;
    Samples ones{}, result{};
    ones.fill(1.0f);
    std::size_t cursor = 0;
    for (const int size : blocks) {
        const auto n = static_cast<std::size_t>(size);
        const float* ip[] = {ones.data() + cursor};
        float* op[] = {result.data() + cursor};
        audio::BufferView<const float> input(ip, 1, n);
        audio::BufferView<float> output(op, 1, n);
        const auto start = cursor + (cursor == 0 ? 0 : skew);
        const auto status = g.process_control_checked(
            output, input, {1, start, std::span<const float>(kIndices.data() + cursor, n)});
        if (status != ControlProcessResult::Accepted) {
            if (refused)
                *refused = status;
            return std::nullopt;
        }
        cursor += n;
    }
    return result;
}

bool matches_oracle(const Samples& samples) {
    float lead = 0.0f, echo = 0.0f;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const float c = kCoefficients[static_cast<std::size_t>(kIndices[i])];
        lead += c * (1.0f - lead);
        echo += c * (kLeadLevel * lead - echo);
        if (std::abs(samples[i] - kEchoLevel * kEchoTrim * echo) > 1e-6f)
            return false;
    }
    return true;
}

std::string with_edited_level(std::string json, std::string_view to) {
    const std::string_view from = "\"level\": 0.75";
    const auto at = json.find(from);
    return at == std::string::npos ? std::string{} : json.replace(at, from.size(), to);
}

// Replace the first occurrence of `from` at or after `after`.
std::string edited_after(std::string json, std::string_view after, std::string_view from,
                         std::string_view to) {
    const auto anchor = json.find(after);
    const auto at = anchor == std::string::npos ? anchor : json.find(from, anchor);
    return at == std::string::npos ? std::string{} : json.replace(at, from.size(), to);
}

// A refused Custom restore leaves the application's graph as it was.
bool refused_untouched(const std::string& edited, TrimTypes trims, std::string_view rule) {
    SignalGraph g;
    const auto existing = g.add_gain_node("existing");
    const auto load = restore(g, edited, true, trims);
    return !edited.empty() && existing != 0 && !load.ok() && load.subgraphs.empty() &&
           g.nodes().size() == 1 && g.nodes().front().id == existing &&
           load.load.error.starts_with(rule);
}

int detected(std::string_view defect, bool caught) {
    if (caught)
        std::cout << "A2 planted defect detected: " << defect << '\n';
    return caught ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    const std::string_view mode = argc == 2 ? std::string_view(argv[1]) : "";
    const auto json = author();
    constexpr std::array whole{16};
    constexpr std::array split{3, 5, 8};
    SignalGraph g;
    if (json.empty() || argc > 2)
        return 2;
    if (mode == "--planted-unknown-template") {
        const auto load = restore(g, json, false);
        return detected("unknown-template",
                        !load.ok() && g.nodes().empty() &&
                            load.load.error.starts_with("subgraph_template_unknown"));
    }
    if (mode == "--planted-out-of-range-level") {
        const auto edited = with_edited_level(json, "\"level\": 7.5");
        const auto load = restore(g, edited, true);
        return detected("out-of-range-level",
                        !edited.empty() && !load.ok() && g.nodes().empty() &&
                            load.load.error.starts_with("subgraph_state_rejected"));
    }
    if (mode == "--planted-edited-level") {
        const auto edited = render(with_edited_level(json, "\"level\": 0.25"), whole);
        return detected("edited-level", edited && !matches_oracle(*edited));
    }
    if (mode == "--planted-skewed-clock") {
        auto refused = ControlProcessResult::Accepted;
        return detected("skewed-clock", !render(json, split, &refused, 1) &&
                                            refused == ControlProcessResult::WrongStart);
    }
    // The echo trim's state is 0.5f; 9.0f is outside what the type accepts.
    if (mode == "--planted-missing-custom-type")
        return detected("missing-custom-type",
                        refused_untouched(json, TrimTypes::None, "subgraph_instance_mismatch"));
    if (mode == "--planted-custom-version")
        return detected("custom-version",
                        refused_untouched(edited_after(json, kTrimType, "\"version\": 1",
                                                       "\"version\": 2"),
                                          TrimTypes::CurrentAndNext, "subgraph_instance_mismatch"));
    if (mode == "--planted-custom-state")
        return detected("custom-state",
                        refused_untouched(edited_after(json, "\"nodes\"", "AAAAPw==", "AAAQQQ=="),
                                          TrimTypes::Current, "subgraph_state_rejected"));
    if (!mode.empty())
        return 2;
    const auto load = restore(g, json, true);
    const auto one = render(json, whole), parts = render(json, split);
    if (!load.ok() || load.subgraphs.size() != 2 || load.subgraphs[0].instance_identity != "echo" ||
        load.subgraphs[0].instance_index != 1 ||
        load.subgraphs[0].parameters.at("level") != kEchoLevel ||
        load.subgraphs[1].parameters.at("level") != kLeadLevel)
        return 1;
    // The Custom member keeps its exact type, version and opaque state.
    for (const auto& strip : load.subgraphs) {
        const auto* trim = g.node(strip.nodes.back());
        const float want = strip.instance_identity == "echo" ? kEchoTrim : 1.0f;
        if (!trim || trim->type != NodeType::Custom || trim->custom_type_id != kTrimType ||
            trim->custom_type_version != 1 || g.custom_node_state(trim->id) != trim_state(want))
            return 1;
    }
    const auto a = GraphSerializer::inspect_document(json);
    const auto b = GraphSerializer::inspect_document(to_saved_subgraph_document(g, load.subgraphs));
    if (!a.ok() || !b.ok() || a.document.semantic_digest() != b.document.semantic_digest() ||
        !one || !parts || *one != *parts || !matches_oracle(*one))
        return 1;
    std::cout << "A2 modular instrument verified digest=" << a.document.semantic_digest() << '\n';
    return 0;
}
