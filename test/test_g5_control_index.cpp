#include <catch2/catch_test_macros.hpp>

#include <pulp/format/processor.hpp>
#include <pulp/host/control_index_nodes.hpp>
#include <pulp/host/graph_patch.hpp>
#include <pulp/host/graph_serializer.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/host/signal_graph_executor_contract.hpp>
#include <pulp/host/signal_graph_executor_routing.hpp>
#include <pulp/host/signal_graph_prepared_topology_edit.hpp>
#include <pulp/host/trigger_graph.hpp>
#include <pulp/signal/control_math.hpp>
#include <pulp/state/param_cursor.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <span>
#include <string>
#include <vector>

using namespace pulp::host;

namespace {

CustomNodeType make_audio_probe() {
    CustomNodeType type;
    type.type_id = "pulp.test.control-audio-probe";
    type.num_input_ports = 2;
    type.num_output_ports = 1;
    type.input_ports = {{"control", PortKind::Control}, {"audio", PortKind::Audio}};
    type.output_ports = {{"audio", PortKind::Audio}};
    type.process = [](pulp::audio::BufferView<float>& out,
                      const pulp::audio::BufferView<const float>& in, int frames) {
        for (int i = 0; i < frames; ++i)
            out.channel_ptr(0)[i] = in.channel_ptr(0)[i] + in.channel_ptr(1)[i];
    };
    return type;
}

class ControlModulatedProcessor final : public pulp::format::Processor {
  public:
    static constexpr pulp::state::ParamID kCutoff = 17;
    static inline std::atomic<int> calls{0};
    static inline std::atomic<bool> reject{false};

    pulp::format::PluginDescriptor descriptor() const override {
        pulp::format::PluginDescriptor descriptor;
        descriptor.name = "Control modulation probe";
        descriptor.manufacturer = "Pulp";
        descriptor.bundle_id = "com.pulp.test.control-modulation";
        descriptor.version = "1";
        descriptor.category = pulp::format::PluginCategory::Effect;
        descriptor.input_buses = {{"In", 1, false}};
        descriptor.output_buses = {{"Out", 1, false}};
        descriptor.node_capabilities.consumes_audio_rate_modulations = true;
        return descriptor;
    }

    void define_parameters(pulp::state::StateStore& store) override {
        pulp::state::ParamInfo parameter;
        parameter.id = kCutoff;
        parameter.name = "Cutoff";
        parameter.range = {0.0f, 1.0f, 0.0f};
        parameter.rate = pulp::state::ParamRate::AudioRate;
        parameter.modulatable = true;
        store.add_parameter(parameter);
    }

    void prepare(const pulp::format::PrepareContext&) override {}

    void process(pulp::audio::BufferView<float>& output,
                 const pulp::audio::BufferView<const float>& input, pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&, const pulp::format::ProcessContext&) override {
        output.clear();
        (void)input;
    }

