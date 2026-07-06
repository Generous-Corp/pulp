// design_fidelity_ledger.cpp — named fidelity taxonomy + per-run JSON ledger.

#include <pulp/view/design_fidelity_ledger.hpp>

#include <choc/text/choc_JSON.h>

#include <map>

namespace pulp::view {

const std::vector<FidelityKindInfo>& fidelity_taxonomy() {
    // Stable documentation order. Each kind mirrors an invariant in
    // design_fidelity.hpp; the two must stay in step when a check is added.
    static const std::vector<FidelityKindInfo> kTaxonomy = {
        {"skew", "warning",
         "A bleed sprite was emitted at an aspect ratio that does not match its source PNG."},
        {"aspect-unverified", "warning",
         "A sprite's aspect could not be verified because the source PNG dimensions were missing."},
        {"gross-size", "warning",
         "A node pinned on both axes was emitted more than 3x its source box."},
        {"widget-size", "warning",
         "A recognized widget's emitted box diverges more than 1.5x from its source intrinsic size."},
        {"widget-undersized", "info",
         "A widget's source was below its native usable minimum; codegen clamped it up."},
        {"text-vcenter", "warning",
         "A single-line label in a tall slot was emitted top-aligned instead of vertically centered."},
        {"dropped-vector", "warning",
         "A vector/path node with no renderable primitive would be dropped to an empty frame."},
    };
    return kTaxonomy;
}

const FidelityKindInfo* fidelity_kind_info(const std::string& kind) {
    for (const auto& info : fidelity_taxonomy())
        if (info.kind == kind) return &info;
    return nullptr;
}

std::string fidelity_ledger_json(const std::vector<FidelityIssue>& issues,
                                 const FidelityLedgerMeta& meta) {
    auto root = choc::value::createObject("");
    root.addMember("format_version", std::string(kFidelityLedgerVersion));
    root.addMember("source", meta.source);
    root.addMember("output", meta.output);
    if (!meta.generated_at.empty()) root.addMember("generated_at", meta.generated_at);

    // Per-finding severity is driven by the informational flag so the warning
    // count equals count_strict_fidelity_failures(). Summary counts are keyed in
    // a std::map for deterministic (sorted) key order.
    std::map<std::string, int> by_kind;
    int warnings = 0;
    int infos = 0;

    auto findings = choc::value::createEmptyArray();
    for (const auto& fi : issues) {
        const bool info = fi.informational;
        (info ? infos : warnings) += 1;
        by_kind[fi.kind] += 1;

        auto obj = choc::value::createObject("");
        obj.addMember("kind", fi.kind);
        obj.addMember("severity", info ? std::string("info") : std::string("warning"));
        obj.addMember("node_id", fi.node_id);
        obj.addMember("node_name", fi.node_name);
        obj.addMember("detail", fi.detail);
        obj.addMember("informational", info);
        findings.addArrayElement(obj);
    }

    auto summary = choc::value::createObject("");
    summary.addMember("total", static_cast<int>(issues.size()));
    summary.addMember("warnings", warnings);  // the hard findings that gate --strict-fidelity
    summary.addMember("informational", infos);
    auto counts = choc::value::createObject("");
    for (const auto& [kind, n] : by_kind) counts.addMember(kind, n);
    summary.addMember("by_kind", counts);
    root.addMember("summary", summary);

    root.addMember("findings", findings);

    auto taxonomy = choc::value::createEmptyArray();
    for (const auto& info : fidelity_taxonomy()) {
        auto obj = choc::value::createObject("");
        obj.addMember("kind", info.kind);
        obj.addMember("severity", info.severity);
        obj.addMember("summary", info.summary);
        taxonomy.addArrayElement(obj);
    }
    root.addMember("taxonomy", taxonomy);

    return choc::json::toString(root, /*pretty=*/true);
}

}  // namespace pulp::view
