#include <catch2/catch_test_macros.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/host/processor_signal_graph_binding.hpp>
#include <pulp/host/signal_graph_control.hpp>
#include <pulp/inspect/control_signal_graph_executor.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace {
class RouteProcessor final : public pulp::format::Processor {
  public:
    static constexpr std::uint32_t kParam = 7;
    pulp::format::PluginDescriptor descriptor() const override {
        pulp::format::PluginDescriptor d;
        d.name = "DSPX route authority";
        d.manufacturer = "Pulp";
        d.bundle_id = "com.pulp.test.dspx-route";
        d.version = "1";
        d.category = pulp::format::PluginCategory::Effect;
        d.input_buses = {{"In", 1, false}};
        d.output_buses = {{"Out", 1, false}};
        d.node_capabilities.consumes_audio_rate_modulations = true;
        return d;
    }
    void define_parameters(pulp::state::StateStore& store) override {
        pulp::state::ParamInfo p;
        p.id = kParam;
        p.name = "Route";
        p.range = {0.0f, 1.0f, 0.5f};
        p.rate = pulp::state::ParamRate::AudioRate;
        p.modulatable = true;
        store.add_parameter(p);
    }
    void prepare(const pulp::format::PrepareContext&) override {}

    void process(pulp::audio::BufferView<float>& output,
                 const pulp::audio::BufferView<const float>&, pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&, const pulp::format::ProcessContext&) override {
        for (std::size_t c = 0; c < output.num_channels(); ++c)
            std::fill_n(output.channel_ptr(c), output.num_samples(), 0.0f);
    }
};

void prepare_graph(pulp::host::SignalGraph& graph, pulp::host::NodeId& source,
                   pulp::host::NodeId& destination) {
    source = graph.add_input_node(1, "source");
    destination = graph.add_processor_node(std::make_unique<RouteProcessor>());
    const auto output = graph.add_output_node(1, "output");
    REQUIRE(source != 0);
    REQUIRE(destination != 0);
    REQUIRE(output != 0);
    REQUIRE(graph.connect(source, 0, destination, 0));
    REQUIRE(graph.connect(destination, 0, output, 0));
    graph.set_canonical_executor_routing_enabled(true);
    REQUIRE(graph.prepare(48000.0, 64));
}

pulp::inspect::ControlRequestEnvelope route_request(std::string params_json) {
    pulp::inspect::ControlRequestEnvelope request;
    request.operation_id = "dev.pulp.graph/modulation-route.edit@1";
    request.operation_version = 1;
    request.params_json = std::move(params_json);
    return request;
}

pulp::inspect::ControlAdmissionPlan route_plan() {
    pulp::inspect::ControlAdmissionPlan plan;
    plan.receipt_id.value = "dspx04-direct-test";
    return plan;
}

std::string insert_params(pulp::host::NodeId source, pulp::host::NodeId destination,
                          std::string_view source_json = {}) {
    const auto source_text =
        source_json.empty() ? std::to_string(source) : std::string(source_json);
    return R"({"commands":[{"kind":"insert","source":)" + source_text +
           R"(,"source_port":0,"destination":)" + std::to_string(destination) +
           R"(,"parameter_id":7,"audio_rate":true,"range_lo":-2,"range_hi":3,"smoothing_ms":10}]})";
}
} // namespace

