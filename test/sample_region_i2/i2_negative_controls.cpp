// I2 deliberately-mutated controls.  These are intentionally kept outside the
// production test translation units: each case runs the paired acceptance
// oracle against one small, executable bad implementation.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <pulp/host/graph_serializer.hpp>
#include <pulp/host/sample_region_authoring.hpp>
#include <pulp/host/sample_region_proof.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/host/signal_graph_prepared_topology_edit.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace pulp::host;

namespace {

constexpr float kCoefficient = 0.5f;

void combinational_process(void*, const PreparedSampleKernelConfig&, const SampleFrameContext&,
                           const float* input, float* output) noexcept {
    output[0] = input[0];
}

bool delay_construct(void* state, const SampleKernelPrepareContext&) noexcept {
    *static_cast<float*>(state) = 0.0f;
    return true;
}

void delay_reset(void* state) noexcept {
    *static_cast<float*>(state) = 0.0f;
}
void delay_destroy(void*) noexcept {}
void delay_publish(const void* state, const PreparedSampleKernelConfig&, float* output) noexcept {
    output[0] = *static_cast<const float*>(state);
}
void delay_commit(void* state, const PreparedSampleKernelConfig&, const float* input) noexcept {
    *static_cast<float*>(state) = input[0];
}

std::vector<float> impulse(std::size_t count) {
    std::vector<float> value(count, 0.0f);
    value.front() = 1.0f;
    return value;
}

std::vector<float> allpass_oracle(std::span<const float> input) {
    std::vector<float> output(input.size(), 0.0f);
    float previous_input = 0.0f;
    float previous_output = 0.0f;
    for (std::size_t i = 0; i < input.size(); ++i) {
        output[i] = kCoefficient * input[i] + previous_input - kCoefficient * previous_output;
        previous_input = input[i];
        previous_output = output[i];
    }
    return output;
}

// NEG-01: the state is read once per callback and therefore remains the
// previous callback's value for every sample in the callback.
std::vector<float> previous_callback_feedback(std::span<const float> input,
                                              std::span<const int> schedule) {
    std::vector<float> output(input.size(), 0.0f);
    float previous_input = 0.0f;
    float previous_output = 0.0f;
    std::size_t offset = 0;
    for (std::size_t block = 0; offset < input.size(); ++block) {
        const auto count =
            std::min<std::size_t>(schedule[block % schedule.size()], input.size() - offset);
        const auto callback_input = previous_input;
        const auto callback_output = previous_output;
        for (std::size_t i = 0; i < count; ++i)
            output[offset + i] =
                kCoefficient * input[offset + i] + callback_input - kCoefficient * callback_output;
        previous_input = input[offset + count - 1];
        previous_output = output[offset + count - 1];
        offset += count;
    }
    return output;
}

std::vector<float> reset_each_callback(std::span<const float> input,
                                       std::span<const int> schedule) {
    std::vector<float> output(input.size(), 0.0f);
    std::size_t offset = 0;
    for (std::size_t block = 0; offset < input.size(); ++block) {
        const auto count =
            std::min<std::size_t>(schedule[block % schedule.size()], input.size() - offset);
        float previous_input = 0.0f;
        float previous_output = 0.0f;
        for (std::size_t i = 0; i < count; ++i) {
            output[offset + i] =
                kCoefficient * input[offset + i] + previous_input - kCoefficient * previous_output;
            previous_input = input[offset + i];
            previous_output = output[offset + i];
        }
        offset += count;
    }
    return output;
}

bool partition_gate(std::span<const float> left, std::span<const float> right) {
    if (left.size() != right.size())
        return false;
    return std::equal(left.begin(), left.end(), right.begin(),
                      [](float a, float b) { return std::abs(a - b) <= 1.0e-6f; });
}

SampleKernelDescriptor scalar(std::string id, std::uint32_t inputs,
                              SampleKernelConfigKind config_kind) {
    SampleKernelDescriptor descriptor;
    descriptor.type_id = std::move(id);
    descriptor.num_input_ports = inputs;
    descriptor.num_output_ports = 1;
    descriptor.authored_config_kind = config_kind;
    descriptor.process = combinational_process;
    return descriptor;
}

SampleKernelDescriptor delay_descriptor() {
    auto descriptor = scalar("pulp.core.unit-delay", 1, SampleKernelConfigKind::None);
    descriptor.causality = SampleKernelCausality::OneSampleDelay;
    descriptor.process = nullptr;
    descriptor.state_size = sizeof(float);
    descriptor.state_alignment = alignof(float);
    descriptor.construct = delay_construct;
    descriptor.reset = delay_reset;
    descriptor.destroy = delay_destroy;
    descriptor.delay_publish = delay_publish;
    descriptor.delay_commit = delay_commit;
    return descriptor;
}

const SampleKernelDescriptor* resolve_kernel(const void*, std::string_view type_id,
                                             int version) noexcept {
    static const std::array descriptors = {
        scalar("pulp.core.sample-region.input", 1, SampleKernelConfigKind::BoundaryIndex),
        scalar("pulp.core.sample-region.output", 1, SampleKernelConfigKind::BoundaryIndex),
        scalar("pulp.core.sample-region.add", 2, SampleKernelConfigKind::None),
        scalar("pulp.core.sample-region.parameter", 0, SampleKernelConfigKind::PromotedParameterId),
        delay_descriptor(),
    };
    const auto found = std::find_if(descriptors.begin(), descriptors.end(), [&](const auto& value) {
        return value.type_id == type_id && value.version == version;
    });
    return found == descriptors.end() ? nullptr : &*found;
}

SampleRegionKernelRegistryView registry() {
    return {nullptr, resolve_kernel};
}

SampleRegionKernelNode member(NodeId node, std::string type, SampleKernelConfig config) {
    return {node, std::move(type), 1, config};
}

SampleKernelConfig config(SampleKernelConfigKind kind, std::uint32_t value = 0) {
    return {kind, value, 0.0f};
}

SampleRegionCandidate delayed_cycle() {
    SampleRegionCandidate candidate;
    candidate.region_id = 9;
    candidate.registry = registry();
    candidate.max_block_size = 64;
    candidate.members = {
        member(1, "pulp.core.sample-region.input", config(SampleKernelConfigKind::BoundaryIndex)),
        member(2, "pulp.core.sample-region.add", config(SampleKernelConfigKind::None)),
        member(3, "pulp.core.unit-delay", config(SampleKernelConfigKind::None)),
        member(4, "pulp.core.sample-region.output", config(SampleKernelConfigKind::BoundaryIndex)),
    };
    candidate.connections = {
        {100, 0, 1, 0}, {1, 0, 2, 0}, {3, 0, 2, 1}, {2, 0, 3, 0}, {2, 0, 4, 0}, {4, 0, 101, 0},
    };
    return candidate;
}

SampleRegionCandidate delayed_cycle_with_parameter() {
    auto candidate = delayed_cycle();
    candidate.members.push_back(member(5, "pulp.core.sample-region.parameter",
                                       config(SampleKernelConfigKind::PromotedParameterId, 29)));
    candidate.members.push_back(
        member(6, "pulp.core.sample-region.add", config(SampleKernelConfigKind::None)));
    candidate.promoted_parameters = {29};
    candidate.connections.erase(candidate.connections.begin() + 4);
    candidate.connections.insert(candidate.connections.begin() + 4,
                                 {{2, 0, 6, 0}, {5, 0, 6, 1}, {6, 0, 4, 0}});
    return candidate;
}

std::string omit_region_metadata(std::string json) {
    const auto key = json.find("\"sample_regions\"");
    REQUIRE(key != std::string::npos);
    const auto open = json.find('[', key);
    REQUIRE(open != std::string::npos);
    int depth = 0;
    std::size_t close = std::string::npos;
    for (std::size_t i = open; i < json.size(); ++i) {
        if (json[i] == '[')
            ++depth;
        if (json[i] == ']' && --depth == 0) {
            close = i;
            break;
        }
    }
    REQUIRE(close != std::string::npos);
    auto begin = key;
    if (begin > 0 && json[begin - 1] == ',')
        --begin;
    json.erase(begin, close + 1 - begin);
    return json;
}

struct PersistedRegion {
    SignalGraph graph;
    std::unique_ptr<SampleRegionParameterOwner> parameters;
    NodeId region_input = 0;
    NodeId delay = 0;
    NodeId region_output = 0;

