#include <pulp/format/processor.hpp>
#include <pulp/format/standalone.hpp>
#include <pulp/format/standalone_control_host.hpp>
#include <pulp/host/processor_signal_graph_binding.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/inspect/control_standalone_host.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <fstream>
#include <pulp/host/graph_serializer.hpp>
#include <choc/text/choc_JSON.h>
#include <memory>

extern "C" const volatile char pulp_control_standalone_host_markers_v1[];

[[gnu::used]] const volatile char graph_file_reload_graph_capability_marker[] =
    "PULP_INSPECT_CAPABILITY_GRAPH_FILE_RELOAD_V1";

[[gnu::used]] const volatile char graph_file_reload_control_implementation_markers[] =
    "PULP_INSPECT_CAPABILITY_DEV_PULP_INSTANCE_READ_1_V1\0"
    "PULP_INSPECT_CAPABILITY_DEV_PULP_SESSION_CONTROL_1_V1\0"
    "PULP_INSPECT_CAPABILITY_DEV_PULP_GRAPH_FILE_RELOAD_1_V1";

namespace {

class RouteNodeProcessor final : public pulp::format::Processor {
  public:
    static constexpr std::uint32_t kParameter = 7;
    pulp::format::PluginDescriptor descriptor() const override {
        pulp::format::PluginDescriptor d;
        d.name = "Graph reload target";
        d.manufacturer = "Pulp";
        d.bundle_id = "dev.pulp.test.graph-file-reload";
        d.version = "1";
        d.input_buses = {{"In", 1, false}};
        d.output_buses = {{"Out", 1, false}};
        d.node_capabilities.consumes_audio_rate_modulations = true;
        return d;
    }
    void define_parameters(pulp::state::StateStore& store) override {
        pulp::state::ParamInfo p;
        p.id = kParameter;
        p.name = "Route";
        p.range = {0.0f, 1.0f, 0.5f};
        p.rate = pulp::state::ParamRate::AudioRate;
        p.modulatable = true;
        store.add_parameter(p);
    }
    void prepare(const pulp::format::PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>& output,
                 const pulp::audio::BufferView<const float>& input, pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&, const pulp::format::ProcessContext&) override {
        for (std::size_t frame = 0; frame < output.num_samples(); ++frame)
            output.channel(0)[frame] = input.channel(0)[frame] * 0.25f;
    }
};

class GraphProductProcessor final : public pulp::format::Processor {
  public:
    pulp::format::PluginDescriptor descriptor() const override {
        pulp::format::PluginDescriptor d;
        d.name = "Graph File Reload graph product fixture";
        d.manufacturer = "Pulp";
        d.bundle_id = "dev.pulp.test.graph-file-reload";
        d.version = "1.0.0";
        d.input_buses = {{"In", 1, false}};
        d.output_buses = {{"Out", 1, false}};
        return d;
    }
    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const pulp::format::PrepareContext& context) override {
        stimulus_.fill(0.5f);
        source_ = graph_.add_input_node(1, "source");
        destination_ = graph_.add_processor_node(std::make_unique<RouteNodeProcessor>());
        output_ = graph_.add_output_node(1, "output");
        if (source_ == 0 || destination_ == 0 || output_ == 0 ||
            !graph_.connect(source_, 0, destination_, 0) ||
            !graph_.connect(destination_, 0, output_, 0))
            return;
        graph_.set_canonical_executor_routing_enabled(true);
        if (!graph_.prepare(context.sample_rate, 64))
            return;
        binding_ = pulp::host::ProcessorSignalGraphBinding::install(*this, graph_,
                                                                    context.sample_rate, 64);
    }
    void process(pulp::audio::BufferView<float>& output,
                 const pulp::audio::BufferView<const float>&, pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&, const pulp::format::ProcessContext&) override {
        float* source = stimulus_.data();
        const pulp::audio::BufferView<const float> input(&source, 1, stimulus_.size());
        for (std::size_t start = 0; start < output.num_samples(); start += stimulus_.size()) {
            auto block = output.slice(start, stimulus_.size());
            graph_.process(block, input.slice(0, block.num_samples()),
                           static_cast<int>(block.num_samples()));
        }
        if (!output.empty()) {
            // The main-thread evidence observer needs only the latest complete
            // callback sample; it does not order any other shared state.
            last_sample_.store(output.channel(0)[0], std::memory_order_relaxed);
            process_count_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void record_evidence() const {
        auto evidence = choc::value::createObject("");
        evidence.addMember("sample", last_sample_.load(std::memory_order_relaxed));
        evidence.addMember("process_count", static_cast<std::int64_t>(
            process_count_.load(std::memory_order_relaxed)));
        evidence.addMember("state", pulp::host::GraphSerializer::to_json(graph_));
        // Resolver callbacks run on the control thread. No I/O or graph
        // serialization enters the product's audio callback.
        std::ofstream file("graph-audio.json");
        file << choc::json::toString(evidence);
    }
    pulp::format::ViewSize view_size() const override {
        return {1, 1, 1, 1, 1, 1};
    }
    pulp::host::NodeId source() const noexcept {
        return source_;
    }
    pulp::host::NodeId destination() const noexcept {
        return destination_;
    }

  private:
    std::array<float, 64> stimulus_{};
    std::atomic<float> last_sample_{0};
    std::atomic<std::uint64_t> process_count_{0};
    pulp::host::SignalGraph graph_;
    std::unique_ptr<pulp::host::ProcessorSignalGraphBinding> binding_;
    pulp::host::NodeId source_ = 0;
    pulp::host::NodeId destination_ = 0;
    pulp::host::NodeId output_ = 0;
};

pulp::host::SignalGraphControlAuthority* resolve_graph(pulp::format::Processor& processor) {
    static_cast<GraphProductProcessor&>(processor).record_evidence();
    return pulp::host::create_processor_signal_graph_authority(processor);
}

std::unique_ptr<pulp::format::Processor> create_processor() {
    return std::make_unique<GraphProductProcessor>();
}

[[maybe_unused]] const bool control_factory_installed =
    pulp::format::detail::install_standalone_control_host_factory(
        &pulp::inspect::make_control_standalone_host);
[[maybe_unused]] const bool graph_resolver_installed =
    pulp::inspect::detail::install_standalone_signal_graph_authority_factory(&resolve_graph);

} // namespace

int main() {
    if (!control_factory_installed || !graph_resolver_installed ||
        pulp_control_standalone_host_markers_v1[0] != 'P')
        return 64;
    pulp::format::StandaloneConfig config;
    config.input_channels = 0;
    config.output_channels = 1;
    config.persist_settings = false;
    config.show_settings_tab = false;
    pulp::format::StandaloneApp app(&create_processor);
    app.set_config(config);
    return app.run_with_editor(false) ? 0 : 65;
}