    bool process_block(pulp::format::ProcessBlock& block) override {
        if (reject.load(std::memory_order_relaxed))
            return false;
        const auto* input = block.buses->first(pulp::format::BusDirection::Input);
        auto* output = block.buses->first(pulp::format::BusDirection::Output);
        if (!input || !output || !block.events || block.events->audio_rate_modulations.size() != 1)
            return false;
        const auto& lane = block.events->audio_rate_modulations.front();
        if (lane.param_id != kCutoff || lane.values.size() != block.frame_count)
            return false;
        calls.fetch_add(1, std::memory_order_relaxed);
        for (std::size_t i = 0; i < block.frame_count; ++i)
            output->output.channel_ptr(0)[i] = input->input.channel_ptr(0)[i] + lane.values[i];
        return true;
    }
};

std::vector<float> render_index_program(const std::vector<int>& partitions, bool routed = true) {
    SignalGraph graph;
    graph.set_canonical_executor_routing_enabled(routed);
    REQUIRE(graph.register_custom_node_type(make_control_indexed_table_type()));
    REQUIRE(graph.register_custom_node_type(make_audio_probe()));
    const auto audio = graph.add_input_node(1);
    const auto index = graph.add_control_index_input_node();
    const auto table = graph.add_custom_node("pulp.control.indexed-table");
    const auto probe = graph.add_custom_node("pulp.test.control-audio-probe");
    const auto sink = graph.add_output_node(1);
    REQUIRE(index != 0);
    REQUIRE(table != 0);
    REQUIRE(probe != 0);
    REQUIRE(graph.connect(graph.node(index)->out("index"), graph.node(table)->in("index")));
    REQUIRE(graph.connect(graph.node(table)->out("value"), graph.node(probe)->in("control")));
    REQUIRE(graph.connect(graph.node(audio)->out("audio"), graph.node(probe)->in("audio")));
    REQUIRE(graph.connect(probe, 0, sink, 0));
    const std::array<float, 4> table_data{0, 1, 2, 3};
    REQUIRE(graph.set_custom_node_state(table, make_control_table_state(table_data)));
    REQUIRE(graph.prepare(48000, 96));

    std::vector<float> indices(96);
    std::vector<float> audio_data(96);
    std::vector<float> result(96, -100);
    for (std::size_t i = 0; i < indices.size(); ++i) {
        indices[i] = static_cast<float>((i / 8) % 6) - 1.0f;
        audio_data[i] = static_cast<float>(i) / 96.0f;
    }
    indices[7] = std::numeric_limits<float>::quiet_NaN();
    indices[23] = std::numeric_limits<float>::infinity();
    indices[40] = 2.5f;
    indices[41] = 1.5f;
    std::size_t cursor = 0;
    for (int requested : partitions) {
        const auto frames = static_cast<std::size_t>(requested);
        REQUIRE(frames > 0);
        REQUIRE(cursor + frames <= indices.size());
        const float* in_ptr[] = {audio_data.data() + cursor};
        float* out_ptr[] = {result.data() + cursor};
        pulp::audio::BufferView<const float> input(in_ptr, 1, frames);
        pulp::audio::BufferView<float> output(out_ptr, 1, frames);
        CHECK(graph.process_control_checked(
                  output, input,
                  {1, cursor, std::span<const float>(indices.data() + cursor, frames)}) ==
              ControlProcessResult::Accepted);
        cursor += frames;
    }
    REQUIRE(cursor == indices.size());
    CHECK(result[0] == audio_data[0]);
    CHECK(result[7] == audio_data[7]);
    CHECK(result[23] == audio_data[23]);
    CHECK(result[40] == audio_data[40] + 2.5f);
    CHECK(result[41] == audio_data[41] + 1.5f);
    return result;
}

} // namespace

TEST_CASE("graph Control index samples are distinct from audio and partition stable",
          "[graph][control-index]") {
    const auto whole = render_index_program({96});
    std::vector<int> uneven;
    for (int remaining = 96; remaining > 0;) {
        const int next = std::min(remaining, static_cast<int>(uneven.size() % 5 + 1));
        uneven.push_back(next);
        remaining -= next;
    }
    CHECK(render_index_program(uneven) == whole);
    CHECK(render_index_program({32, 64}) == whole);
    // With canonical routing off the legacy reference walk renders the same
    // checked blocks and reports them Accepted, not RenderFailed.
    CHECK(render_index_program({96}, false) == whole);
    CHECK(render_index_program(uneven, false) == whole);
}

TEST_CASE("Control index checked admission refuses without changing output or cursor",
          "[graph][control-index]") {
    SignalGraph graph;
    const auto index = graph.add_control_index_input_node();
    REQUIRE(index != 0);
    REQUIRE_FALSE(graph.add_control_index_input_node());
    REQUIRE(graph.prepare(48000, 4));
    std::array<float, 4> audio{1, 2, 3, 4};
    std::array<float, 4> indices{0, 1, 2, 3};
    std::array<float, 4> result{9, 9, 9, 9};
    const float* in_ptr[] = {audio.data()};
    float* out_ptr[] = {result.data()};
    pulp::audio::BufferView<const float> input(in_ptr, 1, 4);
    pulp::audio::BufferView<float> output(out_ptr, 1, 4);
    CHECK(graph.process_control_checked(output, input, {1, 1, indices}) ==
          ControlProcessResult::WrongStart);
    CHECK((result == std::array<float, 4>{9, 9, 9, 9}));
    CHECK(graph.process_control_checked(output, input, {2, 0, indices}) ==
          ControlProcessResult::StaleEpoch);
    CHECK((result == std::array<float, 4>{9, 9, 9, 9}));
    CHECK(graph.process_control_checked(output, input, {1, 0, indices}) ==
          ControlProcessResult::Accepted);
    CHECK(graph.process_control_checked(output, input, {1, 0, indices}) ==
          ControlProcessResult::WrongStart);
    result.fill(9);
    graph.process(output, input, 4);
    CHECK((result == std::array<float, 4>{0, 0, 0, 0}));
    // Two WrongStart, one StaleEpoch, and the unchecked process() call.
    CHECK(graph.control_index_refusals() == 4);

    pulp::format::ProcessContext seek;
    seek.reset_requested = true;
    seek.position_samples = 100;
    result.fill(9);
    CHECK(graph.process_control_checked(output, input, {2, 100, indices}, &seek) ==
          ControlProcessResult::Accepted);
    CHECK(graph.process_control_checked(output, input, {2, 104, indices}) ==
          ControlProcessResult::Accepted);
    CHECK(graph.process_control_checked(output, input, {1, 108, indices}) ==
          ControlProcessResult::StaleEpoch);
}

