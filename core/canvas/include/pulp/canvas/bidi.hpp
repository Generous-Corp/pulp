#pragma once

// BidiAnalyzer — paragraph-level Unicode Bidirectional Algorithm
// implementation backed by SheenBidi (Apache-2.0). Produces logical-
// to-visual run segmentation for Arabic / Hebrew / mixed-direction
// text. The output is consumable by TextShaper / TextRunPlanner /
// SkShaper without depending on system ICU, which is the path the
// existing core/canvas::TextRunPlanner prefers when Skia is linked.
//
// Why we ship our own bidi:
//   * iOS / Android / minimal-toolchain hosts may not link ICU.
//   * Headless tests want deterministic bidi without a Skia link.
//   * SheenBidi is small (~120 KB), Apache-2.0, no POSIX dependencies.
//
// Design contract:
//   * Input is UTF-8.
//   * `base_direction == Auto` invokes UBA rule P2/P3 (first strong
//     character wins). `LTR` / `RTL` force the paragraph level.
//   * Output runs are returned in **logical** order (the order the
//     bytes appear in the source string). Each run carries the
//     embedding `level` from UBA rule X1-I2; odd levels are RTL.
//     The caller (TextShaper / SkShaper) is responsible for emitting
//     RTL runs right-to-left during paint.
//   * The "visual order" helper rearranges the same runs into the
//     L1/L2 visual sequence required for line painting.
//
// Build modes:
//   * PULP_HAS_SHEENBIDI defined → real SheenBidi calls.
//   * Undefined → a single-run pass-through that infers level from
//     `base_direction` only (no codepoint analysis). Lets callers
//     compile without the dependency and ship a correct LTR-only
//     behavior while still exercising the API shape.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::canvas {

/// Paragraph base direction selector (UBA rule P2/P3 surfaces).
enum class BidiBaseDirection : std::uint8_t {
    Auto = 0,   ///< Run UBA rule P2 (first strong character).
    LTR  = 1,   ///< Force paragraph level 0.
    RTL  = 2,   ///< Force paragraph level 1.
};

/// One contiguous span of `text` that shares a single bidi embedding level.
/// `start` + `length` are byte offsets / byte lengths into the input UTF-8.
/// Runs returned by `analyze()` are in logical order; runs returned by
/// `visual_order()` are in visual (paint) order.
struct BidiRun {
    std::uint32_t start  = 0;
    std::uint32_t length = 0;
    std::uint8_t  level  = 0;  ///< 0 = LTR; odd levels (1, 3, …) = RTL.

    constexpr bool is_rtl() const noexcept { return (level & 1u) == 1u; }
    constexpr std::uint32_t end() const noexcept { return start + length; }
};

/// Aggregate paragraph-level bidi result.
struct BidiParagraph {
    std::uint8_t        base_level = 0;
    std::vector<BidiRun> runs;       ///< Logical order.
    bool                used_sheenbidi = false;  ///< False on the pass-through.
};

class BidiAnalyzer {
public:
    /// Analyze one paragraph of UTF-8 text. `base_direction` selects
    /// the paragraph level (Auto = UBA rule P2). Always returns a
    /// non-empty `runs` vector when `text` is non-empty.
    static BidiParagraph analyze(std::string_view text,
                                 BidiBaseDirection base_direction = BidiBaseDirection::Auto);

    /// Re-order `runs` (in logical order, as returned by analyze) into
    /// visual / paint order via UBA rules L1-L2. The implementation is
    /// a simplified L2 reverse-on-odd-level pass — sufficient for
    /// single-line label paint. TextEditor's multi-line wrap path stays on the
    /// SkShaper / ICU iterator route in text_run_planner.cpp.
    static std::vector<BidiRun> visual_order(const std::vector<BidiRun>& runs);

    /// True when the build links SheenBidi. Tests that need the real
    /// engine (vs the pass-through stub) gate themselves on this.
    static bool has_sheenbidi() noexcept;
};

}  // namespace pulp::canvas
