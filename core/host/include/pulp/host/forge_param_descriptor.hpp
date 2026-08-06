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

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pulp::host {

/// Whether a parameter sweeps continuously or selects among named states.
enum class ForgeParamKind { continuous, stepped };

/// How a value maps to its control travel. A log parameter whose control is
/// linear feels dead across most of its range.
enum class ForgeParamCurve { linear, logarithmic };

/// One named state of a stepped parameter. `token` is the stable machine key
/// and must never change; `label` is presentation and may.
struct ForgeParamChoice {
    std::string_view token;
    std::string_view label;
    float value = 0.0f;
    /// Empty means the choice is valid wherever its parameter is valid.
    /// Otherwise it is offered only by these realization modes.
    std::vector<std::string_view> realization_modes;

    ForgeParamChoice() = default;
    ForgeParamChoice(std::string_view token_in, std::string_view label_in, float value_in,
                     std::vector<std::string_view> modes_in = {})
        : token(token_in), label(label_in), value(value_in),
          realization_modes(std::move(modes_in)) {}
};

/// One value of a compile-time realization axis.
///
/// This intentionally does not carry `realization_modes`: an axis value helps
/// define those modes, so scoping it back to a mode would be circular.
struct ForgeAxisValue {
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

    ForgeParamDescriptor() = default;
    ForgeParamDescriptor(std::string_view key_in, state::ParamID id_in, std::string_view label_in,
                         std::string_view unit_in, std::string_view description_in,
                         ForgeParamKind kind_in, ForgeParamCurve curve_in,
                         std::vector<ForgeParamChoice> choices_in = {},
                         std::vector<std::string_view> modes_in = {})
        : key(key_in), id(id_in), label(label_in), unit(unit_in), description(description_in),
          kind(kind_in), curve(curve_in), choices(std::move(choices_in)),
          realization_modes(std::move(modes_in)) {}
};

/// A compile-time construction axis — a choice made when the node is built
/// rather than automated afterwards. Enumerated finitely so a consumer can
/// present the whole space without knowing the family.
struct ForgeRealizationAxis {
    std::string_view key;
    std::string_view label;
    std::string_view description;
    std::vector<ForgeAxisValue> values;
};

/// One axis/value selection used to construct a concrete realization.
///
/// Both fields name stable tokens declared by the containing descriptor. The
/// explicit pair keeps consumers from having to reverse-engineer an opaque
/// `mode` string such as "silicon_x4".
struct ForgeRealizationSetting {
    std::string_view axis;
    std::string_view value;
};

/// One concrete realization: the exact `type_id` a graph names, plus the axis
/// settings that produced it.
struct ForgeRealization {
    /// Stable mode key, e.g. "silicon_x4".
    std::string mode;
    /// The node type id registered with the graph.
    std::string type_id;
    /// Exactly one selection for each declared realization axis.
    std::vector<ForgeRealizationSetting> settings;

    ForgeRealization() = default;
    ForgeRealization(std::string_view mode_in, std::string_view type_id_in,
                     std::vector<ForgeRealizationSetting> settings_in = {})
        : mode(mode_in), type_id(type_id_in), settings(std::move(settings_in)) {}
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
        if (param.key == key)
            return &param;
    return nullptr;
}

/// True when `param` applies to the named realization mode.
inline bool param_applies(const ForgeParamDescriptor& param, std::string_view mode) noexcept {
    if (param.realization_modes.empty())
        return true;
    for (const auto& allowed : param.realization_modes)
        if (allowed == mode)
            return true;
    return false;
}

inline bool choice_applies(const ForgeParamChoice& choice, std::string_view mode) noexcept {
    if (choice.realization_modes.empty())
        return true;
    for (const auto& allowed : choice.realization_modes)
        if (allowed == mode)
            return true;
    return false;
}

}  // namespace pulp::host
