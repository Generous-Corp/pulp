// SPDX-License-Identifier: MIT
//
// Framework detection engine for `pulp import detect`.
//
// Detection markers are runtime DATA loaded from a known-frameworks index
// (tools/import/known-frameworks.json by default, or $PULP_KNOWN_FRAMEWORKS
// for tests). The SDK names no framework and no vendor in code — this header
// only knows the *shape* of a marker, never a specific marker's content.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace pulp::cli::import_detect {

namespace fs = std::filesystem;

enum class MarkerType { FileGlob, ContentMatch };

struct Marker {
    MarkerType type = MarkerType::FileGlob;
    std::string pattern;   // glob (FileGlob) or literal substring (ContentMatch)
    std::string in_glob;   // ContentMatch: restrict to files matching this glob
    double weight = 0.0;
};

struct FrameworkEntry {
    std::string framework_id;
    std::string display_name;
    std::string importer_tool_id;
    int spi_min = 0;
    int spi_max = 0;
    std::vector<Marker> detection;
};

struct KnownFrameworks {
    std::string error;       // non-empty on load failure
    std::vector<FrameworkEntry> frameworks;
};

// Parse a known-frameworks index from JSON text. Pure — unit-tested directly.
KnownFrameworks parse_index(const std::string& json_text);

// Load the index from `path`. Thin wrapper over parse_index over file read.
KnownFrameworks load_index(const fs::path& path);

// Merge every index an INSTALLED add-on importer contributes.
//
// The SDK ships no detection markers of its own (tools/import/known-frameworks.json
// has an empty `frameworks` list, deliberately). Markers are the property of an
// importer, which is installed by URL and drops its own known-frameworks.json into
// `~/.pulp/tools/<tool_id>/`. This walks that directory and merges what it finds.
//
// The consequence, which is the point: with no importer installed, detection matches
// nothing and reveals nothing. The SDK cannot say which importers exist, nor whether
// any given one is public or private, because it genuinely does not know — it knows
// only the SHAPE of a marker. A user with access and a user without get byte-identical
// behaviour from the SDK.
//
// `tools_root` is `~/.pulp/tools` in production; tests pass a temp dir.
KnownFrameworks merge_installed_indices(const fs::path& tools_root);

// Locate the known-frameworks index. Resolution order:
//   1. $PULP_KNOWN_FRAMEWORKS (explicit override; tests + custom installs)
//   2. tools/import/known-frameworks.json walking up from `start_dir`
//   3. tools/import/known-frameworks.json walking up from the executable dir
// Returns an empty path when none is found.
fs::path find_index(const fs::path& start_dir, const fs::path& exe_dir);

// A ranked detection candidate.
struct Candidate {
    std::string framework_id;
    std::string display_name;
    std::string importer_tool_id;
    int spi_min = 0;
    int spi_max = 0;
    double confidence = 0.0;            // normalised to [0,1]
    std::vector<std::string> evidence;  // human-facing marker hits
};

// glob match: '*' matches any run of non-'/' chars, '**' matches across '/',
// '?' matches one non-'/' char. Anchored to the whole relative path.
bool glob_match(const std::string& glob, const std::string& path);

// Scan `project_dir` against every framework's markers and return candidates
// sorted by descending confidence (only those with confidence > 0). The walk
// is read-only and bounded (skips common build/vendor dirs, caps file reads).
std::vector<Candidate> detect(const fs::path& project_dir,
                              const KnownFrameworks& index);

}  // namespace pulp::cli::import_detect