TEST_CASE("Control index position reports the clock and resets on prepare and commit",
          "[graph][control-index]") {
    SignalGraph unbound;
    REQUIRE(unbound.add_input_node(1) != 0);
    REQUIRE(unbound.prepare(48000, 4));
    CHECK_FALSE(unbound.control_index_position().bound);

    SignalGraph graph;
    REQUIRE(graph.add_control_index_input_node() != 0);
    REQUIRE(graph.prepare(48000, 4));
    auto position = graph.control_index_position();
    REQUIRE(position.bound);
    CHECK(position.epoch == 1);
    CHECK(position.cursor == 0);

    std::array<float, 4> audio{1, 2, 3, 4};
    std::array<float, 4> indices{0, 1, 2, 3};
    std::array<float, 4> result{};
    const float* in_ptr[] = {audio.data()};
    float* out_ptr[] = {result.data()};
    pulp::audio::BufferView<const float> input(in_ptr, 1, 4);
    pulp::audio::BufferView<float> output(out_ptr, 1, 4);
    const auto submit = [&](std::uint64_t epoch, std::uint64_t start) {
        return graph.process_control_checked(output, input, {epoch, start, indices});
    };
    CHECK(submit(position.epoch, position.cursor) == ControlProcessResult::Accepted);
    position = graph.control_index_position();
    CHECK(position.epoch == 1);
    CHECK(position.cursor == 4);

    // Every admission refusal is counted and leaves the clock where it was.
    const auto before = graph.control_index_refusals();
    CHECK(submit(1, 0) == ControlProcessResult::WrongStart);
    CHECK(submit(2, 4) == ControlProcessResult::StaleEpoch);
    CHECK(graph.process_control_checked(output, input, {1, 4, {}}) ==
          ControlProcessResult::OutOfRange);
    CHECK(graph.control_index_refusals() == before + 3);
    CHECK(graph.control_index_position().cursor == 4);
    CHECK(submit(1, 4) == ControlProcessResult::Accepted);

    pulp::format::ProcessContext seek;
    seek.reset_requested = true;
    seek.position_samples = 1000;
    CHECK(graph.process_control_checked(output, input, {2, 1000, indices}, &seek) ==
          ControlProcessResult::Accepted);
    position = graph.control_index_position();
    CHECK(position.epoch == 2);
    CHECK(position.cursor == 1004);

    // Re-prepare compiles a fresh lane.
    REQUIRE(graph.prepare(48000, 4));
    position = graph.control_index_position();
    CHECK(position.epoch == 1);
    CHECK(position.cursor == 0);
    CHECK(submit(1, 0) == ControlProcessResult::Accepted);

    // So does committing a prepared edit.
    auto edit = graph.begin_prepared_topology_edit();
    REQUIRE(edit);
    REQUIRE(edit->add_input_node(1) != 0);
    REQUIRE(edit->prepare(48000, 4) == SignalGraph::PreparedTopologyEdit::Result::Prepared);
    REQUIRE(edit->commit() == SignalGraph::PreparedTopologyEdit::Result::Committed);
    position = graph.control_index_position();
    CHECK(position.epoch == 1);
    CHECK(position.cursor == 0);
}

