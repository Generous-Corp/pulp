#include <pulp/host/graph_rt_report.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <cstddef>
#include <string_view>

namespace pulp::host {
namespace {

using signal::rt::RtWorkCounts;

constexpr int kMaxMeasureChannels = 64;
constexpr int kMaxMeasureBlocks = 4096;

std::string_view node_kind_name(NodeType type) noexcept {
    switch (type) {
    case NodeType::AudioInput:
        return "audio_input";
    case NodeType::AudioOutput:
        return "audio_output";
    case NodeType::Plugin:
        return "plugin";
    case NodeType::Gain:
        return "gain";
    case NodeType::MidiInput:
        return "midi_input";
    case NodeType::MidiOutput:
        return "midi_output";
    case NodeType::Custom:
        return "custom";
    case NodeType::SpectralAnalysis:
        return "spectral_analysis";
    case NodeType::SpectralStage:
        return "spectral_stage";
    case NodeType::SpectralSynthesis:
        return "spectral_synthesis";
    }
    return "unknown";
}

bool exceeds(const RtWorkCounts& value, const RtWorkCounts& budget) noexcept {
    return value.fft > budget.fft || value.trig > budget.trig || value.bins > budget.bins;
}

std::string describe(const GraphRtNodeReport& node) {
    return "node " + std::to_string(node.node_id) + " '" + node.name + "'";
}

void fill_contract(const SignalGraph& graph, const GraphNode& node, int block_size,
                   GraphRtNodeReport& out) {
    switch (node.type) {
    case NodeType::AudioInput:
    case NodeType::AudioOutput:
    case NodeType::Gain:
    case NodeType::MidiInput:
    case NodeType::MidiOutput:
        // Bounded copies, a scalar gain, and pre-sized MIDI buffers;
        // allocation- and lock-freedom are pinned by the SignalGraph
        // realtime-safety test suite.
        out.contract_source = GraphRtContractSource::Framework;
        out.safety_class = audio::RtSafetyClass::AudioCallbackSafeAfterPrepare;
        out.allocation = GraphRtAllocationStatus::Proven;
        out.declared_worst_case_work = RtWorkCounts{};
        out.loop_bound = "one pass over block_size frames per port";
        return;
    case NodeType::SpectralAnalysis:
    case NodeType::SpectralStage:
    case NodeType::SpectralSynthesis:
        // Prepared FFT framing; the counted transform cost depends on the
        // hop and is visible in the measured pass, not declared here.
        out.contract_source = GraphRtContractSource::Framework;
        out.safety_class = audio::RtSafetyClass::AudioCallbackSafeAfterPrepare;
        out.allocation = GraphRtAllocationStatus::Declared;
        out.loop_bound = "ceil(block_size / hop) frames per block";
        return;
    case NodeType::Plugin:
        out.contract_source = GraphRtContractSource::External;
        return;
    case NodeType::Custom:
        break;
    }

    const auto* type = graph.custom_node_type(node.custom_type_id, node.custom_type_version);
    if (type == nullptr || !type->rt_contract.declared) {
        out.contract_source = GraphRtContractSource::Undeclared;
        return;
    }
    const auto& contract = type->rt_contract;
    out.contract_source = GraphRtContractSource::Declared;
    out.safety_class = contract.safety_class;
    out.allocation = contract.allocation_free ? GraphRtAllocationStatus::Declared
                                              : GraphRtAllocationStatus::Unproven;
    if (contract.worst_case_work)
        out.declared_worst_case_work = contract.worst_case_work(block_size);
    out.loop_bound = contract.loop_bound;
}

void evaluate(GraphRtReport& report, const GraphRtReportOptions& options) {
    report.reasons.clear();
    bool fails = false;
    bool unknown = false;
    if (!report.prepared) {
        report.verdict = GraphRtVerdict::Unknown;
        report.reasons.emplace_back("graph is not prepared");
        return;
    }
    if (report.block_size <= 0 || report.block_size > report.prepared_max_block_size) {
        fails = true;
        report.reasons.push_back("block size " + std::to_string(report.block_size) +
                                 " is outside the prepared range 1.." +
                                 std::to_string(report.prepared_max_block_size));
    }
    for (const auto& node : report.nodes) {
        switch (node.contract_source) {
        case GraphRtContractSource::External:
            unknown = true;
            report.reasons.push_back(describe(node) +
                                     " is a hosted plugin with no realtime contract");
            continue;
        case GraphRtContractSource::Undeclared:
            unknown = true;
            report.reasons.push_back(describe(node) + " declares no realtime contract");
            continue;
        case GraphRtContractSource::Framework:
        case GraphRtContractSource::Declared:
            break;
        }
        if (!node.safety_class || !audio::rt_safety_class_is_audio_dsp_safe(*node.safety_class)) {
            fails = true;
            report.reasons.push_back(
                describe(node) + " is declared " +
                std::string(node.safety_class ? audio::rt_safety_class_name(*node.safety_class)
                                              : "unclassified"));
        }
        if (node.allocation == GraphRtAllocationStatus::Unproven) {
            fails = true;
            report.reasons.push_back(describe(node) + " does not declare allocation-free");
        }
    }
    if (options.work_budget_per_block) {
        const auto& budget = *options.work_budget_per_block;
        if (report.declared_worst_case_work && exceeds(*report.declared_worst_case_work, budget)) {
            fails = true;
            report.reasons.emplace_back("declared worst-case work exceeds the per-block budget");
        }
        if (report.measured && exceeds(report.measured->worst_block, budget)) {
            fails = true;
            report.reasons.emplace_back("measured worst-case work exceeds the per-block budget");
        }
    }
    if (report.measured && report.measured->allocations && *report.measured->allocations > 0) {
        fails = true;
        report.reasons.push_back("measured pass allocated " +
                                 std::to_string(*report.measured->allocations) + " time(s)");
    }
    report.verdict = fails     ? GraphRtVerdict::DoesNotHold
                     : unknown ? GraphRtVerdict::Unknown
                               : GraphRtVerdict::Holds;
}

choc::value::Value work_value(const RtWorkCounts& counts) {
    auto value = choc::value::createObject("RtWork");
    value.addMember("fft", static_cast<std::int64_t>(counts.fft));
    value.addMember("trig", static_cast<std::int64_t>(counts.trig));
    value.addMember("bins", static_cast<std::int64_t>(counts.bins));
    return value;
}

choc::value::Value int_array(const std::vector<std::int64_t>& values) {
    auto array = choc::value::createEmptyArray();
    for (const auto value : values)
        array.addArrayElement(value);
    return array;
}

} // namespace

std::string_view graph_rt_verdict_name(GraphRtVerdict verdict) noexcept {
    switch (verdict) {
    case GraphRtVerdict::Holds:
        return "holds";
    case GraphRtVerdict::DoesNotHold:
        return "does_not_hold";
    case GraphRtVerdict::Unknown:
        return "unknown";
    }
    return "unknown";
}

std::string_view graph_rt_contract_source_name(GraphRtContractSource source) noexcept {
    switch (source) {
    case GraphRtContractSource::Framework:
        return "framework";
    case GraphRtContractSource::Declared:
        return "declared";
    case GraphRtContractSource::Undeclared:
        return "undeclared";
    case GraphRtContractSource::External:
        return "external";
    }
    return "undeclared";
}

std::string_view graph_rt_allocation_status_name(GraphRtAllocationStatus status) noexcept {
    switch (status) {
    case GraphRtAllocationStatus::Proven:
        return "proven";
    case GraphRtAllocationStatus::Declared:
        return "declared";
    case GraphRtAllocationStatus::Unproven:
        return "unproven";
    }
    return "unproven";
}

GraphRtReport build_graph_rt_report(const SignalGraph& graph, const GraphRtReportOptions& options) {
    GraphRtReport report;
    const auto latency = graph.prepared_latency_snapshot();
    report.prepared = latency.has_value();
    if (!latency) {
        evaluate(report, options);
        return report;
    }
    report.sample_rate = latency->sample_rate;
    report.prepared_max_block_size = latency->max_block_size;
    report.block_size = options.block_size > 0 ? options.block_size : latency->max_block_size;
    report.total_latency_samples = latency->total_latency_samples;
    for (const auto& edge : latency->connections) {
        if (edge.delay_samples > 0) {
            ++report.delayed_connections;
            report.compensation_delay_samples += edge.delay_samples;
        }
    }

    RtWorkCounts declared_sum{};
    bool declared_complete = true;
    // Snapshot order is the deterministic processing order.
    for (const auto& entry : latency->nodes) {
        const auto* node = graph.node(entry.node_id);
        if (node == nullptr)
            continue;
        GraphRtNodeReport out;
        out.node_id = node->id;
        out.name = node->name;
        out.kind = std::string(node_kind_name(node->type));
        out.custom_type_id = node->custom_type_id;
        out.custom_type_version = node->type == NodeType::Custom ? node->custom_type_version : 0;
        out.artifact_latency_samples = entry.artifact_latency_samples;
        out.intentional_delay_samples = entry.intentional_delay_samples;
        out.output_port_latency_samples = entry.output_port_latency_samples;
        out.input_latency_samples = entry.input_latency_samples;
        out.output_latency_samples = entry.output_latency_samples;
        fill_contract(graph, *node, report.block_size, out);
        report.total_intentional_delay_samples += out.intentional_delay_samples;
        if (out.declared_worst_case_work)
            declared_sum = declared_sum + *out.declared_worst_case_work;
        else
            declared_complete = false;
        report.nodes.push_back(std::move(out));
    }
    if (declared_complete)
        report.declared_worst_case_work = declared_sum;
    evaluate(report, options);
    return report;
}

std::optional<GraphRtMeasurement> measure_graph_rt_work(SignalGraph& graph,
                                                        const GraphRtMeasureOptions& options) {
    const auto stats = graph.prepared_stats();
    if (!graph.is_prepared() || stats.max_block_size <= 0)
        return std::nullopt;
    const int block = options.block_size > 0 ? options.block_size : stats.max_block_size;
    if (block > stats.max_block_size || options.blocks <= 0 || options.blocks > kMaxMeasureBlocks)
        return std::nullopt;

    int in_channels = 1;
    int out_channels = 1;
    for (const auto& node : graph.nodes()) {
        if (node.type == NodeType::AudioInput)
            in_channels = std::max(in_channels, node.num_output_ports);
        if (node.type == NodeType::AudioOutput)
            out_channels = std::max(out_channels, node.num_input_ports);
    }
    in_channels = std::min(in_channels, kMaxMeasureChannels);
    out_channels = std::min(out_channels, kMaxMeasureChannels);

    const auto frames = static_cast<std::size_t>(block);
    std::vector<float> in_storage(frames * static_cast<std::size_t>(in_channels), 0.0f);
    std::vector<float> out_storage(frames * static_cast<std::size_t>(out_channels), 0.0f);
    std::vector<const float*> in_ptrs(static_cast<std::size_t>(in_channels));
    std::vector<float*> out_ptrs(static_cast<std::size_t>(out_channels));
    for (int c = 0; c < in_channels; ++c)
        in_ptrs[static_cast<std::size_t>(c)] =
            in_storage.data() + frames * static_cast<std::size_t>(c);
    for (int c = 0; c < out_channels; ++c)
        out_ptrs[static_cast<std::size_t>(c)] =
            out_storage.data() + frames * static_cast<std::size_t>(c);
    audio::BufferView<const float> input(in_ptrs.data(), in_ptrs.size(), frames);
    audio::BufferView<float> output(out_ptrs.data(), out_ptrs.size(), frames);

    GraphRtMeasurement measured;
    measured.work_counters_enabled = signal::rt::kWorkCountersEnabled;
    measured.blocks = options.blocks;
    const std::uint64_t allocations_before =
        options.allocation_count ? options.allocation_count() : 0;
    for (int b = 0; b < options.blocks; ++b) {
        // An impulse in the first block exercises every path once; later
        // blocks are silent, which is where idle-path bursts would show.
        for (int c = 0; c < in_channels; ++c)
            in_storage[frames * static_cast<std::size_t>(c)] = b == 0 ? 1.0f : 0.0f;
        signal::rt::RtWorkCounter counter;
        graph.process(output, input, block);
        const auto delta = counter.delta();
        measured.total = measured.total + delta;
        measured.worst_block.fft = std::max(measured.worst_block.fft, delta.fft);
        measured.worst_block.trig = std::max(measured.worst_block.trig, delta.trig);
        measured.worst_block.bins = std::max(measured.worst_block.bins, delta.bins);
    }
    if (options.allocation_count)
        measured.allocations = options.allocation_count() - allocations_before;
    return measured;
}

void apply_graph_rt_measurement(GraphRtReport& report, const GraphRtMeasurement& measured,
                                const GraphRtReportOptions& options) {
    report.measured = measured;
    evaluate(report, options);
}

std::string graph_rt_report_to_json(const GraphRtReport& report) {
    auto root = choc::value::createObject("GraphRtReport");
    root.addMember("schema", "pulp.graph.rt-report");
    root.addMember("schema_version", 1);
    root.addMember("prepared", report.prepared);
    root.addMember("sample_rate", report.sample_rate);
    root.addMember("prepared_max_block_size", report.prepared_max_block_size);
    root.addMember("block_size", report.block_size);
    root.addMember("verdict", std::string(graph_rt_verdict_name(report.verdict)));
    auto reasons = choc::value::createEmptyArray();
    for (const auto& reason : report.reasons)
        reasons.addArrayElement(reason);
    root.addMember("reasons", reasons);

    auto latency = choc::value::createObject("Latency");
    latency.addMember("total_latency_samples", report.total_latency_samples);
    latency.addMember("total_intentional_delay_samples", report.total_intentional_delay_samples);
    latency.addMember("delayed_connections", report.delayed_connections);
    latency.addMember("compensation_delay_samples", report.compensation_delay_samples);
    root.addMember("latency", latency);

    if (report.declared_worst_case_work)
        root.addMember("declared_worst_case_work", work_value(*report.declared_worst_case_work));
    else
        root.addMember("declared_worst_case_work", choc::value::Value{});

    if (report.measured) {
        const auto& m = *report.measured;
        auto measured = choc::value::createObject("Measured");
        measured.addMember("work_counters_enabled", m.work_counters_enabled);
        measured.addMember("blocks", m.blocks);
        measured.addMember("worst_block", work_value(m.worst_block));
        measured.addMember("total", work_value(m.total));
        if (m.allocations)
            measured.addMember("allocations", static_cast<std::int64_t>(*m.allocations));
        else
            measured.addMember("allocations", choc::value::Value{});
        root.addMember("measured", measured);
    } else {
        root.addMember("measured", choc::value::Value{});
    }

    auto nodes = choc::value::createEmptyArray();
    for (const auto& node : report.nodes) {
        auto value = choc::value::createObject("Node");
        value.addMember("id", static_cast<std::int64_t>(node.node_id));
        value.addMember("name", node.name);
        value.addMember("kind", node.kind);
        if (!node.custom_type_id.empty()) {
            value.addMember("custom_type_id", node.custom_type_id);
            value.addMember("custom_type_version", node.custom_type_version);
        }
        value.addMember("contract",
                        std::string(graph_rt_contract_source_name(node.contract_source)));
        if (node.safety_class)
            value.addMember("rt_safety_class",
                            std::string(audio::rt_safety_class_name(*node.safety_class)));
        else
            value.addMember("rt_safety_class", choc::value::Value{});
        value.addMember("allocation",
                        std::string(graph_rt_allocation_status_name(node.allocation)));
        if (node.declared_worst_case_work)
            value.addMember("declared_worst_case_work", work_value(*node.declared_worst_case_work));
        else
            value.addMember("declared_worst_case_work", choc::value::Value{});
        value.addMember("loop_bound", node.loop_bound);
        value.addMember("artifact_latency_samples", node.artifact_latency_samples);
        value.addMember("intentional_delay_samples", node.intentional_delay_samples);
        value.addMember("output_port_latency_samples", int_array(node.output_port_latency_samples));
        value.addMember("input_latency_samples", node.input_latency_samples);
        value.addMember("output_latency_samples", node.output_latency_samples);
        nodes.addArrayElement(value);
    }
    root.addMember("nodes", nodes);
    return choc::json::toString(root);
}

} // namespace pulp::host
