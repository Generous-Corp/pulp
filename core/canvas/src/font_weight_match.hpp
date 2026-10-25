#pragma once

// CSS font-weight matching (CSS Fonts 4, "Font style matching", step 4).
//
// Platform font managers match a requested weight to the NEAREST face. CSS
// does not: it searches in a fixed direction from the requested weight. The
// two disagree whenever a family lacks the exact weight. Helvetica Neue ships
// Medium (500) and Bold (700); asked for 600, CoreText picks Medium and every
// browser picks Bold. Text imported from a browser then paints lighter and
// narrower than its capture, and the face no longer matches the capture's
// recorded face, so the captured line layout is rejected as well.
//
// Every place Pulp picks a face from a family's style set goes through
// css_match_font_weight(), so the face a paragraph paints with and the face
// the resolver reports for a captured-basis check cannot differ.

#include <climits>
#include <span>

#ifdef PULP_HAS_SKIA
#include "include/core/SkFontMgr.h"
#include "include/core/SkFontStyle.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkTypeface.h"
#endif

namespace pulp::canvas {

/// The weight CSS font matching picks from `available` for `desired`:
///   - an exact match always wins;
///   - `desired` in [400, 500]: weights up to 500 ascending, then weights
///     below `desired` descending, then weights above 500 ascending;
///   - `desired` below 400: weights below `desired` descending, then weights
///     above it ascending;
///   - `desired` above 500: weights above `desired` ascending, then weights
///     below it descending.
/// Returns -1 when `available` is empty.
inline int css_match_font_weight(std::span<const int> available, int desired) {
    if (available.empty())
        return -1;
    for (const int w : available) {
        if (w == desired)
            return desired;
    }
    // The lightest available weight in [lo, hi], or -1.
    const auto lightest_in = [&](int lo, int hi) {
        int best = -1;
        for (const int w : available) {
            if (w >= lo && w <= hi && (best < 0 || w < best))
                best = w;
        }
        return best;
    };
    // The heaviest available weight in [lo, hi], or -1.
    const auto heaviest_in = [&](int lo, int hi) {
        int best = -1;
        for (const int w : available) {
            if (w >= lo && w <= hi && (best < 0 || w > best))
                best = w;
        }
        return best;
    };
    int match = -1;
    if (desired >= 400 && desired <= 500) {
        if ((match = lightest_in(desired, 500)) >= 0)
            return match;
        if ((match = heaviest_in(INT_MIN, desired - 1)) >= 0)
            return match;
        return lightest_in(501, INT_MAX);
    }
    if (desired < 400) {
        if ((match = heaviest_in(INT_MIN, desired)) >= 0)
            return match;
        return lightest_in(desired + 1, INT_MAX);
    }
    if ((match = lightest_in(desired, INT_MAX)) >= 0)
        return match;
    return heaviest_in(INT_MIN, desired - 1);
}

#ifdef PULP_HAS_SKIA

/// The face of `set` CSS font matching picks for `style`, among faces of the
/// requested width and slant. nullptr when no face has both, which leaves
/// width and slant fallback to the caller's own matcher, unchanged.
sk_sp<SkTypeface> css_match_style(SkFontStyleSet& set, const SkFontStyle& style);

/// `mgr.matchFamilyStyle()` with CSS weight matching inside the family: the
/// family's style set is searched by css_match_style(), and only a family the
/// manager does not enumerate, or a width or slant it lacks, falls back to the
/// manager's own nearest match.
sk_sp<SkTypeface> css_match_family_style(SkFontMgr& mgr, const char family[],
                                         const SkFontStyle& style);

/// A font manager that answers every family/style match with
/// css_match_family_style() over `base`, and forwards everything else to it.
/// SkParagraph's font collection uses it as its default manager, so a
/// paragraph's face for a family is the face FontResolver reports.
sk_sp<SkFontMgr> make_css_weight_font_manager(sk_sp<SkFontMgr> base);

#endif // PULP_HAS_SKIA

} // namespace pulp::canvas