    PersistedRegion() {
        const auto input = graph.add_input_node(1, "Input");
        const auto output = graph.add_output_node(1, "Output");
        REQUIRE(graph.connect(input, 0, output, 0));
        REQUIRE(graph.prepare(48000.0, 16));
        auto edit = graph.begin_prepared_topology_edit();
        REQUIRE(edit);
        REQUIRE(edit->disconnect(input, 0, output, 0));
        REQUIRE(register_builtin_sample_region_types(*edit));
        region_input = edit->add_custom_node("pulp.core.sample-region.input");
        delay = edit->add_custom_node("pulp.core.unit-delay");
        region_output = edit->add_custom_node("pulp.core.sample-region.output");
        REQUIRE(edit->connect(input, 0, region_input, 0));
        REQUIRE(edit->connect(region_input, 0, delay, 0));
        REQUIRE(edit->connect(delay, 0, region_output, 0));
        REQUIRE(edit->connect(region_output, 0, output, 0));
        SampleRegionDefinition definition;
        definition.region_id = 901;
        definition.members = {
            {region_input, "pulp.core.sample-region.input", 1,
             config(SampleKernelConfigKind::BoundaryIndex)},
            {delay, "pulp.core.unit-delay", 1, config(SampleKernelConfigKind::None)},
            {region_output, "pulp.core.sample-region.output", 1,
             config(SampleKernelConfigKind::BoundaryIndex)},
        };
        definition.input_boundaries = {region_input};
        definition.output_boundaries = {region_output};
        REQUIRE(edit->declare_sample_region(definition).accepted);
        REQUIRE(edit->prove_sample_region(definition.region_id).accepted);
        parameters =
            SampleRegionParameterOwner::create({}, edit->sample_region_parameter_contract());
        REQUIRE(parameters);
        REQUIRE(edit->bind_sample_region_parameters(parameters->binding()).accepted);
        REQUIRE(edit->prepare(48000.0, 16) == SignalGraph::PreparedTopologyEdit::Result::Prepared);
        REQUIRE(edit->commit() == SignalGraph::PreparedTopologyEdit::Result::Committed);
    }
};

} // namespace

