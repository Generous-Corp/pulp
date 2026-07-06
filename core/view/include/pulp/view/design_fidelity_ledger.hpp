#pragma once

// Named fidelity taxonomy + per-run ledger.
//
// design_fidelity.hpp computes FidelityIssue findings (skew, dropped-vector, …)
// during codegen. This is the reporting layer on top of them:
//
//   - a NAMED taxonomy: every fidelity kind the checks can emit, each with a
//     stable slug, a default severity, and a one-line description. Tooling and
//     humans get a documented contract instead of ad-hoc `kind` strings.
//   - a per-run LEDGER: a deterministic JSON report of one import's findings
//     with per-kind counts, so an import's fidelity is a durable, diffable
//     artifact rather than transient stderr lines. Warnings are data, not
//     failures — the ledger records everything and lets the consumer decide.
//
// Kept separate from design_fidelity.cpp so the pure checks stay free of JSON
// and reporting concerns.

#include <pulp/view/design_fidelity.hpp>  // FidelityIssue

#include <string>
#include <string_view>
#include <vector>

namespace pulp::view {

/// Format id stamped into the ledger JSON; bump on a non-additive shape change.
inline constexpr std::string_view kFidelityLedgerVersion = "2026.07-fidelity-ledger-v1";

/// One entry in the named fidelity taxonomy: the stable kind slug the checks
/// emit, its typical severity, and what it means.
struct FidelityKindInfo {
    std::string kind;      ///< matches FidelityIssue::kind ("dropped-vector", …)
    std::string severity;  ///< typical severity: "warning" | "info"
    std::string summary;   ///< one-line description of the divergence
};

/// Every fidelity kind the checks can emit, in a stable documentation order.
/// This is the authoritative catalog; a `kind` a finding carries that is absent
/// here is an unregistered kind (the ledger still records it, severity from the
/// finding's own informational flag).
const std::vector<FidelityKindInfo>& fidelity_taxonomy();

/// Look up a kind by slug; nullptr if it is not in the taxonomy.
const FidelityKindInfo* fidelity_kind_info(const std::string& kind);

/// Provenance recorded in the ledger header.
struct FidelityLedgerMeta {
    std::string source;        ///< design source name (figma-plugin, fig, …)
    std::string output;        ///< output artifact path
    std::string generated_at;  ///< caller-supplied timestamp; empty = omitted
};

/// Serialize one import run's findings to a deterministic JSON ledger. A
/// finding's severity is "info" when it is informational (advisory, never gates
/// --strict-fidelity) and "warning" otherwise, so the ledger's warning count
/// equals count_strict_fidelity_failures(). Includes a per-kind summary and the
/// taxonomy so a consumer can render kinds it does not know.
std::string fidelity_ledger_json(const std::vector<FidelityIssue>& issues,
                                 const FidelityLedgerMeta& meta);

}  // namespace pulp::view
