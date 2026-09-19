#include "allpass_graph.hpp"

namespace pulp::examples {

bool build_allpass_region(host::SignalGraph::PreparedTopologyEdit& edit, std::string& error) {
    using namespace host;
    if (!edit.nodes().empty() || !register_builtin_sample_region_types(edit)) {
        error = "allpass construction requires an empty candidate and built-in registration";
        return false;
    }
    const auto input = edit.add_input_node(1, "Input");
    const auto output = edit.add_output_node(1, "Output");
    const auto x = edit.add_custom_node("pulp.core.sample-region.input", 1);
    const auto a = edit.add_custom_node("pulp.core.sample-region.parameter", 1);
    const auto negative = edit.add_custom_node("pulp.core.sample-region.constant", 1);
    const auto ax = edit.add_custom_node("pulp.core.sample-region.multiply", 1);
    const auto xd = edit.add_custom_node("pulp.core.unit-delay", 1);
    const auto yd = edit.add_custom_node("pulp.core.unit-delay", 1);
    const auto ayd = edit.add_custom_node("pulp.core.sample-region.multiply", 1);
    const auto nayd = edit.add_custom_node("pulp.core.sample-region.multiply", 1);
    const auto sum = edit.add_custom_node("pulp.core.sample-region.add", 1);
    const auto y = edit.add_custom_node("pulp.core.sample-region.add", 1);
    const auto out = edit.add_custom_node("pulp.core.sample-region.output", 1);
    if (!input || !output || !x || !a || !negative || !ax || !xd || !yd || !ayd || !nayd || !sum ||
        !y || !out || !edit.connect(input, 0, x, 0) || !edit.connect(x, 0, ax, 0) ||
        !edit.connect(a, 0, ax, 1) || !edit.connect(x, 0, xd, 0) || !edit.connect(yd, 0, ayd, 0) ||
        !edit.connect(a, 0, ayd, 1) || !edit.connect(negative, 0, nayd, 0) ||
        !edit.connect(ayd, 0, nayd, 1) || !edit.connect(ax, 0, sum, 0) ||
        !edit.connect(xd, 0, sum, 1) || !edit.connect(sum, 0, y, 0) ||
        !edit.connect(nayd, 0, y, 1) || !edit.connect(y, 0, out, 0) ||
        !edit.connect(out, 0, output, 0)) {
        error = "allpass node or connection construction failed";
        return false;
    }
    const SampleKernelConfig none{SampleKernelConfigKind::None, 0, 0.0f};
    const SampleKernelConfig boundary{SampleKernelConfigKind::BoundaryIndex, 0, 0.0f};
    SampleRegionDefinition region;
    region.region_id = kAllpassRegion;
    region.members = {
        {x, "pulp.core.sample-region.input", 1, boundary},
        {a,
         "pulp.core.sample-region.parameter",
         1,
         {SampleKernelConfigKind::PromotedParameterId, kAllpassCoefficient, 0.0f}},
        {negative,
         "pulp.core.sample-region.constant",
         1,
         {SampleKernelConfigKind::FiniteConstant, 0, -1.0f}},
        {ax, "pulp.core.sample-region.multiply", 1, none},
        {xd, "pulp.core.unit-delay", 1, none},
        {yd, "pulp.core.unit-delay", 1, none},
        {ayd, "pulp.core.sample-region.multiply", 1, none},
        {nayd, "pulp.core.sample-region.multiply", 1, none},
        {sum, "pulp.core.sample-region.add", 1, none},
        {y, "pulp.core.sample-region.add", 1, none},
        {out, "pulp.core.sample-region.output", 1, boundary},
    };
    region.input_boundaries = {x};
    region.output_boundaries = {out};
    SampleRegionPromotedParameter coefficient;
    coefficient.param_id = kAllpassCoefficient;
    coefficient.key = "coefficient";
    coefficient.name = "Allpass coefficient";
    coefficient.range = state::ParamRange::linear(-0.99f, 0.99f, 0.5f);
    coefficient.bound_node_id = a;
    region.promoted_parameters = {coefficient};
    const auto declared = edit.declare_sample_region(std::move(region));
    if (!declared.accepted) {
        error = declared.message;
        return false;
    }
    const auto feedback = edit.connect_in_sample_region(kAllpassRegion, y, 0, yd, 0);
    if (!feedback.accepted) {
        error = feedback.message;
        return false;
    }
    error.clear();
    return true;
}

} // namespace pulp::examples
