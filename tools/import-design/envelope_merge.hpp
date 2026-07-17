// Merging N per-state design envelopes into one multi-state envelope.
//
// A multi-state capture is N ordinary single-frame envelopes — one per state —
// folded into a single envelope whose root carries the rest as
// `alternate_frames`. The merge is deliberately source-agnostic: it only needs
// the figma-plugin envelope shape ({provenance, tokens, asset_manifest,
// diagnostics, root}), so any lane that can produce one envelope per state can
// produce a multi-state import without its own merge code.

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace pulp::import_design {

/// Merge `envelopes` (>= 1, in capture order) into a single envelope at
/// `out_path`, staging every input's `assets/` beside it under `scratch`.
///
/// Envelope 0 supplies every top-level field (provenance, tokens, library
/// manifest); the rest contribute only their root node — appended to
/// `root.alternate_frames` IN ORDER, because that order is the frame index a
/// `swap` element's target_frame names — plus their assets and diagnostics.
///
/// Returns nullopt on success, or a process exit code on failure (diagnosed to
/// stderr).
std::optional<int> merge_frame_envelopes(const std::vector<std::filesystem::path>& envelopes,
                                         const std::filesystem::path& scratch,
                                         const std::filesystem::path& out_path);

/// A per-run, hard-to-predict scratch directory named after `input_file`'s stem.
/// The monotonic tick keeps two concurrent imports of the same file from
/// colliding and stops a local attacker from pre-planting a symlink at a
/// guessable path that a subsequent write would follow.
std::filesystem::path make_scratch_dir(const std::string& tag, const std::string& input_file);

}  // namespace pulp::import_design