TEST_CASE("NEG-01 previous-callback feedback breaks partition invariance",
          "[sample-region][i2][negative][NEG-01]") {
    const auto input = impulse(257);
    const std::array one{1};
    const std::array irregular{1, 127, 3, 64, 17, 45};
    const auto one_sample = previous_callback_feedback(input, one);
    const auto mutant = previous_callback_feedback(input, irregular);
    REQUIRE_FALSE(partition_gate(one_sample, mutant));
    REQUIRE(partition_gate(allpass_oracle(input), allpass_oracle(input)));
}

TEST_CASE("NEG-02 callback reset breaks irregular partition invariance",
          "[sample-region][i2][negative][NEG-02]") {
    auto input = impulse(257);
    input[0] = 0.0f;
    input[2] = 1.0f;
    const std::array one{1};
    const std::array irregular{1, 127, 3, 64, 17, 45};
    const auto one_sample = reset_each_callback(input, one);
    const auto mutant = reset_each_callback(input, irregular);
    REQUIRE_FALSE(partition_gate(one_sample, mutant));
}

TEST_CASE("NEG-05 graph metadata omission fails the v3 load gate",
          "[sample-region][i2][negative][NEG-05]") {
    PersistedRegion fixture;
    const auto canonical = GraphSerializer::to_json(fixture.graph);
    const auto mutant = omit_region_metadata(canonical);
    SignalGraph loaded;
    REQUIRE(register_builtin_sample_region_types(loaded));
    const auto result = GraphSerializer::from_json(loaded, mutant);
    REQUIRE_FALSE(result.ok);
}

std::vector<float> pdc_transform(std::span<const float> response, int reported_latency) {
    std::vector<float> shifted(response.size(), 0.0f);
    if (reported_latency < 0)
        return shifted;
    for (std::size_t i = static_cast<std::size_t>(reported_latency); i < response.size(); ++i)
        shifted[i] = response[i - static_cast<std::size_t>(reported_latency)];
    return shifted;
}

bool direct_term_gate(std::span<const float> response) {
    return response.size() >= 2 && std::abs(response[0] - 0.5f) <= 1.0e-6f &&
           std::abs(response[1] - 0.75f) <= 1.0e-6f;
}

TEST_CASE("NEG-06 a one-sample PDC report moves the allpass direct term",
          "[sample-region][i2][negative][NEG-06]") {
    const auto response = allpass_oracle(impulse(8));
    REQUIRE(direct_term_gate(pdc_transform(response, 0)));
    // Deliberate mutant: UnitDelay is incorrectly reported as compensatable
    // latency, so the host's PDC transform moves the direct allpass term.
    REQUIRE_FALSE(direct_term_gate(pdc_transform(response, 1)));
}