TEST_CASE("Control index refusals are named and each names its fix", "[graph][control-index]") {
    std::array<float, 4> audio{1, 2, 3, 4};
    std::array<float, 4> indices{0, 1, 2, 3};
    std::array<float, 4> result{9, 9, 9, 9};
    const float* in_ptr[] = {audio.data()};
    float* out_ptr[] = {result.data()};
    pulp::audio::BufferView<const float> input(in_ptr, 1, 4);
    pulp::audio::BufferView<float> output(out_ptr, 1, 4);

    SignalGraph plain;
    REQUIRE(plain.add_input_node(1) != 0);
    REQUIRE(plain.prepare(48000, 4));
    CHECK(plain.process_control_checked(output, input, {1, 0, indices}) ==
          ControlProcessResult::Unbound);

    SignalGraph graph;
    REQUIRE(graph.add_control_index_input_node() != 0);
    REQUIRE(graph.prepare(48000, 4));
    CHECK(graph.process_control_checked(output, input, {1, 0, {}}) ==
          ControlProcessResult::OutOfRange);
    CHECK((result == std::array<float, 4>{9, 9, 9, 9}));

    const std::array all{ControlProcessResult::Accepted,    ControlProcessResult::Unbound,
                         ControlProcessResult::OutOfRange,  ControlProcessResult::StaleEpoch,
                         ControlProcessResult::WrongStart,  ControlProcessResult::ClockExhausted,
                         ControlProcessResult::Busy,        ControlProcessResult::UnsupportedMode,
                         ControlProcessResult::RenderFailed};
    std::vector<std::string> names;
    for (const auto result_code : all) {
        const std::string name = control_process_result_name(result_code);
        const std::string fix = control_process_result_fix(result_code);
        CHECK(name != "unknown");
        CHECK(fix != "unknown result");
        CHECK_FALSE(fix.empty());
        for (const auto& prior : names)
            CHECK(prior != name);
        names.push_back(name);
    }
    CHECK(std::string(control_process_result_fix(ControlProcessResult::UnsupportedMode))
              .find("parallel routing") != std::string::npos);
    CHECK(std::string(control_process_result_fix(ControlProcessResult::WrongStart))
              .find("control_index_position()") != std::string::npos);
}

TEST_CASE("Control scalar rules pin zero division, finite extremes, and reset",
          "[signal][control-index]") {
    CHECK(pulp::signal::safe_control_divide(1.0f, 0.0f) == 0.0f);
    CHECK(pulp::signal::safe_control_divide(1.0, 0.0) == 0.0);
    CHECK(pulp::signal::safe_control_divide(std::numeric_limits<float>::max(), 0.5f) ==
          std::numeric_limits<float>::max());
    CHECK(pulp::signal::control_min(std::numeric_limits<float>::infinity(), 2.0f) == 0.0f);
    pulp::signal::ControlIntegrator integrator;
    REQUIRE(integrator.configure(-1.0f, 1.0f, 0.25f));
    CHECK(integrator.process(1.0f) == 1.0f);
    CHECK(integrator.process(-4.0f) == -1.0f);
    integrator.reset();
    CHECK(integrator.value() == 0.25f);
    CHECK_FALSE(integrator.configure(2.0f, 1.0f, 0.0f));
    CHECK(integrator.value() == 0.25f);
}

