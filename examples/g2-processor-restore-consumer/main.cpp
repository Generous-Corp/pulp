#include <pulp/host/graph_serializer.hpp>
#include <pulp/host/processor_restore.hpp>
#include <pulp/host/signal_graph.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unordered_map>

using namespace pulp;
using namespace pulp::host;
using Code = ProcessorRestoreCode;

namespace {
int checks = 0;
int constructions = 0;
int restores = 0;
int preparations = 0;
int destructions = 0;

void check(bool ok, const char* message) {
    ++checks;
    if (!ok)
        throw std::runtime_error(message);
}

class StatefulGain final : public format::Processor {
  public:
    ~StatefulGain() override {
        ++destructions;
    }
    format::PluginDescriptor descriptor() const override {
        format::PluginDescriptor d;
        d.name = "Stateful Gain";
        d.manufacturer = "Example";
        d.bundle_id = "org.example.stateful-gain";
        d.version = "1.0.0";
        d.input_buses = {{"Input", 1, false}};
        d.output_buses = {{"Output", 1, false}};
        return d;
    }
    void define_parameters(state::StateStore& store) override {
        state::ParamInfo gain;
        gain.id = 7;
        gain.name = "Gain";
        gain.range = {0.0f, 1.0f, 0.25f};
        store.add_parameter(gain);
    }
    void prepare(const format::PrepareContext&) override {
        ++preparations;
    }
    void process(audio::BufferView<float>& out, const audio::BufferView<const float>& in,
                 midi::MidiBuffer&, midi::MidiBuffer&, const format::ProcessContext&) override {
        for (std::size_t i = 0; i < out.num_samples(); ++i)
            out.channel_ptr(0)[i] = in.channel_ptr(0)[i] * state().get_value(7) + offset;
    }
    std::vector<std::uint8_t> serialize_plugin_state() const override {
        return large_state.empty() ? std::vector<std::uint8_t>{offset} : large_state;
    }
    bool deserialize_plugin_state(std::span<const std::uint8_t> bytes) override {
        ++restores;
        // This also proves StateStore was bound and parameters were restored
        // before the custom callback.
        check(state().get_value(7) == 0.75f, "bound parameter state precedes custom restore");
        if (bytes.size() > 1 && bytes[0] == 42) {
            large_state.assign(bytes.begin(), bytes.end());
            return true;
        }
        if (bytes.size() != 1 || bytes[0] == 254)
            return false;
        if (bytes[0] == 255)
            throw std::runtime_error("planted custom restore throw");
        offset = bytes[0];
        return true;
    }
    std::uint8_t offset = 1;
    std::vector<std::uint8_t> large_state;
};

// Deliberately distinct from the product bundle ID: the registered graph
// identity, rather than the bundle fallback, must be serialized.
const ProcessorTypeKey key{"org.example.graph.stateful-gain", 7};
const std::array allowed{key};

std::shared_ptr<format::ProcessorNodeInstance> fresh() {
    ++constructions;
    return format::ProcessorNodeInstance::create(std::make_unique<StatefulGain>());
}

ProcessorRestoreDescriptor descriptor() {
    return {key, 1, 1, 1, {}, fresh};
}

struct Fixture {
    std::vector<std::uint8_t> parameters;
    std::vector<std::uint8_t> custom{3};
    Fixture() {
        auto instance = fresh();
        instance->state_store().set_value(7, 0.75f);
        parameters = instance->state_store().serialize();
    }
    ProcessorRestoreRecord record(std::uint32_t id = 1) const {
        return {id, key, 1, 1, 1, parameters, custom};
    }
};

void expect_failure(const ProcessorRestoreResult& result, Code code, std::uint32_t node) {
    if (result.code != code)
        throw std::runtime_error(std::string("expected ") + processor_restore_code_name(code) +
                                 ", got " + processor_restore_code_name(result.code));
    check(result.processors.empty(), "failure exports no candidates");
    check(result.node_id == node, "failure identifies the affected node");
}

void wire(SignalGraph& graph, const std::shared_ptr<format::ProcessorNodeInstance>& instance) {
    const auto input = graph.add_input_node(1);
    const auto processor = graph.add_processor_node(instance);
    const auto output = graph.add_output_node(1);
    check(processor != 0 && graph.connect(input, 0, processor, 0) &&
              graph.connect(processor, 0, output, 0),
          "restored instance wires through public graph");
    check(graph.prepare(48000, 8), "graph prepares restored processor");
}

std::array<float, 8> render(SignalGraph& graph) {
    const std::array<float, 8> input{0, 1, -1, 0.5f, -0.5f, 2, -2, 4};
    std::array<float, 8> output{};
    const float* in_ptr[] = {input.data()};
    float* out_ptr[] = {output.data()};
    audio::BufferView<const float> in(in_ptr, 1, input.size());
    audio::BufferView<float> out(out_ptr, 1, output.size());
    graph.process(out, in, static_cast<int>(output.size()));
    return output;
}

void positive_and_isolation() {
    Fixture f;
    ProcessorRestoreRegistry registry;
    check(registry.register_type(descriptor()) == Code::Restored, "register exact product factory");
    const std::array records{f.record()};
    const int before_prepare = preparations;
    auto result = registry.restore_batch(records, allowed);
    check(result.ok() && result.processors.size() == 1, "complete batch is returned");
    check(preparations == before_prepare, "restoration does not prepare or publish");
    auto& restored = result.processors[0].instance;
    check(restored->state_store().serialize() == f.parameters, "parameter payload roundtrips");
    check(restored->processor().serialize_plugin_state() == f.custom, "custom payload roundtrips");
    SignalGraph restored_graph;
    wire(restored_graph, restored);
    check(render(restored_graph) ==
              std::array<float, 8>{3, 3.75f, 2.25f, 3.375f, 2.625f, 4.5f, 1.5f, 6},
          "graph output depends on restored parameter and custom values");
    const std::unordered_map<NodeId, std::pair<float, float>> layout{
        {restored_graph.nodes()[0].id, {12.5f, -3.0f}},
        {restored_graph.nodes()[1].id, {20.0f, 7.25f}},
        {restored_graph.nodes()[2].id, {42.0f, 8.0f}}};
    const auto graph_json = GraphSerializer::to_json(restored_graph, layout);
    check(!graph_json.empty() && graph_json.find("\"processors\"") != std::string::npos,
          "graph v4 Processor envelope emits through the installed SDK");
    check(graph_json.find("\"type_id\": \"org.example.graph.stateful-gain\"") !=
                  std::string::npos &&
              graph_json.find("\"type_id\": \"org.example.stateful-gain\"") == std::string::npos,
          "graph v4 emits registered identity distinct from bundle ID");
    GraphLoadOptions load_options;
    load_options.processor_registry = &registry;
    load_options.processor_allowlist = {key};
    const auto graph_file =
        std::filesystem::temp_directory_path() / "pulp-g2-processor-restore-consumer.pulpgraph";
    {
        std::ofstream output(graph_file, std::ios::binary | std::ios::trunc);
        check(static_cast<bool>(output), "graph file fixture opens for writing");
        output << graph_json;
        check(static_cast<bool>(output), "graph file fixture writes complete JSON");
    }
    SignalGraph file_graph;
    const auto file_loaded = GraphSerializer::from_file(file_graph, graph_file, load_options);
    check(file_loaded.ok, "graph v4 reloads from a file through the installed SDK");
    check(GraphSerializer::to_json(file_graph, file_loaded.editor_layout) == graph_json,
          "graph file reload preserves canonical notation byte-for-byte");
    std::filesystem::remove(graph_file);

    SignalGraph roundtrip_graph;
    const auto roundtrip = GraphSerializer::from_json(roundtrip_graph, graph_json, load_options);
    check(roundtrip.ok, "graph v4 Processor envelope restores through the installed SDK");
    check(roundtrip.editor_layout == layout, "graph v4 restores every editor layout coordinate");
    const auto roundtrip_json = GraphSerializer::to_json(roundtrip_graph, roundtrip.editor_layout);
    check(roundtrip_json.find("\"x\": 12.5") != std::string::npos &&
              roundtrip_json.find("\"y\": -3") != std::string::npos,
          "graph v4 layout coordinates survive graph topology roundtrip");
    const auto parameter_field = graph_json.substr(graph_json.find("\"parameters_b64\""), 160);
    const auto custom_field = graph_json.substr(graph_json.find("\"custom_b64\""), 80);
    check(roundtrip_json.find(parameter_field) != std::string::npos &&
              roundtrip_json.find(custom_field) != std::string::npos,
          "graph v4 restore preserves parameter/custom bytes");
    SignalGraph untouched;
    check(untouched.add_input_node(1) != 0, "graph file negative starts with a live graph");
    const auto before_missing = untouched.nodes().size();
    const auto missing_file = GraphSerializer::from_file(untouched, graph_file, load_options);
    check(!missing_file.ok && missing_file.error.find("cannot be opened") != std::string::npos,
          "missing graph file is rejected before graph mutation");
    check(untouched.nodes().size() == before_missing,
          "missing graph file leaves the existing graph untouched");
    auto oversized_json = graph_json;
    const auto custom_key = oversized_json.find("\"custom_b64\":");
    if (custom_key == std::string::npos) {
        std::cerr << "GRAPH_JSON=" << graph_json << '\n';
        check(false, "graph v4 custom state field is present");
    }
    const auto custom_value_begin =
        oversized_json.find('"', custom_key + std::string("\"custom_b64\":").size());
    check(custom_value_begin != std::string::npos, "graph v4 custom state value is quoted");
    oversized_json.insert(custom_value_begin + 1, std::string(12 * 1024 * 1024, 'A'));
    check(oversized_json.find("\"custom_b64\": \"") != std::string::npos &&
              oversized_json.back() == '}',
          "oversized graph v4 fixture remains valid JSON");
    SignalGraph rejected_graph;
    check(!GraphSerializer::from_json(rejected_graph, oversized_json, load_options).ok,
          "graph v4 rejects oversized encoded state before decode");
    auto malformed_json = graph_json;
    const auto malformed_value = malformed_json.find("\"custom_b64\":");
    const auto malformed_quote = malformed_json.find('"', malformed_value + 14);
    const auto malformed_end = malformed_json.find('"', malformed_quote + 1);
    malformed_json.replace(malformed_quote + 1, malformed_end - malformed_quote - 1, "%%%");
    SignalGraph malformed_graph;
    check(!GraphSerializer::from_json(malformed_graph, malformed_json, load_options).ok,
          "graph v4 rejects malformed base64");
    auto range_json = graph_json;
    const auto type_version_key = range_json.find("\"type_version\": 7");
    check(type_version_key != std::string::npos, "graph v4 type version field is present");
    range_json.replace(type_version_key, std::string("\"type_version\": 7").size(),
                       "\"type_version\": 4294967303");
    SignalGraph range_graph;
    check(!GraphSerializer::from_json(range_graph, range_json, load_options).ok,
          "graph v4 rejects narrowed type version");
    auto orphan_json = graph_json;
    const auto processors_key = orphan_json.find("\"processors\": [");
    check(processors_key != std::string::npos, "graph v4 processor array is present");
    const auto record_begin = orphan_json.find('{', processors_key);
    const auto record_end = orphan_json.find('}', record_begin);
    check(record_begin != std::string::npos && record_end != std::string::npos,
          "graph v4 processor record is present");
    auto surplus_record = orphan_json.substr(record_begin, record_end - record_begin + 1);
    const auto node_id_key = surplus_record.find("\"node_id\": 2");
    check(node_id_key != std::string::npos, "graph v4 matching processor record is present");
    surplus_record.replace(node_id_key, std::string("\"node_id\": 2").size(), "\"node_id\": 99");
    orphan_json.insert(record_end + 1, ", " + surplus_record);
    SignalGraph orphan_graph;
    const auto orphan = GraphSerializer::from_json(orphan_graph, orphan_json, load_options);
    check(!orphan.ok && orphan.error == "Processor restore record has no matching processor node",
          "graph v4 rejects surplus orphan after matching record restores");

    auto live = fresh();
    SignalGraph live_graph;
    wire(live_graph, live);
    const auto original_audio = render(live_graph);
    const auto original_params = live->state_store().serialize();
    const auto original_custom = live->processor().serialize_plugin_state();
    check(original_audio != render(restored_graph), "changed state changes rendered output");
    const std::array<std::uint8_t, 1> bad_custom{254};
    auto late = f.record(2);
    late.custom = bad_custom;
    const std::array batch{f.record(), late};
    const int before_restore = restores;
    const int before_destruction = destructions;
    expect_failure(registry.restore_batch(batch, allowed), Code::ProcessorStateRestoreFailed, 2);
    check(restores == before_restore + 2, "failure occurs after the earlier candidate restored");
    check(destructions == before_destruction + 2,
          "all failed candidates are released before return");
    check(live->state_store().serialize() == original_params &&
              live->processor().serialize_plugin_state() == original_custom &&
              render(live_graph) == original_audio,
          "late restore failure preserves live state and audio");

    ProcessorRestoreRegistry aliases;
    auto alias = descriptor();
    alias.create = [&] { return live; };
    check(aliases.register_type(alias) == Code::Restored, "register planted live alias factory");
    const int before_alias = restores;
    expect_failure(aliases.restore_batch(records, allowed), Code::ProcessorInstanceNotFresh, 1);
    check(restores == before_alias && live->state_store().serialize() == original_params &&
              live->processor().serialize_plugin_state() == original_custom &&
              render(live_graph) == original_audio,
          "live alias is refused before either payload mutates");
}

void planted_negatives() {
    Fixture f;
    ProcessorRestoreRegistry registry;
    check(registry.register_type(descriptor()) == Code::Restored, "register negative fixture type");
    check(registry.register_type(descriptor()) == Code::DuplicateProcessorType,
          "duplicate registration");
    auto invalid = descriptor();
    invalid.type.type_id = "not.namespaced/unsafe";
    check(registry.register_type(invalid) == Code::InvalidDescriptor, "invalid identity rejected");
    auto record = f.record();
    const int before = constructions;
    auto reject = [&](ProcessorRestoreRecord value, Code code) {
        const std::array records{value};
        expect_failure(registry.restore_batch(records, allowed), code, value.node_id);
    };
    const std::array records{record};
    expect_failure(registry.restore_batch(records, {}), Code::ProcessorTypeDisallowed, 1);
    record.type.type_id = "org.example.unknown";
    reject(record, Code::UnknownProcessorType);
    record = f.record();
    record.type.type_version = 2;
    reject(record, Code::UnsupportedVersion);
    record = f.record();
    record.state_schema_version = 2;
    reject(record, Code::UnsupportedVersion);
    record = f.record();
    record.output_channels = 2;
    reject(record, Code::ProcessorPortMismatch);
    const std::array duplicate{f.record(), f.record()};
    expect_failure(registry.restore_batch(duplicate, allowed), Code::InvalidRecord, 1);
    std::vector<std::uint8_t> oversized(ProcessorRestoreRegistry::max_state_part_bytes + 1);
    record = f.record();
    record.custom = oversized;
    reject(record, Code::StateLimitExceeded);
    std::vector<ProcessorRestoreRecord> many(ProcessorRestoreRegistry::max_nodes + 1, f.record());
    expect_failure(registry.restore_batch(many, allowed), Code::StateLimitExceeded, 0);
    // Five references to the same bounded part exceed the aggregate limit
    // without allocating five oversized copies in the caller.
    oversized.resize(ProcessorRestoreRegistry::max_state_part_bytes);
    std::array<ProcessorRestoreRecord, 5> aggregate;
    for (std::size_t i = 0; i < aggregate.size(); ++i) {
        aggregate[i] = f.record(static_cast<std::uint32_t>(i + 1));
        aggregate[i].custom = oversized;
    }
    expect_failure(registry.restore_batch(aggregate, allowed), Code::StateLimitExceeded, 4);
    check(constructions == before, "metadata and size failures execute no factory");

    record = f.record();
    record.parameters = {};
    const int before_custom = restores;
    reject(record, Code::ProcessorStateRestoreFailed);
    check(restores == before_custom, "parameter rejection does not call custom restore");
    auto corrupt_parameters = f.parameters;
    corrupt_parameters.back() ^= 1;
    record = f.record();
    record.parameters = corrupt_parameters;
    reject(record, Code::ProcessorStateRestoreFailed);
    check(restores == before_custom, "parameter CRC failure does not call custom restore");
    const std::array<std::uint8_t, 1> throwing{255};
    record = f.record();
    record.custom = throwing;
    reject(record, Code::ProcessorStateRestoreFailed);

    ProcessorRestoreRegistry wrong_ports;
    auto wrong = descriptor();
    wrong.input_channels = 2;
    check(wrong_ports.register_type(wrong) == Code::Restored, "register lying port descriptor");
    record = f.record();
    record.input_channels = 2;
    const std::array wrong_records{record};
    expect_failure(wrong_ports.restore_batch(wrong_records, allowed), Code::ProcessorPortMismatch,
                   1);
    ProcessorRestoreRegistry wrong_capabilities;
    wrong = descriptor();
    wrong.capabilities.writes_input_buffers = true;
    check(wrong_capabilities.register_type(wrong) == Code::Restored,
          "register lying capability descriptor");
    expect_failure(wrong_capabilities.restore_batch(records, allowed),
                   Code::ProcessorCapabilityMismatch, 1);

    for (const bool throws : {false, true}) {
        ProcessorRestoreRegistry construction_failure;
        auto failing = descriptor();
        failing.create = [throws]() -> std::shared_ptr<format::ProcessorNodeInstance> {
            if (throws)
                throw std::runtime_error("planted construction failure");
            return {};
        };
        check(construction_failure.register_type(failing) == Code::Restored,
              "register construction failure fixture");
        expect_failure(construction_failure.restore_batch(records, allowed),
                       Code::ProcessorConstructionFailed, 1);
    }
}

void candidate_aliases() {
    Fixture f;
    ProcessorRestoreRegistry registry;
    std::weak_ptr<format::ProcessorNodeInstance> first;
    auto type = descriptor();
    type.create = [&] {
        if (auto prior = first.lock())
            return prior;
        auto instance = fresh();
        first = instance;
        return instance;
    };
    check(registry.register_type(type) == Code::Restored, "register candidate alias factory");
    const std::array batch{f.record(), f.record(2)};
    const int before_restore = restores;
    expect_failure(registry.restore_batch(batch, allowed), Code::ProcessorInstanceNotFresh, 2);
    check(restores == before_restore && first.expired(),
          "candidate alias refused before first restore");

    ProcessorRestoreRegistry retained;
    std::shared_ptr<format::ProcessorNodeInstance> stolen;
    type.create = [&] {
        if (auto prior = first.lock())
            stolen = std::move(prior);
        auto instance = fresh();
        if (first.expired())
            first = instance;
        return instance;
    };
    check(retained.register_type(type) == Code::Restored, "register later retaining factory");
    expect_failure(retained.restore_batch(batch, allowed), Code::ProcessorInstanceNotFresh, 1);
    check(restores == before_restore && stolen->state_store().get_value(7) == 0.25f,
          "later factory retention detected before first restore");
}

void independent_parameter_schema() {
    Fixture f;
    ProcessorRestoreRegistry registry;
    auto type = descriptor();
    type.create = [] {
        auto instance = fresh();
        instance->state_store().set_state_version(3);
        return instance;
    };
    check(registry.register_type(type) == Code::Restored, "register independent parameter schema");
    const std::array old_records{f.record()};
    expect_failure(registry.restore_batch(old_records, allowed), Code::UnsupportedVersion, 1);
    auto state_source = fresh();
    state_source->state_store().set_state_version(3);
    state_source->state_store().set_value(7, 0.75f);
    f.parameters = state_source->state_store().serialize();
    const std::array current_records{f.record()};
    auto result = registry.restore_batch(current_records, allowed);
    check(result.ok(), "envelope schema 1 and parameter schema 3 are independent");
    check(result.processors[0].instance->state_store().serialize() == f.parameters,
          "matching parameter schema retains exact bytes");
    SignalGraph emitted;
    wire(emitted, result.processors[0].instance);
    const auto json = GraphSerializer::to_json(emitted);
    check(!json.empty(), "independent parameter schema emits graph v4 JSON");
    GraphLoadOptions options;
    options.processor_registry = &registry;
    options.processor_allowlist = {key};
    SignalGraph decoded;
    const auto loaded = GraphSerializer::from_json(decoded, json, options);
    check(loaded.ok, "envelope schema 1 and parameter schema 3 roundtrip through graph JSON");
    state_source->state_store().set_state_version(4);
    f.parameters = state_source->state_store().serialize();
    const std::array future_records{f.record()};
    expect_failure(registry.restore_batch(future_records, allowed), Code::UnsupportedVersion, 1);
}

void aggregate_json_boundary() {
    Fixture fixture;
    check(fixture.parameters.size() == 24,
          "aggregate fixture has measured 24-byte parameter state");
    ProcessorRestoreRegistry registry;
    check(registry.register_type(descriptor()) == Code::Restored,
          "register exact-limit Processor factory");
    SignalGraph source;
    constexpr auto part_limit = ProcessorRestoreRegistry::max_state_part_bytes;
    for (int i = 0; i < 4; ++i) {
        auto instance = fresh();
        instance->state_store().set_value(7, 0.75f);
        instance->set_graph_type_id(key.type_id);
        instance->set_graph_type_version(key.type_version);
        auto& state = static_cast<StatefulGain&>(instance->processor()).large_state;
        state.assign(i == 3 ? part_limit - 4 * fixture.parameters.size() : part_limit, 42);
        check(source.add_processor_node(instance) != 0, "add bounded Processor node");
    }
    GraphLoadOptions options;
    options.processor_registry = &registry;
    options.processor_allowlist = {key};
    auto json = GraphSerializer::to_json(source);
    check(!json.empty(), "serialize exact 32 MiB aggregate state");
    SignalGraph exact;
    const auto accepted = GraphSerializer::from_json(exact, json, options);
    check(accepted.ok, "graph v4 accepts exact 32 MiB decoded aggregate");
    const auto reserialized = GraphSerializer::to_json(exact);
    if (reserialized != json) {
        // Diagnostic for a CI-only divergence: report where the roundtrip differs.
        std::size_t at = 0;
        while (at < json.size() && at < reserialized.size() && json[at] == reserialized[at])
            ++at;
        const auto from = at > 120 ? at - 120 : 0;
        std::cerr << "ROUNDTRIP_DIFF source_size=" << json.size()
                  << " roundtrip_size=" << reserialized.size() << " first_diff=" << at
                  << "\nsource   ...=" << json.substr(from, 240)
                  << "\nroundtrip...=" << reserialized.substr(from, 240) << '\n';
    }
    check(reserialized == json, "exact-limit Processor state survives graph JSON roundtrip");

    // A fourth state's final byte moves the aggregate one byte past the
    // budget, while every individual part remains under its own cap.
    const auto final = source.nodes().back().id;
    const auto instance = source.processor_node_instance(final);
    check(instance != nullptr, "fourth Processor instance is available");
    static_cast<StatefulGain&>(instance->processor()).large_state.push_back(42);
    json = GraphSerializer::to_json(source);
    SignalGraph over_limit;
    const auto rejected = GraphSerializer::from_json(over_limit, json, options);
    check(!rejected.ok && rejected.error.find("aggregate limit") != std::string::npos,
          "graph v4 rejects aggregate decoded limit plus one byte");
}
} // namespace

int main() {
    try {
        positive_and_isolation();
        planted_negatives();
        candidate_aliases();
        independent_parameter_schema();
        aggregate_json_boundary();
        std::cout << "PASS: " << checks << " Processor restore and graph v4 emission checks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
