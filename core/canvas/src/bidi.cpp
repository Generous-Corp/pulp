// SheenBidi-backed paragraph bidi analyser. See bidi.hpp for the API
// contract. The Skia / SkShaper path in text_run_planner.cpp keeps its
// existing ICU-iterator route; this file provides the canonical
// fallback that lets non-Skia / non-ICU builds (iOS, headless smoke,
// minimal toolchains) still produce correct visual ordering for
// Arabic / Hebrew / mixed-direction text.

#include "pulp/canvas/bidi.hpp"

#include <algorithm>
#include <utility>

#ifdef PULP_HAS_SHEENBIDI
extern "C" {
#include <SheenBidi/SBAlgorithm.h>
#include <SheenBidi/SBCodepointSequence.h>
#include <SheenBidi/SBParagraph.h>
#include <SheenBidi/SBRun.h>
#include <SheenBidi/SBLine.h>
}
#endif

namespace pulp::canvas {

bool BidiAnalyzer::has_sheenbidi() noexcept {
#ifdef PULP_HAS_SHEENBIDI
    return true;
#else
    return false;
#endif
}

BidiParagraph BidiAnalyzer::analyze(std::string_view text,
                                    BidiBaseDirection base_direction) {
    BidiParagraph out;

    if (text.empty()) {
        out.base_level = (base_direction == BidiBaseDirection::RTL) ? 1u : 0u;
        return out;
    }

#ifdef PULP_HAS_SHEENBIDI
    // SheenBidi consumes a (buffer, length, encoding) tuple. We feed
    // UTF-8 directly — SheenBidi decodes internally and produces byte-
    // indexed levels / runs that line up with the source string.
    SBCodepointSequence seq{};
    seq.stringEncoding = SBStringEncodingUTF8;
    seq.stringBuffer   = const_cast<char*>(text.data());
    seq.stringLength   = static_cast<SBUInteger>(text.size());

    SBAlgorithmRef algorithm = SBAlgorithmCreate(&seq);
    if (!algorithm) {
        // SheenBidi failed (allocation or malformed input). Fall back
        // to the pass-through path below by clearing `out` and not
        // setting `used_sheenbidi`.
        out.base_level = (base_direction == BidiBaseDirection::RTL) ? 1u : 0u;
        BidiRun run;
        run.start  = 0;
        run.length = static_cast<std::uint32_t>(text.size());
        run.level  = out.base_level;
        out.runs.push_back(run);
        return out;
    }

    SBLevel sb_base = SBLevelDefaultLTR;
    if (base_direction == BidiBaseDirection::LTR) sb_base = 0;
    else if (base_direction == BidiBaseDirection::RTL) sb_base = 1;
    // Auto → SBLevelDefaultLTR runs UBA rule P2; SheenBidi switches to
    // RTL automatically when the first strong character is RTL.

    SBParagraphRef paragraph = SBAlgorithmCreateParagraph(
        algorithm, /*paragraphOffset=*/0,
        /*suggestedLength=*/static_cast<SBUInteger>(text.size()),
        sb_base);
    if (!paragraph) {
        SBAlgorithmRelease(algorithm);
        out.base_level = (base_direction == BidiBaseDirection::RTL) ? 1u : 0u;
        BidiRun run;
        run.start  = 0;
        run.length = static_cast<std::uint32_t>(text.size());
        run.level  = out.base_level;
        out.runs.push_back(run);
        return out;
    }

    out.base_level     = static_cast<std::uint8_t>(SBParagraphGetBaseLevel(paragraph));
    out.used_sheenbidi = true;

    // SBLine applies UBA rules L1-L2 over a paragraph range and returns
    // contiguous runs grouped by embedding level. We treat the entire
    // paragraph as one line, which matches the current single-line label
    // paint path. Multi-line callers can call analyze() per wrapped line
    // themselves once line breaking is fixed.
    SBLineRef line = SBParagraphCreateLine(
        paragraph,
        /*lineOffset=*/0,
        /*lineLength=*/static_cast<SBUInteger>(text.size()));
    if (line) {
        const SBUInteger run_count = SBLineGetRunCount(line);
        const SBRun*     runs_ptr  = SBLineGetRunsPtr(line);

        out.runs.reserve(static_cast<std::size_t>(run_count));
        for (SBUInteger i = 0; i < run_count; ++i) {
            BidiRun r;
            r.start  = static_cast<std::uint32_t>(runs_ptr[i].offset);
            r.length = static_cast<std::uint32_t>(runs_ptr[i].length);
            r.level  = static_cast<std::uint8_t>(runs_ptr[i].level);
            out.runs.push_back(r);
        }

        // SBLineGetRunsPtr already returns runs in visual order
        // (L1-L2 applied). To honor the API contract — `analyze()`
        // returns logical order — re-sort by byte offset.
        std::sort(out.runs.begin(), out.runs.end(),
                  [](const BidiRun& a, const BidiRun& b) {
                      return a.start < b.start;
                  });

        SBLineRelease(line);
    } else {
        // No line — degrade to a single base-level run rather than
        // emitting nothing.
        BidiRun r;
        r.start  = 0;
        r.length = static_cast<std::uint32_t>(text.size());
        r.level  = out.base_level;
        out.runs.push_back(r);
    }

    SBParagraphRelease(paragraph);
    SBAlgorithmRelease(algorithm);
#else
    // Pass-through stub: one run, level inferred from base direction.
    // Preserves API shape so callers compile without SheenBidi.
    out.base_level = (base_direction == BidiBaseDirection::RTL) ? 1u : 0u;
    BidiRun r;
    r.start  = 0;
    r.length = static_cast<std::uint32_t>(text.size());
    r.level  = out.base_level;
    out.runs.push_back(r);
#endif

    return out;
}

std::vector<BidiRun> BidiAnalyzer::visual_order(const std::vector<BidiRun>& runs) {
    // UBA rule L2 simplified: reverse contiguous spans of runs whose
    // level is >= 1, then again for >= 2, etc. For Pulp's primary use
    // case (paragraph-level Arabic / Hebrew / mixed Latin) embedding
    // depths above 1 are vanishingly rare, but we handle them
    // generically.
    std::vector<BidiRun> visual = runs;
    if (visual.size() < 2) return visual;

    std::uint8_t max_level = 0;
    for (const auto& r : visual) {
        max_level = std::max(max_level, r.level);
    }

    // Reverse runs from levels [L, max_level] for L = max_level down to 1.
    for (std::uint8_t target = max_level; target >= 1; --target) {
        std::size_t i = 0;
        while (i < visual.size()) {
            if (visual[i].level >= target) {
                std::size_t j = i;
                while (j < visual.size() && visual[j].level >= target) ++j;
                std::reverse(visual.begin() + static_cast<std::ptrdiff_t>(i),
                             visual.begin() + static_cast<std::ptrdiff_t>(j));
                i = j;
            } else {
                ++i;
            }
        }
        if (target == 1) break;  // unsigned underflow guard
    }
    return visual;
}

}  // namespace pulp::canvas
