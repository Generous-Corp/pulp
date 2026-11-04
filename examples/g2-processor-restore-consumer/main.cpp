#include <pulp/host/graph_serializer.hpp>
#include <pulp/host/processor_restore.hpp>
#include <pulp/host/signal_graph.hpp>

#include <array>
#include <iostream>
#include <stdexcept>

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
        return {offset};
    }
    bool deserialize_plugin_state(std::span<const std::uint8_t> bytes) override {
        ++restores;
        // This also proves StateStore was bound and parameters were restored
        // before the custom callback.
        check(state().get_value(7) == 0.75f, "bound parameter state precedes custom restore");
        if (bytes.size() != 1 || bytes[0] == 254)
            return false;
        if (bytes[0] == 255)
            throw std::runtime_error("planted custom restore throw");
        offset = bytes[0];
        return true;
    }
    std::uint8_t offset = 1;
};

const ProcessorTypeKey key{"org.example.stateful-gain", 1};
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
    check(GraphSerializer::to_json(restored_graph).empty(),
          "graph-file persistence remains unsupported");

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
    state_source->state_store().set_state_version(4);
    f.parameters = state_source->state_store().serialize();
    const std::array future_records{f.record()};
    expect_failure(registry.restore_batch(future_records, allowed), Code::UnsupportedVersion, 1);
}
} // namespace

int main() {
    try {
        positive_and_isolation();
        planted_negatives();
        candidate_aliases();
        independent_parameter_schema();
        std::cout << "PASS: " << checks
                  << " Processor restore checks; graph v4 persistence not claimed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
