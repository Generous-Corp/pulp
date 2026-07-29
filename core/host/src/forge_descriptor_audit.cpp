#include <pulp/host/forge_descriptor_audit.hpp>

#include <set>
#include <unordered_map>
#include <unordered_set>

namespace pulp::host {
namespace {

void note(std::vector<ForgeAuditFinding>& out, ForgeAuditFault fault, std::string_view node,
          std::string_view subject, std::string detail) {
    out.push_back({fault, std::string(node), std::string(subject), std::move(detail)});
}

} // namespace

const char* forge_audit_fault_name(ForgeAuditFault fault) {
    switch (fault) {
    case ForgeAuditFault::missing_catalog_node:
        return "missing_catalog_node";
    case ForgeAuditFault::unexpected_catalog_node:
        return "unexpected_catalog_node";
    case ForgeAuditFault::missing_realization:
        return "missing_realization";
    case ForgeAuditFault::unexpected_realization:
        return "unexpected_realization";
    case ForgeAuditFault::mismatched_type_id:
        return "mismatched_type_id";
    case ForgeAuditFault::duplicate_type_id:
        return "duplicate_type_id";
    case ForgeAuditFault::duplicate_axis:
        return "duplicate_axis";
    case ForgeAuditFault::duplicate_realization:
        return "duplicate_realization";
    case ForgeAuditFault::missing_axis_setting:
        return "missing_axis_setting";
    case ForgeAuditFault::unexpected_axis_setting:
        return "unexpected_axis_setting";
    case ForgeAuditFault::unknown_axis_value:
        return "unknown_axis_value";
    case ForgeAuditFault::undescribed_param:
        return "undescribed_param";
    case ForgeAuditFault::stale_descriptor:
        return "stale_descriptor";
    case ForgeAuditFault::duplicate_key:
        return "duplicate_key";
    case ForgeAuditFault::duplicate_id:
        return "duplicate_id";
    case ForgeAuditFault::missing_choices:
        return "missing_choices";
    case ForgeAuditFault::duplicate_choice:
        return "duplicate_choice";
    case ForgeAuditFault::choices_on_continuous:
        return "choices_on_continuous";
    case ForgeAuditFault::choice_out_of_range:
        return "choice_out_of_range";
    case ForgeAuditFault::unknown_realization:
        return "unknown_realization";
    case ForgeAuditFault::empty_field:
        return "empty_field";
    case ForgeAuditFault::no_realizations:
        return "no_realizations";
    }
    return "unknown";
}

std::vector<ForgeAuditFinding>
audit_forge_catalog_membership(const std::vector<ForgeNodeDescriptor>& descriptors,
                               const std::vector<std::string_view>& expected_node_keys) {
    std::vector<ForgeAuditFinding> out;
    std::unordered_map<std::string_view, int> actual;
    for (const auto& descriptor : descriptors)
        ++actual[descriptor.key];

    for (const auto expected : expected_node_keys) {
        const auto found = actual.find(expected);
        if (found == actual.end()) {
            note(out, ForgeAuditFault::missing_catalog_node, expected, "",
                 "indexed semantic node is absent from the export");
        } else if (found->second > 1) {
            note(out, ForgeAuditFault::duplicate_key, expected, "",
                 "node appears " + std::to_string(found->second) + " times in the export");
        }
    }
    const std::unordered_set<std::string_view> expected(expected_node_keys.begin(),
                                                        expected_node_keys.end());
    for (const auto& [key, count] : actual) {
        if (!expected.contains(key))
            note(out, ForgeAuditFault::unexpected_catalog_node, key, "",
                 "export node is absent from the canonical semantic registry");
    }
    return out;
}

std::vector<ForgeAuditFinding>
audit_forge_descriptor(const ForgeNodeDescriptor& descriptor,
                       const std::vector<CustomNodeBakedParam>& baked,
                       std::string_view realization_mode) {
    std::vector<ForgeAuditFinding> out;
    const auto node = descriptor.key;

    if (descriptor.key.empty())
        note(out, ForgeAuditFault::empty_field, node, "key", "node key is empty");
    if (descriptor.label.empty())
        note(out, ForgeAuditFault::empty_field, node, "label", "node label is empty");
    if (descriptor.description.empty())
        note(out, ForgeAuditFault::empty_field, node, "description", "node description is empty");
    if (descriptor.realizations.empty())
        note(out, ForgeAuditFault::no_realizations, node, "",
             "node declares no realization to build");

    std::unordered_map<std::string_view, const ForgeRealizationAxis*> axes;
    for (const auto& axis : descriptor.axes) {
        if (axis.key.empty())
            note(out, ForgeAuditFault::empty_field, node, "<axis>", "axis key is empty");
        if (axis.label.empty())
            note(out, ForgeAuditFault::empty_field, node, axis.key, "axis label is empty");
        if (axis.description.empty())
            note(out, ForgeAuditFault::empty_field, node, axis.key,
                 "axis description is empty");
        if (!axes.emplace(axis.key, &axis).second)
            note(out, ForgeAuditFault::duplicate_axis, node, axis.key,
                 "axis stable key is declared more than once");

        std::unordered_set<std::string_view> values;
        for (const auto& value : axis.values) {
            if (value.token.empty())
                note(out, ForgeAuditFault::empty_field, node, axis.key,
                     "axis value token is empty");
            if (value.label.empty())
                note(out, ForgeAuditFault::empty_field, node, axis.key,
                     "axis value label is empty");
            if (!values.insert(value.token).second)
                note(out, ForgeAuditFault::duplicate_axis, node, axis.key,
                     "axis value '" + std::string(value.token) +
                         "' is declared more than once");
        }
    }

    std::unordered_set<std::string_view> modes;
    for (const auto& realization : descriptor.realizations) {
        if (realization.mode.empty())
            note(out, ForgeAuditFault::empty_field, node, "<realization>",
                 "realization mode is empty");
        if (realization.type_id.empty())
            note(out, ForgeAuditFault::empty_field, node, realization.mode,
                 "realization type id is empty");
        if (!modes.insert(realization.mode).second)
            note(out, ForgeAuditFault::duplicate_realization, node, realization.mode,
                 "realization mode is declared more than once");

        std::unordered_set<std::string_view> selected;
        for (const auto& setting : realization.settings) {
            if (!selected.insert(setting.axis).second) {
                note(out, ForgeAuditFault::duplicate_axis, node, realization.mode,
                     "realization selects axis '" + std::string(setting.axis) +
                         "' more than once");
                continue;
            }
            const auto axis = axes.find(setting.axis);
            if (axis == axes.end()) {
                note(out, ForgeAuditFault::unexpected_axis_setting, node, realization.mode,
                     "selects undeclared axis '" + std::string(setting.axis) + "'");
                continue;
            }
            const auto& values = axis->second->values;
            const bool known =
                std::any_of(values.begin(), values.end(), [&setting](const auto& value) {
                    return value.token == setting.value;
                });
            if (!known)
                note(out, ForgeAuditFault::unknown_axis_value, node, realization.mode,
                     "axis '" + std::string(setting.axis) + "' has no value '" +
                         std::string(setting.value) + "'");
        }
        for (const auto& [axis, ignored] : axes)
            if (!selected.contains(axis))
                note(out, ForgeAuditFault::missing_axis_setting, node, realization.mode,
                     "does not select declared axis '" + std::string(axis) + "'");
    }

    std::unordered_map<std::string_view, int> key_counts;
    std::unordered_map<state::ParamID, int> id_counts;
    std::unordered_map<state::ParamID, const CustomNodeBakedParam*> baked_by_id;
    for (const auto& param : baked) {
        if (!baked_by_id.emplace(param.id, &param).second)
            note(out, ForgeAuditFault::duplicate_id, node, std::to_string(param.id),
                 "constructed realization bakes parameter id " +
                     std::to_string(param.id) + " more than once");
    }

    for (const auto& param : descriptor.params) {
        ++key_counts[param.key];
        ++id_counts[param.id];

        if (param.key.empty())
            note(out, ForgeAuditFault::empty_field, node, "<unnamed>", "parameter key is empty");
        if (param.label.empty())
            note(out, ForgeAuditFault::empty_field, node, param.key, "parameter label is empty");
        if (param.description.empty())
            note(out, ForgeAuditFault::empty_field, node, param.key,
                 "parameter description is empty");

        const bool stepped = param.kind == ForgeParamKind::stepped;
        const bool applies =
            realization_mode.empty() || param_applies(param, realization_mode);
        if (stepped && param.choices.empty())
            note(out, ForgeAuditFault::missing_choices, node, param.key,
                 "stepped parameter declares no named states");
        if (!stepped && !param.choices.empty())
            note(out, ForgeAuditFault::choices_on_continuous, node, param.key,
                 "continuous parameter declares named states");

        std::unordered_set<std::string_view> choice_tokens;
        std::unordered_set<std::string_view> choice_labels;
        std::size_t applicable_choices = 0;
        for (const auto& choice : param.choices) {
            if (choice.token.empty())
                note(out, ForgeAuditFault::empty_field, node, param.key,
                     "named state token is empty");
            if (choice.label.empty())
                note(out, ForgeAuditFault::empty_field, node, param.key,
                     "named state label is empty");
            if (!choice_tokens.insert(choice.token).second)
                note(out, ForgeAuditFault::duplicate_choice, node, param.key,
                     "named state token '" + std::string(choice.token) +
                         "' is declared more than once");
            if (!choice_labels.insert(choice.label).second)
                note(out, ForgeAuditFault::duplicate_choice, node, param.key,
                     "named state label '" + std::string(choice.label) +
                         "' is declared more than once");
            if (realization_mode.empty() || choice_applies(choice, realization_mode))
                ++applicable_choices;
        }
        if (stepped && applies && !realization_mode.empty() && applicable_choices == 0)
            note(out, ForgeAuditFault::missing_choices, node, param.key,
                 "stepped parameter has no named states for realization '" +
                     std::string(realization_mode) + "'");

        const auto found = baked_by_id.find(param.id);
        if (applies && found == baked_by_id.end()) {
            note(out, ForgeAuditFault::stale_descriptor, node, param.key,
                 "describes parameter id " + std::to_string(param.id) +
                     ", which the realization does not bake");
        }

        if (applies && found != baked_by_id.end()) {
            for (const auto& choice : param.choices) {
                if (!realization_mode.empty() &&
                    !choice_applies(choice, realization_mode))
                    continue;
                if (realization_mode.empty() &&
                    !choice.realization_modes.empty())
                    continue;
                if (choice.value < found->second->min_value ||
                    choice.value > found->second->max_value) {
                    note(out, ForgeAuditFault::choice_out_of_range, node, param.key,
                         "state '" + std::string(choice.token) + "' = " +
                             std::to_string(choice.value) + " is outside the baked range [" +
                             std::to_string(found->second->min_value) + ", " +
                             std::to_string(found->second->max_value) + "]");
                }
            }
        }

        for (const auto& mode : param.realization_modes) {
            if (modes.find(mode) == modes.end())
                note(out, ForgeAuditFault::unknown_realization, node, param.key,
                     "restricted to realization '" + std::string(mode) +
                         "', which the node does not declare");
        }
        for (const auto& choice : param.choices) {
            for (const auto& mode : choice.realization_modes) {
                if (modes.find(mode) == modes.end())
                    note(out, ForgeAuditFault::unknown_realization, node, param.key,
                         "choice '" + std::string(choice.token) +
                             "' is restricted to undeclared realization '" +
                             std::string(mode) + "'");
            }
        }
    }

    for (const auto& [key, count] : key_counts)
        if (count > 1)
            note(out, ForgeAuditFault::duplicate_key, node, key,
                 "stable key used by " + std::to_string(count) + " parameters");
    for (const auto& [id, count] : id_counts)
        if (count > 1)
            note(out, ForgeAuditFault::duplicate_id, node, std::to_string(id),
                 "parameter id described " + std::to_string(count) + " times");

    std::unordered_set<state::ParamID> described;
    for (const auto& param : descriptor.params)
        if (realization_mode.empty() || param_applies(param, realization_mode))
            described.insert(param.id);
    // Ordered so the report is stable across runs regardless of hashing.
    std::set<state::ParamID> undescribed;
    for (const auto& param : baked)
        if (described.find(param.id) == described.end())
            undescribed.insert(param.id);
    for (const auto id : undescribed)
        note(out, ForgeAuditFault::undescribed_param, node, std::to_string(id),
             "node bakes parameter id " + std::to_string(id) + " with no semantic descriptor");

    return out;
}

std::string describe_forge_audit_finding(const ForgeAuditFinding& finding) {
    std::string text = std::string(forge_audit_fault_name(finding.fault)) + ": " + finding.node;
    if (!finding.subject.empty())
        text += "." + finding.subject;
    if (!finding.detail.empty())
        text += " — " + finding.detail;
    return text;
}

} // namespace pulp::host
