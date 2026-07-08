#include <pulp/runtime/search_index.hpp>

#include <algorithm>
#include <optional>

namespace pulp::runtime {

namespace {

// Score tiers. Kept far enough apart that intra-tier position/length bonuses
// (each bounded well under 200) can never let a weaker match outrank a
// stronger one.
constexpr int kExactScore = 1000;
constexpr int kPrefixScore = 800;
constexpr int kSubstringScore = 600;
constexpr int kSubsequenceScore = 200;

char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

std::string ascii_lowered(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out.push_back(ascii_lower(c));
    return out;
}

// Reward items whose length is close to the query length: a 5-char query
// matching a 5-char item is a better hit than the same query inside a
// 200-char item. Bounded to [0, 64].
int length_bonus(std::size_t needle_len, std::size_t haystack_len) {
    const auto extra = static_cast<long>(haystack_len) - static_cast<long>(needle_len);
    return static_cast<int>(std::max<long>(0, 64 - extra));
}

// Non-contiguous subsequence match. Returns a bonus in [0, 120] when every
// character of `needle` appears in `haystack` in order, or nullopt otherwise.
// Rewards contiguous runs and an early first match.
std::optional<int> subsequence_bonus(std::string_view needle, std::string_view haystack) {
    std::size_t hi = 0;
    int first_pos = -1;
    int contiguous = 0;
    long prev_match = -2;  // index in haystack of the previously matched char

    for (char c : needle) {
        bool matched = false;
        while (hi < haystack.size()) {
            if (haystack[hi] == c) {
                if (first_pos < 0) first_pos = static_cast<int>(hi);
                if (static_cast<long>(hi) == prev_match + 1) ++contiguous;
                prev_match = static_cast<long>(hi);
                ++hi;
                matched = true;
                break;
            }
            ++hi;
        }
        if (!matched) return std::nullopt;
    }

    int bonus = contiguous * 8 - std::min(first_pos, 40);
    return std::clamp(bonus, 0, 120);
}

// Score one item against an already-normalized query. nullopt = no match.
std::optional<int> score_item(std::string_view needle, std::string_view haystack,
                              bool fuzzy) {
    if (needle == haystack) return kExactScore;

    if (haystack.size() >= needle.size() &&
        haystack.compare(0, needle.size(), needle) == 0) {
        return kPrefixScore + length_bonus(needle.size(), haystack.size());
    }

    const auto pos = haystack.find(needle);
    if (pos != std::string_view::npos) {
        return kSubstringScore - std::min<int>(static_cast<int>(pos), 80) +
               length_bonus(needle.size(), haystack.size());
    }

    if (fuzzy) {
        if (auto bonus = subsequence_bonus(needle, haystack))
            return kSubsequenceScore + *bonus;
    }
    return std::nullopt;
}

}  // namespace

void SearchIndex::build(std::vector<std::string> items, const CancellationToken& cancel) {
    items_ = std::move(items);
    normalized_.clear();
    normalized_.reserve(items_.size());
    for (std::size_t i = 0; i < items_.size(); ++i) {
        if ((i & 0x3ff) == 0 && cancel.is_cancelled()) return;
        normalized_.push_back(ascii_lowered(items_[i]));
    }
}

std::vector<SearchIndex::Match> SearchIndex::query(std::string_view query_text,
                                                   const SearchQueryOptions& options,
                                                   const CancellationToken& cancel) const {
    std::vector<Match> matches;

    // Empty query: every item, original order.
    if (query_text.empty()) {
        const std::size_t count = options.max_results == 0
                                      ? items_.size()
                                      : std::min(options.max_results, items_.size());
        matches.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            if ((i & 0x3ff) == 0 && cancel.is_cancelled()) return matches;
            matches.push_back({i, 0});
        }
        return matches;
    }

    const std::string needle_owned =
        options.case_sensitive ? std::string(query_text) : ascii_lowered(query_text);
    const std::string_view needle = needle_owned;

    for (std::size_t i = 0; i < items_.size(); ++i) {
        if ((i & 0x3ff) == 0 && cancel.is_cancelled()) return {};

        const std::string_view haystack =
            options.case_sensitive ? std::string_view(items_[i])
                                   : std::string_view(normalized_[i]);
        if (auto score = score_item(needle, haystack, options.fuzzy))
            matches.push_back({i, *score});
    }

    std::sort(matches.begin(), matches.end(), [this](const Match& a, const Match& b) {
        if (a.score != b.score) return a.score > b.score;
        const auto la = items_[a.index].size();
        const auto lb = items_[b.index].size();
        if (la != lb) return la < lb;
        return a.index < b.index;
    });

    if (options.max_results != 0 && matches.size() > options.max_results)
        matches.resize(options.max_results);

    return matches;
}

}  // namespace pulp::runtime
