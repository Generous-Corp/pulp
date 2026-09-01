#include <pulp/authoring_capsule/component.hpp>
#include <pulp/authoring_capsule/manifest.hpp>

#include <cstdint>
#include <string_view>
#include <vector>

namespace pulp::authoring_capsule {
namespace {

/// One component row, flattened from `files[]` and `dependencies[]` so the
/// derivation reads the same facts whether the bytes travel in the archive or
/// are named by digest.
struct Row {
    const ComponentPolicy* policy = nullptr;
    /// A resolver for absent bytes. Empty for a file row: only a dependency
    /// carries a provider, so a file row that declares itself `external` names
    /// no way to fetch it and counts as missing rather than resolvable.
    std::string_view provider;
};

bool required_for(const ComponentPolicy& policy, RequiredFor operation) {
    for (auto entry : policy.required_for)
        if (entry == operation) return true;
    return false;
}

}  // namespace

/// Completeness is recomputed from the rows every time. A capsule that
/// declares its own value is ignored, because the declaration and the rows can
/// disagree and only the rows are checkable.
///
/// `DependencyEntry::required` is a coarse hint; `policy.required_for` is the
/// finer authority for which operations a component gates.
///
/// Redistribution, though, is judged across EVERY row, not only the ones that
/// gate an operation. `required_for` is optional, so a verdict that consulted
/// only gating rows could be forged by omitting the field everywhere: each row
/// would be skipped, nothing would contradict the initial `true`, and a capsule
/// full of unknown-rights components would report `self_contained` — which
/// `component.hpp` defines as "every required byte is present and may be
/// redistributed". The same preview would simultaneously list those rows in
/// `rights.blocking_component_paths`, so the two derived facts would flatly
/// contradict each other and the permissive one would be the headline. Judging
/// rights across all rows closes that, and it is also the honest reading:
/// `unknown` rights anywhere mean the capsule is not one you may pass on.
Completeness derive_completeness(const Manifest& manifest) {
    std::vector<Row> rows;
    rows.reserve(manifest.files.size() + manifest.dependencies.size());
    for (const auto& file : manifest.files) rows.push_back(Row{&file.policy, {}});
    for (const auto& dependency : manifest.dependencies)
        rows.push_back(Row{&dependency.policy, dependency.provider});

    // Vacuously true for an empty capsule only. Any row present in the archive
    // whose rights are not an explicit grant clears it, gating or not.
    bool every_required_row_present_and_redistributable = true;
    bool any_required_row_absent = false;
    bool every_absent_required_row_resolvable = true;
    bool play_set_complete = true;
    bool rebuild_canonical_input_absent = false;

    for (const auto& row : rows) {
        const auto& policy = *row.policy;

        // `unknown` is a real state and never decays to `allowed`: only an
        // explicit grant clears the redistribution bar. Checked before the
        // gating filter so a component that gates nothing still cannot be
        // carried into a self-contained claim on rights nobody granted.
        if (policy.source_availability == SourceAvailability::included &&
            !policy.redistribution.is_granted())
            every_required_row_present_and_redistributable = false;

        const bool gates_play = required_for(policy, RequiredFor::play);
        const bool gates_rebuild = required_for(policy, RequiredFor::rebuild);
        if (!gates_play && !gates_rebuild) continue;

        const bool present = policy.source_availability == SourceAvailability::included;
        const bool redistributable = policy.redistribution.is_granted();
        const bool resolvable = policy.source_availability == SourceAvailability::external &&
                                !row.provider.empty();

        if (!present || !redistributable) every_required_row_present_and_redistributable = false;

        if (!present) {
            any_required_row_absent = true;
            if (!resolvable) every_absent_required_row_resolvable = false;
        }

        // Playing needs the bytes, not the right to pass them on, so the play
        // set is judged on presence alone.
        if (gates_play && !present) play_set_complete = false;

        if (gates_rebuild && !present && policy.canonicality == Canonicality::canonical_input)
            rebuild_canonical_input_absent = true;
    }

    if (every_required_row_present_and_redistributable) return Completeness::self_contained;

    // Resolvable outranks play-only: a stable identity plus a provider is a
    // stronger promise than a rendition that merely plays.
    if (any_required_row_absent && every_absent_required_row_resolvable)
        return Completeness::resolvable;

    if (play_set_complete && rebuild_canonical_input_absent) return Completeness::play_only;

    return Completeness::partial;
}

}  // namespace pulp::authoring_capsule
