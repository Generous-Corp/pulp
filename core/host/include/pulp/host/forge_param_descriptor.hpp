#pragma once

// Semantic descriptors for Forge-exposed catalog parameters.
//
// CustomNodeBakedParam carries the numeric contract an audio graph needs: id,
// range, default. An agent choosing a node needs more than that — what the
// parameter is called, what it is measured in, whether it steps between named
// states, and what it does. That vocabulary lived downstream and had to be kept
// in sync by hand; a descriptor moves it next to the DSP it describes.
//
// Descriptors deliberately carry NO range or default. Those come from the baked
// node at export time, so a descriptor cannot drift out of agreement with the
// DSP it annotates — there is only ever one copy of a number.

#include <pulp/state/parameter.hpp>

#include <string_view>
#include <vector>

namespace pulp::host {

/// Whether a parameter sweeps continuously or selects among named states.
enum class ForgeParamKind { continuous, stepped };

/// How a value maps to its control travel. A log parameter whose control is
/// linear feels dead across most of its range.
enum class ForgeParamCurve { linear, logarithmic };

/// One named state of a stepped parameter, or one setting of a realization
/// axis. `token` is the stable machine key and must never change; `label` is
/// presentation and may.
struct ForgeParamChoice {
    std::string_view token;
    std::string_view label;
    float value = 0.0f;
};

struct ForgeParamDescriptor {
    /// Stable machine key, snake_case, unique within its node. Never change one
    /// — downstream graphs are authored against it.
    std::string_view key;
    /// The baked parameter this describes. Must match a CustomNodeBakedParam id
    /// on the node, which is what the completeness check proves.
    state::ParamID id = 0;
    std::string_view label;
    /// Unit symbol as displayed ("dB", "Hz", "ms", "%"). Empty when unitless.
    std::string_view unit;
    std::string_view description;
    ForgeParamKind kind = ForgeParamKind::continuous;
    ForgeParamCurve curve = ForgeParamCurve::linear;
    /// Required and non-empty when `kind` is stepped; empty otherwise.
    std::vector<ForgeParamChoice> choices;
    /// Empty means the parameter exists on every realization. Otherwise it
    /// exists only on the named ones — some families deliberately omit a
    /// control that would be inert (an Ampex deck has no selectable EQ
    /// standard), and an inert control presented as live is a worse answer
    /// than an absent one.
    std::vector<std::string_view> realization_modes;
};

/// A compile-time construction axis — a choice made when the node is built
/// rather than automated afterwards. Enumerated finitely so a consumer can
/// present the whole space without knowing the family.
struct ForgeRealizationAxis {
    std::string_view key;
    std::string_view label;
    std::string_view description;
    std::vector<ForgeParamChoice> values;
};

/// One concrete realization: the exact `type_id` a graph names, plus the axis
/// settings that produced it.
struct ForgeRealization {
    /// Stable mode key, e.g. "silicon_x4".
    std::string_view mode;
    /// The node type id registered with the graph.
    std::string_view type_id;
};

struct ForgeNodeDescriptor {
    /// Stable family key, e.g. "fuzz".
    std::string_view key;
    std::string_view label;
    /// One or two sentences: what this node is for, in the vocabulary someone
    /// choosing it would use.
    std::string_view description;
    std::vector<ForgeRealizationAxis> axes;
    std::vector<ForgeRealization> realizations;
    std::vector<ForgeParamDescriptor> params;
};

/// Look up a parameter descriptor by its stable key. Returns nullptr when
/// absent, so a caller can decide whether that is a lookup miss or a contract
/// violation.
inline const ForgeParamDescriptor* find_param(const ForgeNodeDescriptor& node,
                                              std::string_view key) noexcept {
    for (const auto& param : node.params)
        if (param.key == key) return &param;
    return nullptr;
}

/// True when `param` applies to the named realization mode.
inline bool param_applies(const ForgeParamDescriptor& param,
                          std::string_view mode) noexcept {
    if (param.realization_modes.empty()) return true;
    for (const auto& allowed : param.realization_modes)
        if (allowed == mode) return true;
    return false;
}

}  // namespace pulp::host