TEST_CASE("DSPX-04 host authority lifecycle and dense refusal", "[dspx-04][host-authority]") {
    pulp::host::SignalGraph graph;
    const auto source = graph.add_input_node(1, "source");
    const auto destination = graph.add_processor_node(std::make_unique<RouteProcessor>());
    const auto output = graph.add_output_node(1, "output");
    REQUIRE(source != 0);
    REQUIRE(destination != 0);
    REQUIRE(output != 0);
    REQUIRE(graph.connect(source, 0, destination, 0));
    REQUIRE(graph.connect(destination, 0, output, 0));
    graph.set_canonical_executor_routing_enabled(true);
    REQUIRE(graph.prepare(48000.0, 64));
    pulp::host::SignalGraphControlAuthority authority(graph, 48000.0, 64);
    pulp::host::SignalGraphRouteCommand insert;
    insert.kind = decltype(insert.kind)::Insert;
    insert.audio_rate = false;
    insert.source = source;
    insert.destination = destination;
    insert.parameter_id = RouteProcessor::kParam;
    insert.range_lo = 0.0f;
    insert.range_hi = 1.0f;
    REQUIRE(authority.apply({&insert, 1}).code == pulp::host::SignalGraphRouteResultCode::Applied);
    REQUIRE(graph.connections().size() == 3);
    pulp::host::SignalGraphRouteCommand remove = insert;
    remove.kind = decltype(remove.kind)::Remove;
    REQUIRE(authority.apply({&remove, 1}).code == pulp::host::SignalGraphRouteResultCode::Applied);
    REQUIRE(graph.connections().size() == 2);
    std::array<pulp::host::SignalGraphRouteCommand,
               pulp::host::SignalGraphControlAuthority::kMaxDenseCommands + 1>
        overflow{};
    REQUIRE(authority.apply(overflow).code ==
            pulp::host::SignalGraphRouteResultCode::DenseQueueOverflow);
    REQUIRE(graph.connections().size() == 2);
}

TEST_CASE("DSPX-04 processor graph binding is explicit and fail-closed",
          "[dspx-04][processor-graph-binding]") {
    RouteProcessor processor;
    pulp::host::SignalGraph graph;
    REQUIRE(pulp::host::create_processor_signal_graph_authority(processor) == nullptr);
    auto binding = pulp::host::ProcessorSignalGraphBinding::install(processor, graph, 48000.0, 64);
    REQUIRE(binding != nullptr);
    REQUIRE(pulp::host::create_processor_signal_graph_authority(processor) ==
            &binding->authority());
    const auto lease = pulp::host::create_processor_signal_graph_authority_lease(processor);
    REQUIRE(lease.valid());
    REQUIRE(lease.authority == &binding->authority());
    auto duplicate =
        pulp::host::ProcessorSignalGraphBinding::install(processor, graph, 48000.0, 64);
    REQUIRE(duplicate == nullptr);
    binding.reset();
    REQUIRE(pulp::host::create_processor_signal_graph_authority(processor) == nullptr);
    REQUIRE_FALSE(lease.valid());
}

TEST_CASE("DSPX-04 executor preserves integer route range and smoothing",
          "[dspx-04][host-authority][executor]") {
    pulp::host::SignalGraph graph;
    pulp::host::NodeId source = 0, destination = 0;
    prepare_graph(graph, source, destination);
    pulp::host::SignalGraphControlAuthority authority(graph, 48000.0, 64);
    auto executor = pulp::inspect::make_control_signal_graph_executor(
        [&authority](const pulp::inspect::ControlAdmissionPlan&) { return &authority; });
    pulp::inspect::ControlExecutionContext context;
    context.checkpoint = [] { return pulp::inspect::ControlExecutionCheckpoint::Continue; };

    const auto outcome =
        executor(route_plan(), route_request(insert_params(source, destination)), context);
    REQUIRE(outcome.terminal_state == pulp::inspect::ControlReceiptState::Completed);
    REQUIRE_FALSE(outcome.result.result_code.has_value());
    REQUIRE(graph.connections().size() == 3);
    const auto modulation =
        std::ranges::find_if(graph.connections(), [](const pulp::host::Connection& connection) {
            return connection.audio_rate_modulation;
        });
    REQUIRE(modulation != graph.connections().end());
    REQUIRE(modulation->source_node == source);
    REQUIRE(modulation->dest_node == destination);
    REQUIRE(modulation->automation_param_id == RouteProcessor::kParam);
    REQUIRE(modulation->automation_range_lo == -2.0f);
    REQUIRE(modulation->automation_range_hi == 3.0f);
    REQUIRE(modulation->automation_smoothing_ms == 10.0f);
}

