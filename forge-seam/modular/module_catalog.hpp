#pragma once

// Every module we know of: the ones installed, and the ones merely published.
//
// Two sources, deliberately kept apart because they answer different
// questions. The Rack plugin directories say what can be WIRED right now. The
// library catalog -- fetched by tools/rack/library_catalog.py from VCV's
// public manifest and the plugins' own repositories -- says what EXISTS, which
// is most of the library and costs nothing to know.
//
// Being able to name a module you do not own is not a dead end. It is how you
// find out the thing you want exists, and what it is called.

#include <ctime>
#include <functional>
#include <string>
#include <vector>

#include "forge/mention_overlay.hpp"

namespace forge_modular {

/// One module, from either source.
///
/// Public so the search can be tested against a known library rather than
/// against whatever this machine happens to have installed. Every ordering
/// mistake this file has made was a mistake about a LIST, and a list assembled
/// from the developer's own Rack folder cannot be asserted about.
struct ModuleEntry {
    std::string brand;      ///< "Audible Instruments", "CV funk"
    std::string name;       ///< "Macro Oscillator"
    std::string slug;       ///< "AudibleInstruments/Braids"
    bool installed = false;
};

/// Search a known list. Matching and ranking live here; the filesystem does not.
std::vector<MentionCandidate> search_entries(const std::vector<ModuleEntry>& entries,
                                             const std::string& query,
                                             std::size_t limit = 40);

/// Search everything known, installed first.
///
/// Installed modules rank above catalogued ones because only they can be wired
/// into a patch that will sound -- offering an uninstallable module above one
/// that works would be ranking novelty over usefulness.
std::vector<MentionCandidate> search_modules(const std::string& query,
                                             std::size_t limit = 40);

/// How many modules each source knows about. For a status line, and for a test
/// that wants to assert the catalog is actually being read rather than that
/// the search happens to return something.
struct CatalogCounts {
    std::size_t installed = 0;
    std::size_t catalogued = 0;
};
CatalogCounts catalog_counts();

// ── the library index ────────────────────────────────────────────────────────
//
// The index is written by tools/rack/library_catalog.py and read by the search
// above. It was written by nobody: the script existed, the reader existed, and
// no code path joined them -- so the @ list offered only what was already
// installed, and the whole download capability was unreachable from the UI. The
// join belongs here, beside the reader, so there is one answer to the question
// "where does that file come from".

/// Where the index lives. One answer, so a second one cannot drift from it.
std::string library_index_path();

/// Should the index be built?
///
/// Missing is the first-run case; stale is the everyday one, because VCV
/// publishes new plugins continuously and an index built once is wrong within
/// weeks. Takes `now` rather than reading the clock, so a test can age a file
/// without waiting a week for it.
bool library_index_needs_build(const std::string& path, std::time_t now,
                               int max_age_days = 7);

/// The command that builds it, for a given toolchain directory.
std::string library_index_command(const std::string& tools_dir);

/// Build the index when it is missing or stale, using the caller's launcher.
///
/// Returns the command issued, or "" when nothing was needed. The launcher
/// belongs to the caller because only the shell knows how to detach a process,
/// and this file has to stay testable without spawning one.
std::string ensure_library_index(const std::string& tools_dir,
                                 const std::function<void(const std::string&)>& run,
                                 std::time_t now = std::time(nullptr));

}  // namespace forge_modular