struct IndependentReceipt {
    SampleRegionResourceStats resources;
};

IndependentReceipt independent_receipt(const SampleRegionCandidate& candidate) {
    IndependentReceipt receipt;
    receipt.resources.member_nodes = static_cast<std::uint32_t>(candidate.members.size());
    const auto is_member = [&](NodeId id) {
        return std::any_of(candidate.members.begin(), candidate.members.end(),
                           [id](const auto& member) { return member.node == id; });
    };
    receipt.resources.internal_connections = static_cast<std::uint32_t>(std::count_if(
        candidate.connections.begin(), candidate.connections.end(),
        [&](const auto& edge) { return is_member(edge.source) && is_member(edge.destination); }));
    receipt.resources.input_boundaries = 1;
    receipt.resources.output_boundaries = 1;
    receipt.resources.delay_nodes = static_cast<std::uint32_t>(
        std::count_if(candidate.members.begin(), candidate.members.end(),
                      [](const auto& member) { return member.type_id == "pulp.core.unit-delay"; }));
    receipt.resources.promoted_parameters =
        static_cast<std::uint32_t>(candidate.promoted_parameters.size());
    receipt.resources.state_bytes = receipt.resources.delay_nodes * sizeof(float);
    receipt.resources.state_alignment = alignof(float);
    receipt.resources.logical_boundary_bytes =
        (receipt.resources.input_boundaries + receipt.resources.output_boundaries) *
        candidate.max_block_size * sizeof(float);
    receipt.resources.work_per_frame =
        receipt.resources.member_nodes + receipt.resources.internal_connections +
        receipt.resources.delay_nodes + receipt.resources.input_boundaries +
        receipt.resources.output_boundaries;
    receipt.resources.work_per_block = receipt.resources.work_per_frame * candidate.max_block_size;
    return receipt;
}

bool accounting_gate(const SampleRegionProof& proof, const IndependentReceipt& receipt) {
    return proof.accepted && proof.resources.member_nodes == receipt.resources.member_nodes &&
           proof.resources.internal_connections == receipt.resources.internal_connections &&
           proof.resources.input_boundaries == receipt.resources.input_boundaries &&
           proof.resources.output_boundaries == receipt.resources.output_boundaries &&
           proof.resources.promoted_parameters == receipt.resources.promoted_parameters &&
           proof.resources.delay_nodes == receipt.resources.delay_nodes &&
           proof.resources.state_bytes == receipt.resources.state_bytes &&
           proof.resources.state_alignment == receipt.resources.state_alignment &&
           proof.resources.logical_boundary_bytes == receipt.resources.logical_boundary_bytes &&
           proof.resources.work_per_frame == receipt.resources.work_per_frame &&
           proof.resources.work_per_block == receipt.resources.work_per_block;
}

TEST_CASE("NEG-07 corrupted resource accounting fails its identity gate",
          "[sample-region][i2][negative][NEG-07]") {
    // The existing planner suite owns exhaustive exact/one-over authored and
    // parser ceilings.  This I2-only control adds the missing planted mutation
    // for every derived resource field without duplicating those tests.
    const auto candidate = delayed_cycle_with_parameter();
    const auto proof = prove_sample_region(candidate);
    const auto receipt = independent_receipt(candidate);
    REQUIRE(accounting_gate(proof, receipt));
    const auto corrupt_receipt = [&](auto mutate) {
        auto mutant = receipt;
        mutate(mutant.resources);
        REQUIRE_FALSE(accounting_gate(proof, mutant));
    };
    corrupt_receipt([](auto& value) { ++value.member_nodes; });
    corrupt_receipt([](auto& value) { ++value.internal_connections; });
    corrupt_receipt([](auto& value) { ++value.input_boundaries; });
    corrupt_receipt([](auto& value) { ++value.output_boundaries; });
    corrupt_receipt([](auto& value) { ++value.promoted_parameters; });
    corrupt_receipt([](auto& value) { ++value.delay_nodes; });
    corrupt_receipt([](auto& value) { ++value.state_bytes; });
    corrupt_receipt([](auto& value) { ++value.state_alignment; });
    corrupt_receipt([](auto& value) { ++value.logical_boundary_bytes; });
    corrupt_receipt([](auto& value) { ++value.work_per_frame; });
    corrupt_receipt([](auto& value) { ++value.work_per_block; });
}
