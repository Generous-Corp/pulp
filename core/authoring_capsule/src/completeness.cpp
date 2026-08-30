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
/// finer authority and the only input consulted here, so a row that gates no
/// operation cannot downgrade the verdict.
Completeness derive_completeness(const Manifest& manifest) {
    std::vector<Row> rows;
    rows.reserve(manifest.files.size() + manifest.dependencies.size());
    for (const auto& file : manifest.files) rows.push_back(Row{&file.policy, {}});
    for (const auto& dependency : manifest.dependencies)
        rows.push_back(Row{&dependency.policy, dependency.provider});

    // Vacuously true when nothing gates play or rebuild: a capsule that
    // requires no component for either operation is trivially self-contained.
    bool every_required_row_present_and_redistributable = true;
    bool any_required_row_absent = false;
    bool every_absent_required_row_resolvable = true;
    bool play_set_complete = true;
    bool rebuild_canonical_input_absent = false;

    for (const auto& row : rows) {
        const auto& policy = *row.policy;
        const bool gates_play = required_for(policy, RequiredFor::play);
        const bool gates_rebuild = required_for(policy, RequiredFor::rebuild);
        if (!gates_play && !gates_rebuild) continue;

        const bool present = policy.source_availability == SourceAvailability::included;
        // `unknown` is a real state and never decays to `allowed`: only an
        // explicit grant clears the redistribution bar.
        const bool redistributable = policy.redistribution == Redistribution::allowed;
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
