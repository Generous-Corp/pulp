// Offline .fig import lane for pulp-import-design.
//
// `--from fig` decodes a local Figma save file offline (no account, MCP, or
// network) into the same figma-plugin envelope the in-editor plugin emits, then
// hands off to the standard figma-plugin path. The container/kiwi/zstd decode
// itself lives in a Node subtool (fig_decode.mjs); this module locates and
// drives it, keeping the CLI's main translation unit free of the lane's plumbing.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace pulp::import_design::fig {

/// Inputs the fig lane needs from the CLI. `source_str` and `input_file` are
/// rewritten in place when a frame is decoded (source becomes "figma-plugin",
/// file becomes the emitted scene.pulp.json), so the caller continues down the
/// existing figma-plugin path unchanged.
struct LaneArgs {
    std::string& source_str;
    std::string& input_file;
    /// Frames to capture, in the order `--frame` was given. One entry is the
    /// ordinary single-state import. Two or more capture a MULTI-STATE design:
    /// each frame is decoded on its own and merged into a single envelope whose
    /// root carries the rest as `alternate_frames`, so the frame index a `swap`
    /// element targets is this vector's index (frames[0] is frame 0).
    std::vector<std::string> frame_names;
    std::string page_name;
    bool outline_mode = false;
    bool outline_json = false;
    /// When a frame is decoded, set to the scratch directory holding the
    /// generated scene.pulp.json + assets so the caller can remove it once the
    /// import has finished reading them. Left untouched on the outline path.
    std::string* created_tmp_dir = nullptr;
    /// When a frame is decoded, set to the decoder's `geometry.json` — Figma's
    /// own solved rect for every emitted node, keyed by the same node_id the
    /// envelope carries. It lives in the scratch directory and dies with it, so
    /// a caller that wants it (--dump-layout) must copy it out before returning.
    std::string* geometry_file = nullptr;
};

/// Handle `--from fig`, and reject `--outline` on any other source. Returns an
/// exit code the caller should return immediately (an outline was printed, or a
/// usage/decode error occurred), or std::nullopt to continue normally with
/// `args.source_str` / `args.input_file` now pointing at the decoded envelope.
std::optional<int> handle(const LaneArgs& args);

}  // namespace pulp::import_design::fig
