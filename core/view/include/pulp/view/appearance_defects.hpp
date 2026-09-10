#pragma once

#include <pulp/view/view.hpp>

#include <string>
#include <vector>

namespace pulp::view {

/// Visual-appearance defect detection over a laid-out View tree.
///
/// These detectors assert PAINTED GEOMETRY, not the values that fed it. A
/// parameter arriving at the runtime, a node existing in a snapshot, or a
/// widget reporting a size are all proxies: each can be true while the user
/// looks at two labels printed on top of each other. What is asserted here is
/// where the glyphs land.
///
/// The report always carries a coverage statistic. A run that evaluated a
/// fraction of the text on screen cannot distinguish "no defects" from "did
/// not look", so `AppearanceReport::clean()` is only meaningful together with
/// `coverage.trustworthy()`.

enum class AppearanceDefectKind {
    /// Two pieces of painted text share pixels.
    text_overlap,
    /// A run's glyphs are wider than the box that is supposed to contain them,
    /// so the tail is clipped or truncated.
    painted_wider_than_box,
    /// Two canvas-command text runs (`CanvasDrawCmd::fill_text` /
    /// `stroke_text`) share pixels. These are drawn by a command stream, not
    /// by a layout node, so no box-based check can see them.
    canvas_text_overlap,
    /// A visible clipping container holds text and shows none of it — the
    /// viewer sees an empty panel. The signature is exact: N text runs in the
    /// subtree, all N clipped away, N >= 1. A scroll frame showing some of its
    /// rows measures at least one and is not this; a frame scrolled entirely
    /// past its text measures none and is, which is honest, because that frame
    /// is blank to look at. The container's own box may be collapsed to zero
    /// area, which is the same defect wearing a different geometry — a
    /// container closed on purpose is marked invisible instead, and the walk
    /// records that as an invisible skip without ever opening a clip scope.
    /// This is the one defect the collision detectors are structurally unable
    /// to see: text that never lands anywhere produces no pair to compare and
    /// no box to outgrow, so the report reads clean.
    container_paints_no_text,
};

const char* to_string(AppearanceDefectKind kind);

/// Why a piece of text was not evaluated. Every skip is counted; a detector
/// that silently drops what it cannot measure reports absence it never checked.
enum class TextRunSkip {
    none,
    invisible,        ///< the node or an ancestor is not visible
    empty_text,       ///< nothing to paint
    clipped_away,     ///< ink falls entirely outside its effective clip
    unmeasurable,     ///< the shaper returned no extents for non-empty text
    degenerate_box,   ///< the layout box has no area to paint into
};

/// One piece of painted text, resolved to screen space.
struct TextRun {
    std::string node_id;
    std::string kind;      ///< View type name, or "canvas:fill_text"
    std::string text;
    Rect ink{};            ///< painted glyph extents, absolute, alignment applied
    /// The part of `ink` that survives the clip — the pixels a viewer can
    /// actually see. Two runs collide only where both are visible, so this is
    /// what the overlap detectors compare. `ink` stays unclipped because the
    /// width detector asks a different question: did the glyphs outgrow their
    /// own box, which is true whether or not an ancestor also cut them off.
    Rect visible_ink{};
    Rect box{};            ///< the layout box the ink is meant to live in
    Rect clip{};           ///< effective clip at paint time, absolute
    int paint_order = 0;
    /// Nearest scrolling ancestor. Two runs are only geometrically comparable
    /// once both are resolved into the same space; this records which frame a
    /// run came from so a scrolled row cannot be compared against a fixed one
    /// by accident.
    const View* scroll_frame = nullptr;
    bool from_canvas = false;
};

/// N of M evaluated, K skipped and why. Printed on every run.
struct AppearanceCoverage {
    int text_nodes_seen = 0;        ///< text-bearing views encountered
    int text_runs_measured = 0;     ///< runs with real ink extents
    int skipped_invisible = 0;
    int skipped_empty = 0;
    int skipped_clipped = 0;
    /// The part of `skipped_clipped` that fell inside a container reported as
    /// `container_paints_no_text`. Clipping means two different things and a
    /// single count conflates them: a row scrolled below the fold was
    /// correctly not evaluated, while text inside a container that paints
    /// nothing is a region the walk could not see into at all. Only the
    /// second is blindness, and only the second is counted here.
    int skipped_clipped_in_empty_container = 0;
    int skipped_unmeasurable = 0;
    int skipped_degenerate_box = 0;

