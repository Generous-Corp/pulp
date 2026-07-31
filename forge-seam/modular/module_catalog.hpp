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

#include <string>
#include <vector>

#include "forge/mention_overlay.hpp"

namespace forge_modular {

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

}  // namespace forge_modular