TEST_CASE("Control port kind and canonical persistence reject coercion", "[graph][control-index]") {
    SignalGraph graph;
    REQUIRE(graph.register_custom_node_type(make_control_map_clip_type()));
    const auto a = graph.add_custom_node("pulp.control.map-clip");
    const auto b = graph.add_custom_node("pulp.control.map-clip");
    const auto audio = graph.add_input_node(1);
    REQUIRE(graph.connect(graph.node(a)->out("value"), graph.node(b)->in("value")));
    CHECK_FALSE(graph.connect(graph.node(a)->out("value"), graph.node(audio)->out("audio")));
    CHECK_FALSE(graph.connect_feedback(a, 0, b, 0));
    REQUIRE(graph.connections().size() == 1);
    CHECK(graph.connections()[0].control_signal);
    const auto json = GraphSerializer::to_json(graph);
    REQUIRE_FALSE(json.empty());
    CHECK(json.find("\"format_version\": 5") != std::string::npos);
    CHECK(json.find("\"control_signal\": true") != std::string::npos);
    SignalGraph restored;
    REQUIRE(restored.register_custom_node_type(make_control_map_clip_type()));
    const auto loaded = GraphSerializer::from_json(restored, json);
    REQUIRE(loaded.ok);
    REQUIRE(restored.connections().size() == 1);
    CHECK(restored.connections()[0].control_signal);
    CHECK(GraphSerializer::to_json(restored) == json);

    auto dropped_kind = json;
    const auto tag = dropped_kind.find("\"control_signal\": true");
    REQUIRE(tag != std::string::npos);
    dropped_kind.replace(tag, std::string("\"control_signal\": true").size(),
                         "\"control_signal\": false");
    SignalGraph coerced;
    REQUIRE(coerced.register_custom_node_type(make_control_map_clip_type()));
    CHECK_FALSE(GraphSerializer::from_json(coerced, dropped_kind).ok);

    auto old_envelope = json;
    const auto version = old_envelope.find("\"format_version\": 5");
    REQUIRE(version != std::string::npos);
    old_envelope.replace(version, std::string("\"format_version\": 5").size(),
                         "\"format_version\": 4");
    SignalGraph downgrade;
    CHECK_FALSE(GraphSerializer::from_json(downgrade, old_envelope).ok);

    auto changed = make_control_map_clip_type();
    changed.output_ports[0].name = "changed";
    SignalGraph incompatible;
    REQUIRE(incompatible.register_custom_node_type(std::move(changed)));
    CHECK_FALSE(GraphSerializer::from_json(incompatible, json).ok);

    SignalGraph unresolved;
    const auto missing = GraphSerializer::from_json(unresolved, json);
    REQUIRE(missing.ok);
    REQUIRE_FALSE(missing.missing_custom_node_types.empty());
    CHECK(GraphSerializer::to_json(unresolved) == json);
    CHECK_FALSE(unresolved.prepare(48000, 16));
}

TEST_CASE("numeric connections refuse typed ports but keep legacy MIDI node edges",
          "[graph][control-index]") {
    SignalGraph graph;
    REQUIRE(graph.register_custom_node_type(make_control_map_clip_type()));
    const auto a = graph.add_custom_node("pulp.control.map-clip");
    const auto b = graph.add_custom_node("pulp.control.map-clip");
    CHECK_FALSE(graph.connect(a, 0, b, 0));
    CHECK(graph.connections().empty());

    // MIDI input/output nodes predate typed ports and have always been joined
    // through the numeric overload; the Control guard must not refuse them.
    const auto midi_in = graph.add_midi_input_node("Keys");
    const auto midi_out = graph.add_midi_output_node("Out");
    REQUIRE(graph.connect(midi_in, 0, midi_out, 0));
    CHECK_FALSE(graph.connect(midi_in, 0, b, 0));
}

TEST_CASE("numeric connections and persistence keep working on trigger nodes",
          "[graph][control-index][trigger]") {
    SignalGraph graph;
    const auto in = graph.add_input_node(2);
    const auto out = graph.add_output_node(1);
    TriggerProducerConfig config;
    const auto onset = graph.add_trigger_producer(config);
    const auto envelope = graph.add_trigger_envelope({0.5f});
    const auto modulation = graph.add_trigger_modulation();
    const auto lowpass = graph.add_trigger_lowpass(0.25f);
    REQUIRE((onset && envelope && modulation && lowpass));
    for (const auto id : {onset, envelope, modulation, lowpass}) {
        const auto* node = graph.node(id);
        REQUIRE(node != nullptr);
        CHECK(node->input_ports.size() == static_cast<std::size_t>(node->num_input_ports));
        CHECK(node->output_ports.size() == static_cast<std::size_t>(node->num_output_ports));
    }
    CHECK(graph.connect(in, 0, onset, 0));
    CHECK(graph.connect(envelope, 0, modulation, 0));
    CHECK(graph.connect(in, 1, modulation, 1));
    CHECK(graph.connect(modulation, 0, lowpass, 0));
    CHECK(graph.connect(lowpass, 0, out, 0));

    const auto json = GraphSerializer::to_json(graph);
    REQUIRE_FALSE(json.empty());
    CHECK(json.find("\"input_ports\"") == std::string::npos);
    SignalGraph restored;
    const auto loaded = GraphSerializer::from_json(restored, json);
    REQUIRE(loaded.ok);
    CHECK(GraphSerializer::to_json(restored) == json);
}

