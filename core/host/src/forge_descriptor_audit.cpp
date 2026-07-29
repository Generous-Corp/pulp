#include <pulp/host/forge_descriptor_audit.hpp>

#include <set>
#include <unordered_map>
#include <unordered_set>

namespace pulp::host {
namespace {

void note(std::vector<ForgeAuditFinding>& out, ForgeAuditFault fault,
          std::string_view node, std::string_view subject, std::string detail) {
    out.push_back({fault, std::string(node), std::string(subject), std::move(detail)});
}

}  // namespace

const char* forge_audit_fault_name(ForgeAuditFault fault) {
    switch (fault) {
        case ForgeAuditFault::undescribed_param: return "undescribed_param";
        case ForgeAuditFault::stale_descriptor: return "stale_descriptor";
        case ForgeAuditFault::duplicate_key: return "duplicate_key";
        case ForgeAuditFault::duplicate_id: return "duplicate_id";
        case ForgeAuditFault::missing_choices: return "missing_choices";
        case ForgeAuditFault::choices_on_continuous: return "choices_on_continuous";
        case ForgeAuditFault::choice_out_of_range: return "choice_out_of_range";
        case ForgeAuditFault::unknown_realization: return "unknown_realization";
        case ForgeAuditFault::empty_field: return "empty_field";
        case ForgeAuditFault::no_realizations: return "no_realizations";
    }
    return "unknown";
}

std::vector<ForgeAuditFinding> audit_forge_descriptor(
    const ForgeNodeDescriptor& descriptor,
    const std::vector<CustomNodeBakedParam>& baked) {
    std::vector<ForgeAuditFinding> out;
    const auto node = descriptor.key;

    if (descriptor.key.empty())
        note(out, ForgeAuditFault::empty_field, node, "key", "node key is empty");
    if (descriptor.label.empty())
        note(out, ForgeAuditFault::empty_field, node, "label", "node label is empty");
    if (descriptor.description.empty())
        note(out, ForgeAuditFault::empty_field, node, "description",
             "node description is empty");
    if (descriptor.realizations.empty())
        note(out, ForgeAuditFault::no_realizations, node, "",
             "node declares no realization to build");

    std::unordered_set<std::string_view> modes;
    for (const auto& realization : descriptor.realizations)
        modes.insert(realization.mode);

    std::unordered_map<std::string_view, int> key_counts;
    std::unordered_map<state::ParamID, int> id_counts;
    std::unordered_map<state::ParamID, const CustomNodeBakedParam*> baked_by_id;
    for (const auto& param : baked) baked_by_id[param.id] = &param;

    for (const auto& param : descriptor.params) {
        ++key_counts[param.key];
        ++id_counts[param.id];

        if (param.key.empty())
            note(out, ForgeAuditFault::empty_field, node, "<unnamed>",
                 "parameter key is empty");
        if (param.label.empty())
            note(out, ForgeAuditFault::empty_field, node, param.key,
                 "parameter label is empty");
        if (param.description.empty())
            note(out, ForgeAuditFault::empty_field, node, param.key,
                 "parameter description is empty");

        const auto found = baked_by_id.find(param.id);
        if (found == baked_by_id.end()) {
            note(out, ForgeAuditFault::stale_descriptor, node, param.key,
                 "describes parameter id " + std::to_string(param.id) +
                     ", which the node does not bake");
        }

        const bool stepped = param.kind == ForgeParamKind::stepped;
        if (stepped && param.choices.empty())
            note(out, ForgeAuditFault::missing_choices, node, param.key,
                 "stepped parameter declares no named states");
        if (!stepped && !param.choices.empty())
            note(out, ForgeAuditFault::choices_on_continuous, node, param.key,
                 "continuous parameter declares named states");

        if (found != baked_by_id.end()) {
            for (const auto& choice : param.choices) {
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
    for (const auto& param : descriptor.params) described.insert(param.id);
    // Ordered so the report is stable across runs regardless of hashing.
    std::set<state::ParamID> undescribed;
    for (const auto& param : baked)
        if (described.find(param.id) == described.end()) undescribed.insert(param.id);
    for (const auto id : undescribed)
        note(out, ForgeAuditFault::undescribed_param, node, std::to_string(id),
             "node bakes parameter id " + std::to_string(id) +
                 " with no semantic descriptor");

    return out;
}

std::vector<ForgeAuditFinding> audit_forge_catalog(
    const std::vector<ForgeNodeDescriptor>& descriptors,
    const std::vector<std::vector<CustomNodeBakedParam>>& baked_per_node) {
    std::vector<ForgeAuditFinding> out;
    const auto count = std::min(descriptors.size(), baked_per_node.size());
    for (std::size_t i = 0; i < count; ++i) {
        auto findings = audit_forge_descriptor(descriptors[i], baked_per_node[i]);
        out.insert(out.end(), findings.begin(), findings.end());
    }

    std::unordered_map<std::string_view, int> node_keys;
    for (const auto& descriptor : descriptors) ++node_keys[descriptor.key];
    for (const auto& [key, n] : node_keys)
        if (n > 1)
            note(out, ForgeAuditFault::duplicate_key, key, "",
                 "node key declared by " + std::to_string(n) + " catalog entries");

    return out;
}

std::string describe_forge_audit_finding(const ForgeAuditFinding& finding) {
    std::string text = std::string(forge_audit_fault_name(finding.fault)) + ": " +
                       finding.node;
    if (!finding.subject.empty()) text += "." + finding.subject;
    if (!finding.detail.empty()) text += " — " + finding.detail;
    return text;
}

}  // namespace pulp::host
