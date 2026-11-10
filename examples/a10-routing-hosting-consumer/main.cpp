#include <pulp/host/plugin_slot.hpp>
#include <pulp/host/signal_graph_runtime.hpp>

#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
using namespace pulp;
using namespace pulp::host;

void require(bool value, const char* rule) {
    if (!value)
        throw std::runtime_error(rule);
}

class FixtureSlot final : public PluginSlot {
  public:
    FixtureSlot() {
        info_.name = "A10 deterministic hosted fixture";
        info_.manufacturer = "Pulp";
        info_.version = "1";
        info_.unique_id = "com.pulp.a10.fixture";
        info_.format = PluginFormat::BuiltIn;
        info_.num_inputs = 2;
        info_.num_outputs = 1;
        info_.supports_sidechain = true;
        info_.has_editor = false;
        params_.push_back(
            HostParamInfo{7, "gain", "", 0.0f, 2.0f, 1.0f, {}, ParamRate::ControlRate});
    }

    const PluginInfo& info() const override {
        return info_;
    }
    bool is_loaded() const override {
        return loaded_;
    }
    bool prepare(double, int) override {
        loaded_ = true;
        return true;
    }
    void release() override {
        loaded_ = false;
    }
    void process(audio::BufferView<float>& output, const audio::BufferView<const float>& input,
                 const midi::MidiBuffer&, midi::MidiBuffer&, const ParameterEventQueue&,
                 int n) override {
        const float gain = parameter_;
        for (int i = 0; i < n; ++i)
            output.channel_ptr(0)[i] =
                (input.num_channels() ? input.channel_ptr(0)[i] : 0.0f) * gain;
    }
    std::vector<HostParamInfo> parameters() const override {
        return params_;
    }
    float get_parameter(uint32_t id) const override {
        return id == 7 ? parameter_ / 2.0f : 0.0f;
    }
    void set_parameter(uint32_t id, float value) override {
        if (id == 7)
            parameter_ = value * 2.0f;
    }
    void set_bypass(bool value) override {
        bypassed_ = value;
    }
    bool is_bypassed() const override {
        return bypassed_;
    }
    std::vector<uint8_t> save_state() const override {
        return {static_cast<uint8_t>(parameter_ * 10)};
    }
    bool restore_state(const std::vector<uint8_t>& state) override {
        if (state.size() != 1)
            return false;
        parameter_ = static_cast<float>(state[0]) / 10.0f;
        return true;
    }
    bool has_editor() const override {
        return false;
    }
    void* create_editor_view() override {
        return nullptr;
    }
    void destroy_editor_view() override {}
    int latency_samples() const override {
        return 4;
    }
    int tail_samples() const override {
        return 0;
    }

  private:
    PluginInfo info_;
    std::vector<HostParamInfo> params_;
    float parameter_ = 1.0f;
    bool loaded_ = false;
    bool bypassed_ = false;
};

} // namespace

int main() {
    try {
        SignalGraph graph;
        const auto input = graph.add_input_node(1, "main");
        const auto side = graph.add_input_node(1, "sidechain");
        const auto control = graph.add_gain_node("automation");
        const auto plugin = graph.add_plugin_node(std::make_unique<FixtureSlot>(), 2, 1, "fixture");
        const auto output = graph.add_output_node(1, "output");
        require(input && side && control && plugin && output, "nodes");
        require(graph.connect(input, 0, plugin, 0), "main-route");
        require(graph.connect_sidechain(side, 0, plugin, 1), "sidechain-route");
        require(!graph.connect_sidechain(side, 0, output, 0), "sidechain-destination-refusal");
        require(graph.connect(control, 0, plugin, 0), "control-route");
        require(graph.connect_automation(control, 0, plugin, 7, 0.0f, 2.0f), "automation-route");
        require(!graph.connect_automation(control, 0, plugin, 999, 0.0f, 1.0f),
                "unknown-param-refusal");
        require(graph.connect(plugin, 0, output, 0), "output-route");
        require(graph.prepare(48000.0, 64), "prepare");
        require(graph.latency_samples() == 4, "latency");
        const auto snapshot = graph.prepared_latency_snapshot();
        require(snapshot && snapshot->total_latency_samples == 4, "latency-snapshot");

        graph.begin_swap_edit();
        const auto extra = graph.add_gain_node("staged");
        require(extra != 0, "staged-node");
        const auto swap = graph.prepare_swap(48000.0, 64);
        require(swap == SignalGraph::SwapResult::Swapped ||
                    swap == SignalGraph::SwapResult::NeedsEagerPrepare,
                "swap-result");
        if (swap == SignalGraph::SwapResult::NeedsEagerPrepare)
            require(graph.prepare(48000.0, 64), "eager-prepare");

        std::cout
            << "a10-routing-hosting PASS sidechain=1 automation=1 pdc=4 transactional_swap=1\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "a10-routing-hosting FAIL rule=" << error.what() << '\n';
        return 1;
    }
}