TEST_CASE("restored Control index graphs prepare and future formats name the fix",
          "[graph][control-index][serializer]") {
    SignalGraph source;
    REQUIRE(source.register_custom_node_type(make_control_indexed_table_type()));
    const auto index = source.add_control_index_input_node();
    const auto table = source.add_custom_node("pulp.control.indexed-table");
    REQUIRE(source.connect(source.node(index)->out("index"), source.node(table)->in("index")));
    const auto json = GraphSerializer::to_json(source);
    REQUIRE_FALSE(json.empty());

    // Only the table is registered; the built-in index input registers itself.
    SignalGraph restored;
    REQUIRE(restored.register_custom_node_type(make_control_indexed_table_type()));
    const auto loaded = GraphSerializer::from_json(restored, json);
    REQUIRE(loaded.ok);
    CHECK(loaded.missing_custom_node_types.empty());
    REQUIRE(restored.prepare(48000, 4));
    CHECK(restored.control_index_position().bound);

    auto future = json;
    const auto at = future.find("\"format_version\": 5");
    REQUIRE(at != std::string::npos);
    future.replace(at, std::string("\"format_version\": 5").size(), "\"format_version\": 6");
    SignalGraph refused;
    const auto newer = GraphSerializer::from_json(refused, future);
    REQUIRE_FALSE(newer.ok);
    CHECK(newer.error.find("unsupported graph format_version 6") != std::string::npos);
    CHECK(newer.error.find("upgrade Pulp, or re-export the graph at format 5 or lower") !=
          std::string::npos);

    const auto inspected = GraphSerializer::inspect_document(
        R"({"identity_schema":"pulp.graph.saved-identity.v6","format_version":6})");
    CHECK(inspected.refusal == GraphDocumentRefusal::UnsupportedFutureVersion);
    CHECK(inspected.detail.find("upgrade Pulp") != std::string::npos);
}

namespace {
void scalar_passthrough(void*, const pulp::host::PreparedSampleKernelConfig&,
                        const pulp::host::SampleFrameContext&, const float* input,
                        float* output) noexcept {
    output[0] = input[0];
}
void block_passthrough(pulp::audio::BufferView<float>& out,
                       const pulp::audio::BufferView<const float>& in, int frames) {
    for (int i = 0; i < frames; ++i)
        out.channel_ptr(0)[i] = in.channel_ptr(0)[i];
}
} // namespace

TEST_CASE("re-registering a paired type with different typed ports is not a no-op",
          "[graph][control-index]") {
    CustomNodeType type;
    type.type_id = "pulp.test.typed-pair";
    type.num_input_ports = 1;
    type.num_output_ports = 1;
    type.input_ports = {{"value", PortKind::Control}};
    type.output_ports = {{"value", PortKind::Control}};
    type.process = block_passthrough;
    SampleKernelDescriptor kernel;
    kernel.type_id = type.type_id;
    kernel.num_input_ports = 1;
    kernel.num_output_ports = 1;
    kernel.authored_config_kind = SampleKernelConfigKind::None;
    kernel.process = scalar_passthrough;
    kernel.metadata.category = "test";

    SignalGraph graph;
    REQUIRE(graph.register_custom_node_type(type, kernel));
    CHECK(graph.register_custom_node_type(type, kernel));
    auto renamed = type;
    renamed.output_ports[0].name = "changed";
    CHECK_FALSE(graph.register_custom_node_type(renamed, kernel));
    auto audio_kind = type;
    audio_kind.input_ports[0].kind = PortKind::Audio;
    CHECK_FALSE(graph.register_custom_node_type(audio_kind, kernel));
    CHECK(graph.custom_node_type(type.type_id, 1)->output_ports[0].name == "value");
}

TEST_CASE("graph format v4 documents still load and keep their bytes",
          "[graph][control-index][serializer]") {
    SignalGraph source;
    const auto in = source.add_input_node(2);
    const auto out = source.add_output_node(2);
    REQUIRE(source.connect(in, 0, out, 0));
    const auto legacy = GraphSerializer::to_json(source);
    REQUIRE(legacy.find("\"format_version\": 2") != std::string::npos);
    auto v4 = legacy;
    v4.replace(v4.find("\"format_version\": 2"), std::string("\"format_version\": 2").size(),
               "\"format_version\": 4");
    SignalGraph graph;
    const auto loaded = GraphSerializer::from_json(graph, v4);
    REQUIRE(loaded.ok);
    REQUIRE(graph.connections().size() == 1);
    CHECK_FALSE(graph.connections()[0].control_signal);
    // A graph without typed ports keeps its pre-v5 writer version.
    const auto json = GraphSerializer::to_json(graph);
    CHECK(json == legacy);
}

