#pragma once

// pulp::design — variant sets -> typed parameterized component contract.
//
// A design-tool component set (a Figma variant set, or the equivalent in a
// decoded `.fig`) names each member with a `Prop=Value` string, e.g.
// "Size=Large, State=Hover". Imported naively that is N frozen frames. This
// translation unit collapses the set into one *typed* component contract: each
// property with its observed value enum and a chosen default. That contract is
// the supply side R1's manifest and R2's adherence lint consume — a widget the
// importer can bind to by prop, instead of a wall of one-off frames.
//
// It is pure and deterministic: it takes the variant *names* (which both intake
// lanes already produce) and computes the prop model. Resolving per-variant
// style deltas from instance overrides needs the decoded node geometry and is
// deliberately out of scope here — this layer models well-formed variant sets
// first and emits named issues for anything it could not cleanly collapse, so a
// downstream fidelity ledger (design_fidelity_ledger) can report what was baked.

#include <string>
#include <string_view>
#include <vector>

namespace pulp::design {

/// Format id stamped into the contract; bump on a non-additive shape change.
inline constexpr std::string_view kVariantContractVersion = "2026.07-variant-contract-v1";

/// One property parsed from a variant name: "Size=Large" -> {"Size","Large"}.
struct VariantProperty {
    std::string name;
    std::string value;
};

/// Parse a component-set variant name ("Size=Large, State=Hover") into ordered
/// properties. Names and values are trimmed. A comma segment with no '=' is
/// skipped here (the contract builder records it as a `malformed-name` issue).
std::vector<VariantProperty> parse_variant_name(std::string_view name);

/// One property in the synthesized contract: its name, the sorted-unique value
/// set observed across the set (its enum), and the chosen default value.
struct ContractProp {
    std::string name;
    std::vector<std::string> values;  ///< sorted, unique
    std::string default_value;        ///< see default rules on build_component_contract
};

/// A named issue about a variant set that could not be cleanly collapsed. Kinds:
///   "single-variant"     — the set has fewer than two members (not really a set)
///   "malformed-name"     — a variant name had a segment with no `Prop=Value`
///   "inconsistent-props" — a variant's property *keys* differ from the set's
///                          modal key set (a mixed/ill-formed set)
struct VariantIssue {
    std::string kind;
    std::string detail;
};

/// The typed component contract synthesized from a variant set.
struct ComponentContract {
    std::string format_version;         ///< kVariantContractVersion
    std::string component;              ///< the component-set name
    std::vector<ContractProp> props;    ///< sorted by property name
    int variant_count = 0;
    std::vector<VariantIssue> issues;   ///< in a stable (kind, detail) order
};

/// Build a contract from a component-set name and its member variant names.
/// Default value per property, in priority order:
///   1. an explicit "Default" value if the enum contains one (case-insensitive);
///   2. for a boolean-shaped enum ({On,Off}/{True,False}/{Yes,No}) the falsey one;
///   3. otherwise the value that appears in the most variants (ties: sorted first).
ComponentContract build_component_contract(std::string_view component,
                                           const std::vector<std::string>& variant_names);

/// Serialize a contract to deterministic JSON (feeds R1's manifest / R2's lint).
std::string component_contract_json(const ComponentContract& contract);

}  // namespace pulp::design
