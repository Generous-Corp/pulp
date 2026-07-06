#pragma once

// SearchIndex — ranked substring / prefix / fuzzy search over a fixed list of
// strings.
//
// This is the concrete "index and filter a large library" workload behind the
// off-UI-thread query service (see core/view/.../query_service.hpp). It is a
// pure data structure with no threads of its own: `build()` does the one-time
// normalization work and `query()` is a `const` scan, so callers are free to
// run either on a background thread and marshal the result back themselves.
//
// Ranking, highest first: exact match > prefix match > substring match >
// subsequence ("fuzzy") match. Within a tier, shorter items and earlier match
// positions rank higher; ties break on the item's original index so results
// are fully deterministic. An empty query returns every item in original
// order.
//
// Matching is ASCII-case-insensitive by default. Unicode is compared
// byte-for-byte (no case folding beyond ASCII) — deliberate, to keep results
// deterministic and locale-independent.

#include <pulp/runtime/async_stream.hpp>  // CancellationToken

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::runtime {

struct SearchQueryOptions {
    /// Cap on the number of matches returned. 0 means unlimited.
    std::size_t max_results = 0;

    /// Allow non-contiguous subsequence ("fuzzy") matches. When false, an
    /// item must contain the query as a contiguous substring to match.
    bool fuzzy = true;

    /// Match with ASCII case sensitivity. Defaults to case-insensitive.
    bool case_sensitive = false;
};

class SearchIndex {
public:
    struct Match {
        std::size_t index;  ///< position in the original `items` list
        int score;          ///< higher is a better match
    };

    SearchIndex() = default;

    /// Replace the indexed set. Precomputes an ASCII-lowercased copy of each
    /// item for case-insensitive matching. Intended to run on a background
    /// thread for large libraries, so it polls `cancel` and returns early when
    /// signalled (the partially-built index must then be discarded, not used).
    void build(std::vector<std::string> items, const CancellationToken& cancel = {});

    std::size_t size() const { return items_.size(); }
    bool empty() const { return items_.empty(); }

    /// The original text of item `i`. Caller must ensure `i < size()`.
    const std::string& item(std::size_t i) const { return items_[i]; }

    /// Rank every item against `query`. Periodically checks `cancel`; if
    /// cancellation is observed the scan returns early with whatever it has
    /// gathered so far (callers should discard a cancelled result). An empty
    /// query returns items in original order, bounded by `max_results`.
    std::vector<Match> query(std::string_view query,
                             const SearchQueryOptions& options = {},
                             const CancellationToken& cancel = {}) const;

private:
    std::vector<std::string> items_;       ///< original text, original order
    std::vector<std::string> normalized_;  ///< ASCII-lowercased, 1:1 with items_
};

}  // namespace pulp::runtime