TEST_CASE("Control edges disconnect on the graph and in a prepared edit",
          "[graph][control-index][edit]") {
    SignalGraph graph;
    REQUIRE(graph.register_custom_node_type(make_control_map_clip_type()));
    const auto a = graph.add_custom_node("pulp.control.map-clip");
    const auto b = graph.add_custom_node("pulp.control.map-clip");
    REQUIRE(graph.connect(graph.node(a)->out("value"), graph.node(b)->in("value")));
    REQUIRE(graph.connections().size() == 1);
    CHECK(graph.disconnect(a, 0, b, 0));
    CHECK(graph.connections().empty());
    CHECK_FALSE(graph.disconnect(a, 0, b, 0));

    REQUIRE(graph.connect(graph.node(a)->out("value"), graph.node(b)->in("value")));
    REQUIRE(graph.prepare(48000, 8));
    auto edit = graph.begin_prepared_topology_edit();
    REQUIRE(edit);
    CHECK(edit->disconnect(a, 0, b, 0));
    CHECK(edit->prepare(48000, 8) == SignalGraph::PreparedTopologyEdit::Result::Prepared);
    CHECK(edit->commit() == SignalGraph::PreparedTopologyEdit::Result::Committed);
    CHECK(graph.connections().empty());

    auto reconnect = graph.begin_prepared_topology_edit();
    REQUIRE(reconnect);
    CHECK(reconnect->connect(reconnect->node(a)->out("value"), reconnect->node(b)->in("value")));
    CHECK(reconnect->disconnect(a, 0, b, 0));
}

TEST_CASE("Control edges keep their kind in latency lookup and refuse notation",
          "[graph][control-index]") {
    SignalGraph graph;
    REQUIRE(graph.register_custom_node_type(make_control_min_type()));
    const auto a = graph.add_custom_node("pulp.control.min", "first");
    const auto b = graph.add_custom_node("pulp.control.min", "second");
    REQUIRE(graph.connect(graph.node(a)->out("value"), graph.node(b)->in("a")));
    REQUIRE(graph.prepare(48000, 8));
    const auto latency = graph.prepared_latency_snapshot();
    REQUIRE(latency.has_value());
    Connection query{a, 0, b, 0};
    CHECK_FALSE(latency->connection_delay(query).has_value());
    query.control_signal = true;
    CHECK(latency->connection_delay(query).has_value());

    GraphPatchCatalog catalog;
    REQUIRE(catalog.register_custom(make_control_min_type()));
    const auto text = format_graph_patch(graph, catalog);
    CHECK_FALSE(static_cast<bool>(text));
    CHECK(text.diagnostic.code == GraphPatchDiagnostic::Code::UnsupportedContract);
    INFO(text.diagnostic.message());
    CHECK(text.diagnostic.rule.find("Control edge") != std::string::npos);
    CHECK(text.diagnostic.suggested_fix.find("GraphSerializer") != std::string::npos);
}

TEST_CASE("candidate Control state validates before mutation and keeps baseline state",
          "[graph][control-index][edit]") {
    SignalGraph graph;
    REQUIRE(graph.register_custom_node_type(make_control_integrator_type()));
    const auto baseline = graph.add_custom_node("pulp.control.integrator");
    REQUIRE(baseline != 0);
    REQUIRE(graph.prepare(48000, 8));
    auto edit = graph.begin_prepared_topology_edit();
    REQUIRE(edit);
    const std::array<float, 3> config{-1, 1, 0.25f};
    const auto bytes = make_control_scalar_state(config);
    CHECK_FALSE(edit->set_control_node_state(baseline, bytes));
    const auto added = edit->add_custom_node("pulp.control.integrator");
    REQUIRE(added != 0);
    const std::array<float, 3> invalid{2, 1, 0};
    CHECK_FALSE(edit->set_control_node_state(added, make_control_scalar_state(invalid)));
    CHECK(edit->set_control_node_state(added, bytes));
    CHECK(edit->prepare(48000, 8) == SignalGraph::PreparedTopologyEdit::Result::Prepared);
    CHECK(edit->commit() == SignalGraph::PreparedTopologyEdit::Result::Committed);
    CHECK(graph.custom_node_state(added) == bytes);
}

