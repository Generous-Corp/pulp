#include "allpass_processor.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <pulp/host/graph_serializer.hpp>
#include <pulp/runtime/log.hpp>

namespace pulp::examples {

SampleRegionAllpassProcessor::SampleRegionAllpassProcessor(std::string_view graph_json) {
    if (!graph_json.empty()) {
        if (!host::register_builtin_sample_region_types(graph_)) {
            error_ = "sample-region type registration failed";
            return;
        }
        const auto loaded = host::GraphSerializer::from_json(graph_, std::string(graph_json));
        if (!loaded.ok || !loaded.missing_plugins.empty() ||
            !loaded.missing_custom_node_types.empty()) {
            error_ = "allpass graph could not be resolved: " + loaded.error;
            return;
        }
        if (graph_.nodes().empty()) {
            error_ = "reloaded allpass graph must not be empty";
            return;
        }
    }
    if (!make_initial_edit())
        return;
    contract_ = initial_edit_->sample_region_parameter_contract().freeze({});
    if (!contract_.valid() || !contract_.frozen()) {
        error_ = contract_.error();
        initial_edit_.reset();
        return;
    }
    if (!graph_json.empty()) {
        // Reload may change routing, but never the host-visible parameter identity
        // or the mono, region-only execution contract of this Processor.
        host::SignalGraph canonical;
        auto expected = canonical.begin_prepared_topology_edit();
        std::string construction_error;
        if (!expected || !build_allpass_region(*expected, construction_error) ||
            !contract_.matches_promoted(expected->sample_region_parameter_contract())) {
            error_ = "reloaded graph changed the frozen allpass parameter manifest";
            contract_ = {};
            initial_edit_.reset();
            return;
        }
        const auto regions = initial_edit_->sample_regions();
        bool valid_shape = regions.size() == 1 && regions[0].region_id == kAllpassRegion &&
                           regions[0].input_boundaries.size() == 1 &&
                           regions[0].output_boundaries.size() == 1;
        unsigned inputs = 0, outputs = 0;
        if (valid_shape) {
            for (const auto& node : initial_edit_->nodes()) {
                if (node.type == host::NodeType::AudioInput) {
                    ++inputs;
                    valid_shape = valid_shape && node.num_output_ports == 1;
                } else if (node.type == host::NodeType::AudioOutput) {
                    ++outputs;
                    valid_shape = valid_shape && node.num_input_ports == 1;
                } else {
                    valid_shape =
                        valid_shape && node.type == host::NodeType::Custom &&
                        std::any_of(regions[0].members.begin(), regions[0].members.end(),
                                    [&](const auto& member) { return member.node == node.id; });
                }
            }
        }
        if (!valid_shape || inputs != 1 || outputs != 1 ||
            !initial_edit_->prove_sample_region(kAllpassRegion).accepted) {
            error_ = "reloaded graph must contain one valid mono region with no outside DSP";
            contract_ = {};
            initial_edit_.reset();
        }
    }
}

SampleRegionAllpassProcessor::~SampleRegionAllpassProcessor() = default;

bool SampleRegionAllpassProcessor::make_initial_edit() {
    initial_edit_ = graph_.begin_prepared_topology_edit();
    if (!initial_edit_) {
        error_ = "could not create prepared topology candidate";
        return false;
    }
    if (initial_edit_->nodes().empty() && !build_allpass_region(*initial_edit_, error_)) {
        initial_edit_.reset();
        return false;
    }
    return true;
}

format::PluginDescriptor SampleRegionAllpassProcessor::descriptor() const {
    return {.name = "Sample Region Allpass",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.sample-region-allpass",
            .version = "1.0.0",
            .category = format::PluginCategory::Effect,
            .input_buses = {{"Audio In", 1}},
            .output_buses = {{"Audio Out", 1}}};
}

void SampleRegionAllpassProcessor::define_parameters(state::StateStore& store) {
    if (binding_ && &binding_->store() == &store)
        return;
    if (binding_ || !contract_.valid() || !contract_.frozen() || store.param_count() != 0) {
        error_ = "allpass requires its frozen manifest and one empty adapter-owned store";
        ready_ = false;
        return;
    }
    for (const auto& info : contract_.parameters())
        store.add_parameter(info);
    binding_ = contract_.bind(store);
    if (!binding_) {
        error_ = "could not bind the frozen allpass parameter manifest";
        return;
    }
    set_state_store(&store);
}

bool SampleRegionAllpassProcessor::is_bus_layout_supported(const BusesLayout& layout) const {
    return layout.inputs == std::vector<int>{1} && layout.outputs == std::vector<int>{1};
}

void SampleRegionAllpassProcessor::prepare(const format::PrepareContext& context) {
    ready_ = false;
    if (!binding_ || context.input_channels != 1 || context.output_channels != 1 ||
        !std::isfinite(context.sample_rate) || context.sample_rate <= 0 ||
        context.max_buffer_size <= 0) {
        error_ = "allpass preparation requires a bound mono store and valid rate/block size";
        return;
    }
    if (!initialized_) {
        if (!initial_edit_ && !make_initial_edit())
            return;
        if (!contract_.matches_promoted(initial_edit_->sample_region_parameter_contract())) {
            error_ = "allpass candidate changed the frozen parameter manifest";
            initial_edit_.reset();
            return;
        }
        const auto bound = initial_edit_->bind_sample_region_parameters(*binding_);
        using Result = host::SignalGraph::PreparedTopologyEdit::Result;
        if (!bound.accepted ||
            initial_edit_->prepare(context.sample_rate, context.max_buffer_size) !=
                Result::Prepared ||
            initial_edit_->commit() != Result::Committed) {
            error_ = "allpass candidate bind, prepare or commit failed: " + bound.message;
            initial_edit_.reset();
            return;
        }
        initial_edit_.reset();
        initialized_ = true;
    } else if (!graph_.prepare(context.sample_rate, context.max_buffer_size)) {
        error_ = "allpass graph preparation failed";
        return;
    }
    max_buffer_size_ = context.max_buffer_size;
    input_alias_scratch_.assign(static_cast<std::size_t>(max_buffer_size_), 0.0f);
    error_.clear();
    ready_ = true;
}

void SampleRegionAllpassProcessor::release() {
    ready_ = false;
    input_alias_scratch_.clear();
    initial_edit_.reset();
    graph_.release();
}

void SampleRegionAllpassProcessor::process(audio::BufferView<float>& output,
                                           const audio::BufferView<const float>& input,
                                           midi::MidiBuffer&, midi::MidiBuffer&,
                                           const format::ProcessContext& context) {
    if (!ready_ || input.num_channels() != 1 || output.num_channels() != 1 ||
        context.num_samples < 0 || context.num_samples > max_buffer_size_ ||
        static_cast<std::size_t>(context.num_samples) > input.num_samples() ||
        static_cast<std::size_t>(context.num_samples) > output.num_samples()) {
        output.clear();
        return;
    }
    // REAPER supplies in-place AU/VST3 buffers for this mono effect. The
    // routed graph clears its output bus before copying AudioInput, so an
    // aliased input would be erased before the graph sees it. Preserve the
    // input in prepare-sized scratch (matching BakedGraphProcessor's in-place
    // contract) before entering the graph; this remains allocation-free on the
    // render thread and leaves separate-buffer hosts on the direct path.
    const float* input_ptr = input.channel_ptr(0);
    float* output_ptr = output.channel_ptr(0);
    const auto frames = static_cast<std::size_t>(context.num_samples);
    const bool overlaps = output_ptr < input_ptr + frames && input_ptr < output_ptr + frames;
    if (overlaps) {
        std::copy_n(input_ptr, frames, input_alias_scratch_.data());
        const std::array<const float*, 1> safe_ptrs{input_alias_scratch_.data()};
        const audio::BufferView<const float> safe_input(safe_ptrs.data(), 1, context.num_samples);
        graph_.process(output, safe_input, context.num_samples);
    } else {
        graph_.process(output, input, context.num_samples);
    }
}

std::unique_ptr<format::Processor> create_sample_region_allpass() {
    auto processor = std::make_unique<SampleRegionAllpassProcessor>();
    if (!processor->error().empty())
        return {};
    return processor;
}

} // namespace pulp::examples