    int canvas_widgets = 0;
    int canvas_text_commands = 0;
    int canvas_text_measured = 0;
    int canvas_text_skipped = 0;

    int pairs_compared = 0;
    int pairs_skipped_cross_frame = 0;

    /// False when the text shaper is the character-width estimator rather than
    /// a real shaping engine. Estimated advances are not evidence about
    /// truncation to sub-em precision.
    bool shaping_is_real = false;

    int total_skipped() const {
        return skipped_invisible + skipped_empty + skipped_clipped +
               skipped_unmeasurable + skipped_degenerate_box;
    }
    int text_runs_total() const { return text_runs_measured + total_skipped(); }

    /// A run that measured nothing, measured a minority of the text it saw,
    /// or could not see into a region at all cannot support a claim of
    /// absence. The region term is a veto rather than a ratio: a caller that
    /// reads this boolean and nothing else must not be told a panel is
    /// well-measured while an entire container of it was never reachable,
    /// however small that container is against the rest of the screen.
    bool trustworthy() const;

    std::string to_string() const;
};

struct AppearanceFinding {
    AppearanceDefectKind kind = AppearanceDefectKind::text_overlap;
    std::string a_id, a_text;
    std::string b_id, b_text;
    Rect a_rect{}, b_rect{}, overlap{};
    float painted_width = 0.0f;
    float box_width = 0.0f;
    /// For `container_paints_no_text`: how many text runs the container holds,
    /// every one of them clipped away.
    int clipped_runs = 0;
    /// How far the ink reaches past the worse of the box's two vertical edges.
    /// Positive whenever the glyphs are outside the box, whether because they
    /// are wider than it or because they were placed off one of its sides.
    float overflow_px = 0.0f;

    std::string describe() const;
};

struct AppearanceReport {
    std::vector<AppearanceFinding> findings;
    AppearanceCoverage coverage;
    /// Every run the walk resolved, in paint order. Exposed so a caller can
    /// build its own control query over the same instrument.
    std::vector<TextRun> runs;

    bool clean() const { return findings.empty(); }
    std::string to_string() const;
};

struct AppearanceOptions {
    /// Overlaps thinner than this in either axis are ignored — adjacent text
    /// commonly shares a boundary pixel.
    float overlap_tolerance_px = 0.5f;
    /// Ink may exceed its box by this much before it counts as truncation.
    float width_tolerance_px = 0.5f;
    /// Overlaps smaller than this area are ignored.
    float min_overlap_area_px2 = 1.0f;
    bool include_canvas_text = true;
    /// Compare runs that live in different scroll frames. Off by default: a
    /// scrolled row and a fixed toolbar can only be compared once both are in
    /// screen space, and a run scrolled out of its own clip is not on screen
    /// at all.
    bool compare_across_scroll_frames = false;
    /// Grow every measured run's ink by this many pixels on each side before
    /// the collision detectors run.
    ///
    /// This is the positive control for an empty finding list. A detector that
    /// reports nothing has either found a clean panel or failed to reach it,
    /// and the report reads identically either way. Re-running with a few
    /// pixels of inflation on a panel with any text density at all MUST
    /// produce collisions; if it does not, the instrument never saw the text
    /// and its clean verdict means nothing. Never a measurement — findings
    /// produced under inflation describe the control, not the panel.
    float control_inflate_ink_px = 0.0f;
};

/// Walk `root` (already laid out) and report painted-text defects.
AppearanceReport detect_appearance_defects(const View& root,
                                           const AppearanceOptions& options = {});

} // namespace pulp::view