TEST_CASE("Control table drives dense AudioRate Processor parameter in both serial backends",
          "[graph][control-index][modulation]") {
    for (const bool routed : {false, true}) {
        ControlModulatedProcessor::calls.store(0, std::memory_order_relaxed);
        ControlModulatedProcessor::reject.store(false, std::memory_order_relaxed);
        SignalGraph graph;
        REQUIRE(graph.register_custom_node_type(make_control_indexed_table_type()));
        const auto audio = graph.add_input_node(1);
        const auto index = graph.add_control_index_input_node();
        const auto table = graph.add_custom_node("pulp.control.indexed-table");
        const auto processor =
            graph.add_processor_node(std::make_unique<ControlModulatedProcessor>());
        const auto sink = graph.add_output_node(1);
        REQUIRE(audio != 0);
        REQUIRE(index != 0);
        REQUIRE(table != 0);
        REQUIRE(processor != 0);
        REQUIRE(sink != 0);
        REQUIRE(graph.connect(graph.node(index)->out("index"), graph.node(table)->in("index")));
        REQUIRE(graph.connect(audio, 0, processor, 0));
        REQUIRE(graph.connect(processor, 0, sink, 0));
        REQUIRE(graph.connect_control_modulation(graph.node(table)->out("value"), processor,
                                                 ControlModulatedProcessor::kCutoff, 0, 1));
        CHECK_FALSE(graph.connect_audio_rate_modulation(table, 0, processor,
                                                        ControlModulatedProcessor::kCutoff, 0, 1));
        REQUIRE(graph.connections().back().control_signal);
        REQUIRE(graph.connections().back().audio_rate_modulation);
        pulp::state::ModulationLane lane;
        REQUIRE(graph.audio_rate_modulation_lane(graph.connections().back(), lane));
        CHECK(lane.source.id == table);
        const std::array<float, 4> table_data{0.0f, 0.25f, 0.5f, 1.0f};
        REQUIRE(graph.set_custom_node_state(table, make_control_table_state(table_data)));
        graph.set_canonical_executor_routing_enabled(routed);
        CHECK(static_cast<bool>(
            validate_signal_graph_executor_topology(graph.nodes(), graph.connections())));
        REQUIRE(graph.prepare(48000, 8));
        CHECK(signal_graph_executor_eligible(graph));

        std::array<float, 8> audio_data{};
        std::array<float, 8> indices{0, 1, 2, 3, 2, 1, 0, 3};
        std::array<float, 8> output_data{};
        audio_data.fill(0.1f);
        const float* in_ptr[] = {audio_data.data()};
        float* out_ptr[] = {output_data.data()};
        pulp::audio::BufferView<const float> input(in_ptr, 1, 8);
        pulp::audio::BufferView<float> output(out_ptr, 1, 8);
        CHECK(graph.process_control_checked(output, input, {1, 0, indices}) ==
              ControlProcessResult::Accepted);
        CHECK(ControlModulatedProcessor::calls.load(std::memory_order_relaxed) > 0);
        CHECK(graph.routed_only_execution_failures() == 0);
        for (std::size_t i = 0; i < 8; ++i) {
            CHECK(std::abs(output_data[i] - (audio_data[i] + table_data[indices[i]])) < 1e-6f);
        }
        ControlModulatedProcessor::reject.store(true, std::memory_order_relaxed);
        output_data.fill(42.0f);
        CHECK(graph.process_control_checked(output, input, {1, 8, indices}) ==
              ControlProcessResult::RenderFailed);
        CHECK(graph.routed_only_execution_failures() == 1);
        for (const float sample : output_data)
            CHECK(sample == 0.0f);
        ControlModulatedProcessor::reject.store(false, std::memory_order_relaxed);
        CHECK(graph.process_control_checked(output, input, {1, 8, indices}) ==
              ControlProcessResult::WrongStart);
        CHECK(graph.process_control_checked(output, input, {1, 16, indices}) ==
              ControlProcessResult::Accepted);
    }
}