TEST_CASE("DSPX-04 executor refuses out of range identifiers without mutation",
          "[dspx-04][host-authority][executor]") {
    pulp::host::SignalGraph graph;
    pulp::host::NodeId source = 0, destination = 0;
    prepare_graph(graph, source, destination);
    pulp::host::SignalGraphControlAuthority authority(graph, 48000.0, 64);
    auto executor = pulp::inspect::make_control_signal_graph_executor(
        [&authority](const pulp::inspect::ControlAdmissionPlan&) { return &authority; });
    pulp::inspect::ControlExecutionContext context;
    context.checkpoint = [] { return pulp::inspect::ControlExecutionCheckpoint::Continue; };
    const auto connections_before = graph.connections().size();
    const auto generation_before = authority.generation();

    const auto outcome = executor(
        route_plan(), route_request(insert_params(source, destination, "4294967297")), context);
    REQUIRE(outcome.terminal_state == pulp::inspect::ControlReceiptState::Failed);
    REQUIRE(outcome.result.result_code == pulp::inspect::ControlResultCode::InvalidRequest);
    REQUIRE(graph.connections().size() == connections_before);
    REQUIRE(authority.generation() == generation_before);
}

TEST_CASE("DSPX-04 executor rejects absent and released binding",
          "[dspx-04][host-authority][executor]") {
    RouteProcessor processor;
    pulp::host::SignalGraph graph;
    auto executor = pulp::inspect::make_control_signal_graph_executor(
        [&processor](const pulp::inspect::ControlAdmissionPlan&) {
            return pulp::host::create_processor_signal_graph_authority(processor);
        });
    pulp::inspect::ControlExecutionContext context;
    context.checkpoint = [] { return pulp::inspect::ControlExecutionCheckpoint::Continue; };
    const auto absent = executor(route_plan(), route_request("{\"commands\":[]}"), context);
    REQUIRE(absent.terminal_state == pulp::inspect::ControlReceiptState::Failed);
    REQUIRE(absent.result.result_code == pulp::inspect::ControlResultCode::HostUnavailable);

    auto binding = pulp::host::ProcessorSignalGraphBinding::install(processor, graph, 48000.0, 64);
    REQUIRE(binding != nullptr);
    binding.reset();
    const auto released = executor(route_plan(), route_request("{\"commands\":[]}"), context);
    REQUIRE(released.terminal_state == pulp::inspect::ControlReceiptState::Failed);
    REQUIRE(released.result.result_code == pulp::inspect::ControlResultCode::HostUnavailable);
    REQUIRE(graph.connections().empty());
}

TEST_CASE("DSPX-04 executor cancellation checkpoint prevents authority access",
          "[dspx-04][host-authority][executor]") {
    pulp::host::SignalGraph graph;
    pulp::host::NodeId source = 0, destination = 0;
    prepare_graph(graph, source, destination);
    pulp::host::SignalGraphControlAuthority authority(graph, 48000.0, 64);
    bool resolver_called = false;
    auto executor = pulp::inspect::make_control_signal_graph_executor(
        [&authority, &resolver_called](const pulp::inspect::ControlAdmissionPlan&) {
            resolver_called = true;
            return &authority;
        });
    pulp::inspect::ControlExecutionContext context;
    context.checkpoint = [] { return pulp::inspect::ControlExecutionCheckpoint::Cancelled; };
    const auto connections_before = graph.connections().size();
    const auto generation_before = authority.generation();

    const auto outcome =
        executor(route_plan(), route_request(insert_params(source, destination)), context);
    REQUIRE(outcome.terminal_state == pulp::inspect::ControlReceiptState::Failed);
    REQUIRE(outcome.result.result_code == pulp::inspect::ControlResultCode::Cancelled);
    REQUIRE_FALSE(resolver_called);
    REQUIRE(graph.connections().size() == connections_before);
    REQUIRE(authority.generation() == generation_before);
}
