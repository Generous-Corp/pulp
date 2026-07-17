#pragma once

#include <filesystem>
#include <string>

namespace pulp::import_design {

// Place a --validate / --reference render (and its diff) beside the --output
// file rather than in the process's current directory.
//
// The render path used to be a bare relative name ("<design>-<source>-render.png"),
// so --validate littered whatever directory the CLI ran from — running it from
// a worktree dropped an untracked ~1.4 MB PNG into the repo root, and a
// same-named file already there was clobbered. Prefixing the --output file's
// own directory keeps the artifact with the thing it describes.
//
// When --output has no parent (a bare "ui.js"), the artifact stays in the CWD,
// which is the intended default for that invocation.
inline std::string render_artifact_path(const std::string& output_file,
                                        const std::string& name) {
    const auto dir = std::filesystem::path(output_file).parent_path();
    return dir.empty() ? name : (dir / name).string();
}

}  // namespace pulp::import_design
