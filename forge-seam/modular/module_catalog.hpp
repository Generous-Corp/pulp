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

#include <cstddef>
#include <ctime>
#include <functional>
#include <optional>
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

/// How to name a module to a person: the maker, then the module.
///
/// Prepending the brand unconditionally produced "CV funk CV funk Blank 8HP",
/// because VCV module names very often already lead with the maker's own name.
/// So the brand goes on only when the name does not already start with it.
std::string module_label(const std::string& brand, const std::string& name);

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

/// Where the builder records how it went. A background job that fails in
/// silence is the whole of this defect: the app asked for an index, a stale
/// generator printed its usage and exited 2, and nothing on the machine said
/// so for four days.
std::string library_index_status_path();

/// How much is in an index file.
struct IndexCounts {
    std::size_t plugins = 0;
    std::size_t modules = 0;
};

/// Count an index without going through the search path. Pure, so the floors
/// below can be asserted against a known document.
IndexCounts count_index_text(const std::string& text);

// The published library is ~420 plugins and ~4,300 modules and only grows.
// A file holding a small fraction of that is a broken build wearing a current
// timestamp -- 200 plugins and no CV funk in it passed a freshness check for
// four days because the check only looked at the clock.
//
// The floors sit far below the real library and far above the broken artifact,
// so ordinary churn can never trip them and a truncated fetch always does.
inline constexpr std::size_t kMinPlausiblePlugins = 250;
inline constexpr std::size_t kMinPlausibleModules = 2000;

/// Is this index big enough to be the real library?
bool index_is_plausible(const IndexCounts& counts);

/// What the index on this machine currently holds. Cached on the file's size
/// and write time, so a status line may ask on every tick.
struct LibraryIndexState {
    bool present = false;
    IndexCounts counts;
    std::time_t written = 0;   ///< 0 when there is no index
};
LibraryIndexState library_index_state();

/// Should the index be built?
///
/// Missing is the first-run case; stale is the everyday one, because VCV
/// publishes new plugins continuously and an index built once is wrong within
/// weeks. IMPLAUSIBLE is the third, and it is the one that was missing: an
/// index far below the real library is broken whatever its mtime says.
///
/// Takes `now` rather than reading the clock, so a test can age a file without
/// waiting a week for it.
bool library_index_needs_build(const std::string& path, std::time_t now,
                               int max_age_days = 7);

/// The command that builds it, for a given toolchain directory. It records its
/// own exit status, so the app can say a refresh failed instead of leaving
/// somebody to wonder why nothing changed.
std::string library_index_command(const std::string& tools_dir);

/// What the last build reported: the exit status, or no value when none has
/// run since this machine last had none.
std::optional<int> library_index_last_status();

/// Build the index when it is missing or stale, using the caller's launcher.
///
/// Returns the command issued, or "" when nothing was needed. The launcher
/// belongs to the caller because only the shell knows how to detach a process,
/// and this file has to stay testable without spawning one.
std::string ensure_library_index(const std::string& tools_dir,
                                 const std::function<void(const std::string&)>& run,
                                 std::time_t now = std::time(nullptr));

/// Build it whatever its state -- what the Refresh control does. Somebody who
/// presses a button has already decided the answer to "is it needed".
std::string build_library_index(const std::string& tools_dir,
                                const std::function<void(const std::string&)>& run);

}  // namespace forge_modular
