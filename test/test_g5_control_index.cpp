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
#include <pulp/signal/indexed_control.hpp>
#include <pulp/state/param_cursor.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
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

TEST_CASE("Control stages expose pure and stateful finite partition-stable forms",
          "[signal][control-index]") {
    using pulp::signal::control_clamp;
    using pulp::signal::control_max;
    using pulp::signal::control_min;
    using pulp::signal::safe_control_divide;
    CHECK(control_min(std::numeric_limits<float>::infinity(), 2.0f) == 0.0f);
    CHECK(control_max(-std::numeric_limits<float>::infinity(), 2.0f) == 2.0f);
    CHECK(control_clamp(std::numeric_limits<float>::quiet_NaN(), -1.0f, 1.0f) == 0.0f);
    CHECK(safe_control_divide(1.0f, 0.0f) == 0.0f);
    CHECK(pulp::signal::map_clip(0.5f, 0.0f, 1.0f, 10.0f, 20.0f) == 15.0f);

    CHECK_FALSE(
        pulp::signal::hysteresis_step(std::numeric_limits<float>::infinity(), 0.5f, 0.25f, false));
    pulp::signal::Hysteresis hysteresis;
    REQUIRE(hysteresis.configure(0.5f, 0.25f));
    CHECK_FALSE(hysteresis.process(0.49f));
    CHECK(hysteresis.process(0.5f));
    CHECK(hysteresis.process(std::numeric_limits<float>::quiet_NaN()));
    CHECK(hysteresis.process(0.25f) == false);

    const std::array<float, 7> increments{
        0.2f, 0.2f, std::numeric_limits<float>::infinity(), 0.2f, -0.1f, 0.2f, 0.2f};
    const auto run_integrator = [&](std::span<const float> input,
                                    std::span<const std::size_t> partitions) {
        float state = 0.0f;
        std::size_t offset = 0;
        for (const auto count : partitions) {
            for (std::size_t i = 0; i < count; ++i)
                state = pulp::signal::integrator_step(state, input[offset + i], -1.0f, 1.0f);
            offset += count;
        }
        return state;
    };
    const std::array<std::size_t, 1> whole{increments.size()};
    const std::array<std::size_t, 3> split{2, 1, 4};
    CHECK(run_integrator(increments, whole) == run_integrator(increments, split));

    const std::array<float, 6> targets{1.0f, 1.0f, std::numeric_limits<float>::quiet_NaN(),
                                       0.0f, 0.0f, 1.0f};
    const auto run_slew = [&](std::span<const float> input,
                              std::span<const std::size_t> partitions) {
        pulp::signal::Slew slew;
        slew.set_rates(0.25f, 0.5f);
        std::size_t offset = 0;
        for (const auto count : partitions) {
            for (std::size_t i = 0; i < count; ++i)
                (void)slew.process(input[offset + i]);
            offset += count;
        }
        return slew.value();
    };
    const std::array<std::size_t, 1> all_targets{targets.size()};
    const std::array<std::size_t, 3> target_split{1, 2, 3};
    CHECK(run_slew(targets, all_targets) == run_slew(targets, target_split));
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

TEST_CASE("G-5 trigger chain uses a real feedback edge across block partitions",
          "[trigger][feedback]") {
    const auto render = [](std::span<const int> partitions) {
        SignalGraph graph;
        const auto input = graph.add_input_node(1);
        const auto output = graph.add_output_node(1);
        TriggerProducerConfig producer_config;
        producer_config.lane_count = 1;
        producer_config.lanes[0].threshold = 0.5f;
        const auto onset = graph.add_trigger_producer(producer_config);
        const auto envelope = graph.add_trigger_envelope({0.5f});
        const auto modulation = graph.add_trigger_modulation();
        const auto filter = graph.add_trigger_lowpass(0.25f);
        REQUIRE(((input && output && onset && envelope && modulation && filter)));
        REQUIRE(graph.connect(input, 0, onset, 0));
        REQUIRE(graph.connect(envelope, 0, modulation, 0));
        REQUIRE(graph.connect(onset, 0, modulation, 1));
        REQUIRE(graph.connect(modulation, 0, filter, 0));
        REQUIRE(graph.connect_feedback(filter, 0, filter, 0));
        REQUIRE(graph.connect(filter, 0, output, 0));
        REQUIRE(graph.connect_trigger({onset, envelope}) == TriggerGraphResult::Accepted);
        auto plan = graph.prepare_trigger_plan(48000, 16);
        REQUIRE(plan.result == TriggerGraphResult::Accepted);
        REQUIRE(graph.try_install_prepared_trigger_plan(plan.plan) == TriggerGraphResult::Accepted);

        std::array<float, 8> source{0, 1, 0, 0, 0, 0, 0, 0};
        std::array<float, 8> rendered{};
        std::size_t cursor = 0;
        std::vector<std::int64_t> trigger_samples;
        for (const auto frames : partitions) {
            REQUIRE(frames > 0);
            REQUIRE(cursor + static_cast<std::size_t>(frames) <= source.size());
            const float* input_ptr[] = {source.data() + cursor};
            float* output_ptr[] = {rendered.data() + cursor};
            pulp::audio::BufferView<const float> input_view(input_ptr, 1, frames);
            pulp::audio::BufferView<float> output_view(output_ptr, 1, frames);
            REQUIRE(graph.process_checked(output_view, input_view, frames) ==
                    TriggerGraphResult::Accepted);
            for (const auto& record : graph.trigger_trace())
                trigger_samples.push_back(static_cast<std::int64_t>(record.sample));
            cursor += static_cast<std::size_t>(frames);
        }
        REQUIRE(cursor == source.size());
        REQUIRE(trigger_samples.size() == 1);
        CHECK(trigger_samples.front() == 1);
        CHECK(rendered[0] == 0.0f);
        CHECK(rendered[1] > 0.0f);
        CHECK(rendered[2] > 0.0f);
        return rendered;
    };

    const std::array<int, 1> whole{8};
    const std::array<int, 3> split{1, 3, 4};
    // A feedback edge carries the previous block's output, so the loop has a
    // block-sized delay: partitions agree until the first block boundary after
    // the trigger, and the delayed feedback shows up exactly there.
    const auto whole_out = render(whole);
    const auto split_out = render(split);
    for (std::size_t i = 0; i < 4; ++i)
        CHECK(whole_out[i] == split_out[i]);
    CHECK(split_out[4] > whole_out[4]);

    SignalGraph negative;
    const auto in = negative.add_input_node(1);
    const auto out = negative.add_output_node(1);
    const auto onset = negative.add_trigger_producer({});
    const auto envelope = negative.add_trigger_envelope();
    const auto filter = negative.add_trigger_lowpass();
    REQUIRE(((in && out && onset && envelope && filter)));
    REQUIRE(negative.connect(in, 0, onset, 0));
    REQUIRE(negative.connect(envelope, 0, filter, 0));
    REQUIRE(negative.connect_feedback(filter, 0, filter, 0));
    REQUIRE(negative.connect(filter, 0, out, 0));
    REQUIRE(negative.connect_trigger({onset, envelope}) == TriggerGraphResult::Accepted);
    CHECK_FALSE(negative.connect_feedback(filter, 0, filter, 0));
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
    future.replace(at, std::string("\"format_version\": 5").size(), "\"format_version\": 7");
    SignalGraph refused;
    const auto newer = GraphSerializer::from_json(refused, future);
    REQUIRE_FALSE(newer.ok);
    CHECK(newer.error.find("unsupported graph format_version 7") != std::string::npos);
    CHECK(newer.error.find("upgrade Pulp, or re-export the graph at format 6 or lower") !=
          std::string::npos);

    const auto inspected = GraphSerializer::inspect_document(
        R"({"identity_schema":"pulp.graph.saved-identity.v6","format_version":7})");
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

TEST_CASE("MapClip maps non-finite input to the output minimum", "[signal][control-index]") {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    // Zero lies mid-interval, so zero-substitution would not return 100.
    pulp::signal::MapClip map;
    REQUIRE(map.configure(-1.0f, 1.0f, 100.0f, 200.0f));
    REQUIRE(map.process(nan) == 100.0f);
    REQUIRE(map.process(inf) == 100.0f);
    REQUIRE(map.process(-inf) == 100.0f);
    // Zero at/above input_max would map to output_max under zero-substitution.
    pulp::signal::MapClip below;
    REQUIRE(below.configure(-3.0f, -1.0f, 10.0f, 20.0f));
    REQUIRE(below.process(nan) == 10.0f);
    REQUIRE(pulp::signal::map_clip(nan, -1.0f, 1.0f, 100.0f, 200.0f) == 100.0f);
}

namespace {

// Control index -> `node_type` -> audio probe, with a silent audio input, so
// the rendered output is exactly the node's Control output.
NodeId build_control_node_graph(SignalGraph& graph, const CustomNodeType& node_type) {
    REQUIRE(graph.register_custom_node_type(node_type));
    REQUIRE(graph.register_custom_node_type(make_audio_probe()));
    const auto audio = graph.add_input_node(1);
    const auto index = graph.add_control_index_input_node();
    const auto node = graph.add_custom_node(node_type.type_id);
    const auto probe = graph.add_custom_node("pulp.test.control-audio-probe");
    const auto sink = graph.add_output_node(1);
    REQUIRE(node != 0);
    REQUIRE(graph.connect(graph.node(index)->out("index"),
                          graph.node(node)->in(node_type.input_ports.front().name)));
    REQUIRE(graph.connect(graph.node(node)->out("value"), graph.node(probe)->in("control")));
    REQUIRE(graph.connect(graph.node(audio)->out("audio"), graph.node(probe)->in("audio")));
    REQUIRE(graph.connect(probe, 0, sink, 0));
    return node;
}

std::vector<float> render_control_node_graph(SignalGraph& graph, const std::vector<float>& values) {
    std::vector<float> silence(values.size(), 0.0f);
    std::vector<float> result(values.size(), -100.0f);
    const float* in_ptr[] = {silence.data()};
    float* out_ptr[] = {result.data()};
    pulp::audio::BufferView<const float> input(in_ptr, 1, values.size());
    pulp::audio::BufferView<float> output(out_ptr, 1, values.size());
    const auto position = graph.control_index_position();
    REQUIRE(position.bound);
    REQUIRE(
        graph.process_control_checked(output, input, {position.epoch, position.cursor, values}) ==
        ControlProcessResult::Accepted);
    return result;
}

std::vector<float> run_unary_node(const CustomNodeType& type,
                                  const std::vector<std::uint8_t>& state,
                                  const std::vector<float>& input) {
    void* instance = type.create();
    REQUIRE(instance != nullptr);
    if (!state.empty())
        REQUIRE(type.load_state(instance, state));
    type.prepare(instance, 48000.0, static_cast<int>(input.size()));
    std::vector<float> output(input.size(), -100.0f);
    const float* in_ptr[] = {input.data()};
    float* out_ptr[] = {output.data()};
    pulp::audio::BufferView<const float> in(in_ptr, 1, input.size());
    pulp::audio::BufferView<float> out(out_ptr, 1, output.size());
    type.process_instance(instance, out, in, static_cast<int>(input.size()));
    type.destroy(instance);
    return output;
}

std::vector<float> run_binary_node(const CustomNodeType& type, const std::vector<float>& a,
                                   const std::vector<float>& b) {
    REQUIRE(a.size() == b.size());
    std::vector<float> output(a.size(), -100.0f);
    const float* in_ptr[] = {a.data(), b.data()};
    float* out_ptr[] = {output.data()};
    pulp::audio::BufferView<const float> in(in_ptr, 2, a.size());
    pulp::audio::BufferView<float> out(out_ptr, 1, output.size());
    type.process(out, in, static_cast<int>(a.size()));
    return output;
}

} // namespace

TEST_CASE("an unconfigured slew node saves the times it runs with and restores identically",
          "[graph][control-index][serializer]") {
    const std::vector<float> step(96, 1.0f);
    SignalGraph source;
    const auto slew = build_control_node_graph(source, make_control_slew_type());
    REQUIRE(source.prepare(48000, 96));
    const auto original = render_control_node_graph(source, step);
    // SlewLimiterT's default 10 ms rise is 480 samples at 48 kHz, so a unit
    // step is still ramping after 96 samples.
    CHECK(original.front() > 0.0f);
    CHECK(original.back() < 0.5f);

    const auto saved = source.custom_node_state(slew);
    const std::array<float, 2> default_times{10.0f, 10.0f};
    CHECK(saved == make_control_scalar_state(default_times));

    const auto json = GraphSerializer::to_json(source);
    SignalGraph restored;
    REQUIRE(restored.register_custom_node_type(make_control_slew_type()));
    REQUIRE(restored.register_custom_node_type(make_audio_probe()));
    REQUIRE(GraphSerializer::from_json(restored, json).ok);
    REQUIRE(restored.prepare(48000, 96));
    CHECK(render_control_node_graph(restored, step) == original);
}

TEST_CASE("an unconfigured indexed table survives save after prepare and restore",
          "[graph][control-index][serializer]") {
    SignalGraph source;
    const auto table = build_control_node_graph(source, make_control_indexed_table_type());
    REQUIRE(source.prepare(48000, 4));
    // The live instance exists now, so the saved state comes from save_state.
    CHECK(source.custom_node_state(table).empty());
    const auto json = GraphSerializer::to_json(source);

    SignalGraph restored;
    REQUIRE(restored.register_custom_node_type(make_control_indexed_table_type()));
    REQUIRE(restored.register_custom_node_type(make_audio_probe()));
    REQUIRE(GraphSerializer::from_json(restored, json).ok);
    REQUIRE(restored.prepare(48000, 4));
    const std::vector<float> indices{0, 1, 2, 3};
    CHECK(render_control_node_graph(restored, indices) == std::vector<float>(4, 0.0f));
}

TEST_CASE("Control blocks are refused on a graph prepared into the parallel domain",
          "[graph][control-index]") {
    SignalGraph graph;
    CustomNodeType latent;
    latent.type_id = "pulp.test.control-latent";
    latent.num_input_ports = 1;
    latent.num_output_ports = 1;
    latent.latency_samples = [](double) { return 4; };
    latent.process = [](pulp::audio::BufferView<float>& out,
                        const pulp::audio::BufferView<const float>& in, int frames) {
        for (int i = 0; i < frames; ++i)
            out.channel_ptr(0)[i] = in.channel_ptr(0)[i];
    };
    REQUIRE(graph.register_custom_node_type(latent));
    const auto input = graph.add_input_node(1);
    const auto delayed = graph.add_custom_node("pulp.test.control-latent");
    const auto output = graph.add_output_node(1);
    REQUIRE(graph.add_control_index_input_node() != 0);
    REQUIRE(graph.connect(input, 0, delayed, 0));
    REQUIRE(graph.connect(delayed, 0, output, 0));
    REQUIRE(graph.connect(input, 0, output, 0));
    graph.set_canonical_executor_routing_enabled(true);
    graph.set_parallel_routing_enabled(true);
    graph.set_parallel_min_work_units(0);
    REQUIRE(graph.prepare(48000, 4));
    REQUIRE(graph.routed_execution_status(4).parallel_selected);

    // Turning the flag off does not re-prepare: the snapshot keeps the
    // parallel domain prepare fixed for the latency-compensated graph.
    graph.set_parallel_routing_enabled(false);
    REQUIRE_FALSE(graph.parallel_routing_enabled());
    REQUIRE(graph.routed_execution_status(4).parallel_selected);

    std::array<float, 4> audio{1, 2, 3, 4};
    std::array<float, 4> indices{0, 1, 2, 3};
    std::array<float, 4> result{9, 9, 9, 9};
    const float* in_ptr[] = {audio.data()};
    float* out_ptr[] = {result.data()};
    pulp::audio::BufferView<const float> in(in_ptr, 1, 4);
    pulp::audio::BufferView<float> out(out_ptr, 1, 4);
    const auto refusals = graph.control_index_refusals();
    CHECK(graph.process_control_checked(out, in, {1, 0, indices}) ==
          ControlProcessResult::UnsupportedMode);
    CHECK((result == std::array<float, 4>{9, 9, 9, 9}));
    CHECK(graph.control_index_refusals() == refusals + 1);

    // A fresh prepare with the flag off leaves the parallel domain and admits.
    REQUIRE(graph.prepare(48000, 4));
    REQUIRE_FALSE(graph.routed_execution_status(4).parallel_selected);
    CHECK(graph.process_control_checked(out, in, {1, 0, indices}) ==
          ControlProcessResult::Accepted);
}

TEST_CASE("hysteresis, slew, max and safe-divide Control nodes render their rules",
          "[graph][control-index]") {
    SECTION("hysteresis defaults to 0.5 / 0.25 and holds between thresholds") {
        const auto type = make_control_hysteresis_type();
        const std::vector<float> input{0.4f, 0.5f, 0.3f, 0.25f, 0.3f, 0.6f};
        CHECK(run_unary_node(type, {}, input) == std::vector<float>{0, 1, 1, 0, 0, 1});
        const std::array<float, 2> thresholds{0.8f, 0.1f};
        CHECK(run_unary_node(type, make_control_scalar_state(thresholds), input) ==
              std::vector<float>(6, 0.0f));
        const std::array<float, 2> inverted{0.1f, 0.8f};
        void* instance = type.create();
        CHECK_FALSE(type.load_state(instance, make_control_scalar_state(inverted)));
        type.destroy(instance);
    }

    SECTION("slew defaults to 10 ms and honours configured times") {
        const auto type = make_control_slew_type();
        const std::vector<float> step(4, 1.0f);
        const auto ramp = run_unary_node(type, {}, step);
        CHECK(ramp.front() > 0.0f);
        CHECK(ramp.back() < 0.1f);
        const std::array<float, 2> instant{0.0f, 0.0f};
        CHECK(run_unary_node(type, make_control_scalar_state(instant), step) == step);
        const std::array<float, 2> negative{-1.0f, 0.0f};
        void* instance = type.create();
        CHECK_FALSE(type.load_state(instance, make_control_scalar_state(negative)));
        type.destroy(instance);
    }

    SECTION("max and safe-divide map non-finite and zero inputs to finite outputs") {
        const auto inf = std::numeric_limits<float>::infinity();
        const auto nan = std::numeric_limits<float>::quiet_NaN();
        CHECK(run_binary_node(make_control_max_type(), {1.0f, -2.0f, -inf, nan},
                              {0.5f, -1.0f, 2.0f, 3.0f}) ==
              std::vector<float>{1.0f, -1.0f, 2.0f, 3.0f});
        CHECK(run_binary_node(make_control_safe_divide_type(), {1.0f, 1.0f, 6.0f},
                              {0.0f, nan, 3.0f}) == std::vector<float>{0.0f, 0.0f, 2.0f});
    }
}

TEST_CASE("a stateful Control node restarts from its initial state on a seek and on prepare",
          "[graph][control-index][negative-control]") {
    SignalGraph graph;
    const auto integrator = build_control_node_graph(graph, make_control_integrator_type());
    const std::array<float, 3> range{0.0f, 10.0f, 0.0f};
    REQUIRE(graph.set_custom_node_state(integrator, make_control_scalar_state(range)));
    REQUIRE(graph.prepare(48000, 4));
    const std::vector<float> steps(4, 0.25f);
    const std::vector<float> fresh{0.25f, 0.5f, 0.75f, 1.0f};
    REQUIRE(render_control_node_graph(graph, steps) == fresh);
    // The stream continues across ordinary blocks.
    CHECK(render_control_node_graph(graph, steps).front() == 1.25f);

    // A seek opens the next epoch at the transport position; the program
    // renders as it did from a fresh graph rather than continuing from 2.0.
    std::vector<float> silence(4, 0.0f);
    std::vector<float> result(4, -100.0f);
    const float* in_ptr[] = {silence.data()};
    float* out_ptr[] = {result.data()};
    pulp::audio::BufferView<const float> input(in_ptr, 1, 4);
    pulp::audio::BufferView<float> output(out_ptr, 1, 4);
    pulp::format::ProcessContext seek;
    seek.reset_requested = true;
    seek.position_samples = 0;
    const auto position = graph.control_index_position();
    REQUIRE(graph.process_control_checked(output, input, {position.epoch + 1, 0, steps}, &seek) ==
            ControlProcessResult::Accepted);
    CHECK(result == fresh);

    // Re-prepare restarts the clock at epoch 1, cursor 0, and the node with it.
    CHECK(render_control_node_graph(graph, steps).front() == 1.25f);
    REQUIRE(graph.prepare(48000, 4));
    CHECK(render_control_node_graph(graph, steps) == fresh);
}

namespace {

// A stateful Control pass-through whose reset only counts, so a test can see
// whether SignalGraph called it from the audio thread on a seek.
CustomNodeType make_counting_control_type(std::shared_ptr<int> resets, bool declare_rt) {
    CustomNodeType type;
    type.type_id = "pulp.test.control-counting-reset";
    type.num_input_ports = 1;
    type.num_output_ports = 1;
    type.input_ports = {{"value", PortKind::Control}};
    type.output_ports = {{"value", PortKind::Control}};
    type.create = []() -> void* { return new int(0); };
    type.destroy = [](void* instance) { delete static_cast<int*>(instance); };
    type.reset = [resets](void*) { ++*resets; };
    type.process_instance = [](void*, pulp::audio::BufferView<float>& out,
                               const pulp::audio::BufferView<const float>& in, int frames) {
        std::copy_n(in.channel_ptr(0), frames, out.channel_ptr(0));
    };
    if (declare_rt) {
        type.rt_contract.declared = true;
        type.rt_contract.safety_class = pulp::audio::RtSafetyClass::AudioCallbackSafeAfterPrepare;
        type.rt_contract.allocation_free = true;
    }
    return type;
}

int resets_after_seek(bool declare_rt) {
    auto resets = std::make_shared<int>(0);
    SignalGraph graph;
    build_control_node_graph(graph, make_counting_control_type(resets, declare_rt));
    REQUIRE(graph.prepare(48000, 4));
    const std::vector<float> steps(4, 0.25f);
    render_control_node_graph(graph, steps);
    const int before = *resets;
    std::vector<float> silence(4, 0.0f);
    std::vector<float> result(4, -100.0f);
    const float* in_ptr[] = {silence.data()};
    float* out_ptr[] = {result.data()};
    pulp::audio::BufferView<const float> input(in_ptr, 1, 4);
    pulp::audio::BufferView<float> output(out_ptr, 1, 4);
    pulp::format::ProcessContext seek;
    seek.reset_requested = true;
    seek.position_samples = 0;
    const auto position = graph.control_index_position();
    REQUIRE(graph.process_control_checked(output, input, {position.epoch + 1, 0, steps}, &seek) ==
            ControlProcessResult::Accepted);
    CHECK(result == steps);
    return *resets - before;
}

} // namespace

TEST_CASE("a seek resets a Custom Control node only when it declares an audio-safe contract",
          "[graph][control-index][rt-safety][negative-control]") {
    // Declared audio-callback-safe: the seek restarts the node.
    CHECK(resets_after_seek(true) == 1);
    // No declared contract: the reset might allocate or lock, so the audio
    // thread never calls it and the node keeps its state across the seek.
    CHECK(resets_after_seek(false) == 0);
}

namespace {

// One-port converters so a Control chain can run from an ordinary audio input
// with no Control index input and no graph-event edge, so no event lane.
CustomNodeType make_audio_to_control_type() {
    CustomNodeType type;
    type.type_id = "pulp.test.audio-to-control";
    type.num_input_ports = 1;
    type.num_output_ports = 1;
    type.input_ports = {{"audio", PortKind::Audio}};
    type.output_ports = {{"value", PortKind::Control}};
    type.process = [](pulp::audio::BufferView<float>& out,
                      const pulp::audio::BufferView<const float>& in,
                      int frames) { std::copy_n(in.channel_ptr(0), frames, out.channel_ptr(0)); };
    return type;
}

CustomNodeType make_control_to_audio_type() {
    CustomNodeType type;
    type.type_id = "pulp.test.control-to-audio";
    type.num_input_ports = 1;
    type.num_output_ports = 1;
    type.input_ports = {{"value", PortKind::Control}};
    type.output_ports = {{"audio", PortKind::Audio}};
    type.process = [](pulp::audio::BufferView<float>& out,
                      const pulp::audio::BufferView<const float>& in,
                      int frames) { std::copy_n(in.channel_ptr(0), frames, out.channel_ptr(0)); };
    return type;
}

std::vector<float> render_laneless(SignalGraph& graph, std::vector<float> values,
                                   const pulp::format::ProcessContext& transport) {
    std::vector<float> result(values.size(), -100.0f);
    const float* in_ptr[] = {values.data()};
    float* out_ptr[] = {result.data()};
    pulp::audio::BufferView<const float> input(in_ptr, 1, values.size());
    pulp::audio::BufferView<float> output(out_ptr, 1, values.size());
    graph.process(output, input, static_cast<int>(values.size()), transport);
    return result;
}

} // namespace

TEST_CASE("a seek resets a stateful Control node on a graph with no event lane",
          "[graph][control-index][negative-control]") {
    SignalGraph graph;
    const auto integrator_type = make_control_integrator_type();
    REQUIRE(graph.register_custom_node_type(integrator_type));
    REQUIRE(graph.register_custom_node_type(make_audio_to_control_type()));
    REQUIRE(graph.register_custom_node_type(make_control_to_audio_type()));
    const auto audio = graph.add_input_node(1);
    const auto to_control = graph.add_custom_node("pulp.test.audio-to-control");
    const auto integrator = graph.add_custom_node(integrator_type.type_id);
    const auto to_audio = graph.add_custom_node("pulp.test.control-to-audio");
    const auto sink = graph.add_output_node(1);
    REQUIRE((to_control != 0 && integrator != 0 && to_audio != 0));
    REQUIRE(graph.connect(audio, 0, to_control, 0));
    REQUIRE(graph.connect(graph.node(to_control)->out("value"),
                          graph.node(integrator)->in(integrator_type.input_ports.front().name)));
    REQUIRE(graph.connect(graph.node(integrator)->out("value"), graph.node(to_audio)->in("value")));
    REQUIRE(graph.connect(to_audio, 0, sink, 0));
    const std::array<float, 3> range{0.0f, 10.0f, 0.0f};
    REQUIRE(graph.set_custom_node_state(integrator, make_control_scalar_state(range)));
    REQUIRE(graph.prepare(48000, 4));
    // No Control index input and no graph-event edge: the Control-only entry
    // point is unbound, so this graph runs through process() alone.
    REQUIRE_FALSE(graph.control_index_position().bound);

    const std::vector<float> steps(4, 0.25f);
    const std::vector<float> fresh{0.25f, 0.5f, 0.75f, 1.0f};
    pulp::format::ProcessContext playing;
    playing.position_samples = 0;
    REQUIRE(render_laneless(graph, steps, playing) == fresh);
    // Negative control: an ordinary block continues the stream.
    playing.position_samples = 4;
    CHECK(render_laneless(graph, steps, playing).front() == 1.25f);

    pulp::format::ProcessContext seek;
    seek.reset_requested = true;
    seek.position_samples = 0;
    REQUIRE(seek.should_reset_stream_history());
    CHECK(render_laneless(graph, steps, seek) == fresh);
}

TEST_CASE("a negative seek on a graph with no event lane fails closed without a reset",
          "[graph][control-index][negative-control]") {
    SignalGraph graph;
    const auto integrator_type = make_control_integrator_type();
    REQUIRE(graph.register_custom_node_type(integrator_type));
    REQUIRE(graph.register_custom_node_type(make_audio_to_control_type()));
    REQUIRE(graph.register_custom_node_type(make_control_to_audio_type()));
    const auto audio = graph.add_input_node(1);
    const auto to_control = graph.add_custom_node("pulp.test.audio-to-control");
    const auto integrator = graph.add_custom_node(integrator_type.type_id);
    const auto to_audio = graph.add_custom_node("pulp.test.control-to-audio");
    const auto sink = graph.add_output_node(1);
    REQUIRE((to_control != 0 && integrator != 0 && to_audio != 0));
    REQUIRE(graph.connect(audio, 0, to_control, 0));
    REQUIRE(graph.connect(graph.node(to_control)->out("value"),
                          graph.node(integrator)->in(integrator_type.input_ports.front().name)));
    REQUIRE(graph.connect(graph.node(integrator)->out("value"), graph.node(to_audio)->in("value")));
    REQUIRE(graph.connect(to_audio, 0, sink, 0));
    const std::array<float, 3> range{0.0f, 10.0f, 0.0f};
    REQUIRE(graph.set_custom_node_state(integrator, make_control_scalar_state(range)));
    REQUIRE(graph.prepare(48000, 4));
    REQUIRE_FALSE(graph.control_index_position().bound);

    const std::vector<float> steps(4, 0.25f);
    const std::vector<float> fresh{0.25f, 0.5f, 0.75f, 1.0f};
    pulp::format::ProcessContext playing;
    playing.position_samples = 0;
    REQUIRE(render_laneless(graph, steps, playing) == fresh);
    playing.position_samples = 4;
    REQUIRE(render_laneless(graph, steps, playing).back() == 2.0f);

    // A reset to a negative position is out of range: the block is silent and
    // the integrator keeps the 2.0 it reached, so the next block continues it.
    pulp::format::ProcessContext bad_seek;
    bad_seek.reset_requested = true;
    bad_seek.position_samples = -4;
    REQUIRE(bad_seek.should_reset_stream_history());
    CHECK(render_laneless(graph, steps, bad_seek) == std::vector<float>(4, 0.0f));
    playing.position_samples = 8;
    CHECK(render_laneless(graph, steps, playing).front() == 2.25f);

    // Control: a seek to a non-negative position still restarts the node.
    pulp::format::ProcessContext seek;
    seek.reset_requested = true;
    seek.position_samples = 0;
    REQUIRE(seek.should_reset_stream_history());
    CHECK(render_laneless(graph, steps, seek) == fresh);
}

TEST_CASE("custom_node_reset_runs_on_audio_thread admits only audio-callback safety classes",
          "[graph][control-index][rt-safety]") {
    auto resets = std::make_shared<int>(0);
    auto type = make_counting_control_type(resets, true);
    CHECK(custom_node_reset_runs_on_audio_thread(type));
    for (const auto safety : {pulp::audio::RtSafetyClass::AudioCallbackSafe,
                              pulp::audio::RtSafetyClass::AudioCallbackSafeWithImmutableInputs}) {
        type.rt_contract.safety_class = safety;
        CHECK(custom_node_reset_runs_on_audio_thread(type));
    }
    for (const auto safety :
         {pulp::audio::RtSafetyClass::ControlThreadOnly,
          pulp::audio::RtSafetyClass::BackgroundThreadOnly, pulp::audio::RtSafetyClass::OfflineOnly,
          pulp::audio::RtSafetyClass::RealtimeTelemetryOnly}) {
        type.rt_contract.safety_class = safety;
        CHECK_FALSE(custom_node_reset_runs_on_audio_thread(type));
    }
    type.rt_contract.safety_class = pulp::audio::RtSafetyClass::AudioCallbackSafe;
    type.rt_contract.declared = false;
    CHECK_FALSE(custom_node_reset_runs_on_audio_thread(type));
    type.rt_contract.declared = true;
    type.reset = nullptr;
    CHECK_FALSE(custom_node_reset_runs_on_audio_thread(type));
    // Every built-in stateful Control node keeps its seek reset.
    for (const auto& builtin : {make_control_integrator_type(), make_control_slew_type(),
                                make_control_hysteresis_type(), make_control_indexed_table_type()})
        CHECK(custom_node_reset_runs_on_audio_thread(builtin));
}

TEST_CASE("Control blocks publish non-finite indices as zero and refuse negative resets",
          "[graph][control-index][negative-control]") {
    SignalGraph graph;
    REQUIRE(graph.add_control_index_input_node() != 0);
    REQUIRE(graph.prepare(48000, 4));
    std::array<float, 4> audio{1, 2, 3, 4};
    std::array<float, 4> result{9, 9, 9, 9};
    const float* in_ptr[] = {audio.data()};
    float* out_ptr[] = {result.data()};
    pulp::audio::BufferView<const float> input(in_ptr, 1, 4);
    pulp::audio::BufferView<float> output(out_ptr, 1, 4);
    // A pre-roll position is a range error, not clock exhaustion.
    const std::array<float, 4> indices{0, 1, 2, 3};
    pulp::format::ProcessContext preroll;
    preroll.reset_requested = true;
    preroll.position_samples = -64;
    CHECK(graph.process_control_checked(output, input, {2, 0, indices}, &preroll) ==
          ControlProcessResult::OutOfRange);
    CHECK((result == std::array<float, 4>{9, 9, 9, 9}));
    CHECK(graph.control_index_position().epoch == 1);
    CHECK(graph.process_control_checked(output, input, {1, 0, indices}) ==
          ControlProcessResult::Accepted);
}

TEST_CASE("map-clip and hysteresis nodes treat non-finite input like their pure stages",
          "[graph][control-index][negative-control]") {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    const std::array<float, 4> map{-1.0f, 1.0f, 100.0f, 200.0f};
    // Zero lies mid-interval, so substituting zero would give 150.
    const auto mapped =
        run_unary_node(make_control_map_clip_type(), make_control_scalar_state(map), {nan, inf});
    CHECK(mapped == std::vector<float>{pulp::signal::map_clip(nan, -1.0f, 1.0f, 100.0f, 200.0f),
                                       pulp::signal::map_clip(inf, -1.0f, 1.0f, 100.0f, 200.0f)});
    CHECK(mapped.front() == 100.0f);

    // High at 0.8, then non-finite samples: the pure step holds the prior
    // state, and so does the node; substituting zero would release it.
    const std::vector<float> held{0.8f, nan, -inf, inf, 0.2f};
    const auto rendered = run_unary_node(make_control_hysteresis_type(), {}, held);
    bool prior = false;
    std::vector<float> expected;
    for (const float sample : held) {
        prior = pulp::signal::hysteresis_step(sample, 0.5f, 0.25f, prior);
        expected.push_back(prior ? 1.0f : 0.0f);
    }
    CHECK(expected == std::vector<float>{1, 1, 1, 1, 0});
    CHECK(rendered == expected);
}

TEST_CASE("built-in Control nodes declare an allocation-free realtime contract",
          "[graph][control-index][rt-safety]") {
    for (const auto& type :
         {make_control_min_type(), make_control_max_type(), make_control_safe_divide_type(),
          make_control_map_clip_type(), make_control_hysteresis_type(),
          make_control_integrator_type(), make_control_slew_type(),
          make_control_indexed_table_type(), make_control_index_input_type()}) {
        INFO(type.type_id);
        CHECK(type.rt_contract.declared);
        CHECK(type.rt_contract.safety_class ==
              pulp::audio::RtSafetyClass::AudioCallbackSafeAfterPrepare);
        CHECK(type.rt_contract.allocation_free);
        CHECK_FALSE(type.rt_contract.loop_bound.empty());
    }
}

TEST_CASE("a non-finite Control index never reaches a Control consumer",
          "[graph][control-index][negative-control]") {
    // Pass-through node: forwards its Control input unchanged, so the probe
    // output is exactly the index the graph published.
    CustomNodeType pass;
    pass.type_id = "pulp.test.control-pass";
    pass.num_input_ports = 1;
    pass.num_output_ports = 1;
    pass.input_ports = {{"value", PortKind::Control}};
    pass.output_ports = {{"value", PortKind::Control}};
    pass.process = [](pulp::audio::BufferView<float>& out,
                      const pulp::audio::BufferView<const float>& in,
                      int frames) { std::copy_n(in.channel_ptr(0), frames, out.channel_ptr(0)); };
    SignalGraph graph;
    build_control_node_graph(graph, pass);
    REQUIRE(graph.prepare(48000, 4));
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    CHECK(render_control_node_graph(graph, {0.5f, nan, inf, -inf}) ==
          std::vector<float>{0.5f, 0.0f, 0.0f, 0.0f});
}
