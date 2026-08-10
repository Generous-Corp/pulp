#pragma once

// A Rack patch file, turned into something the preview can draw.
//
// The .vcv is the artifact the user actually gets, so the preview reads that
// rather than a parallel description the generator also emits. One source: a
// preview that drew from a side-channel could disagree with the file Rack
// opens, and the disagreement would favour the prettier of the two.

#include "forge/rack_layout.hpp"

#include <string>
#include <vector>

namespace forge_modular {

struct LoadedPatch {
    std::vector<RackModule> modules;
    std::vector<Connection> connections;
    /// Why nothing loaded, or empty. A value rather than a log line, because a
    /// screen that shows an empty rack for an unreadable file is lying.
    std::string error;

    bool ok() const { return error.empty() && !modules.empty(); }
};

/// Read a Rack patch. Never throws: a malformed file returns an error string.
LoadedPatch load_patch(const std::string& path);

}  // namespace forge_modular
