// design_ir_analysis.cpp — post-parse analysis passes over the DesignIR.
//
// Extracted from design_ir_json.cpp (which is the IR JSON serialization
// CONTRACT) so those passes have a home to grow in and the contract file stays
// focused. Two passes live here:
//   - import report  (collect_import_report + import_report_to_json/_to_text):
//     surfaces per-control resolution provenance for the CLI + CI gate.
//   - placement verification (apply_placement_verification): the structural
//     half of the render-golden gate; flags zero-extent / off-frame overlays.
// Both walk the IR and depend only on the IR types + two shared helpers
// (json_escape, interactive_kind_id) declared in design_import_internal.hpp.
// (A finer report-vs-verify file split is possible later; kept together here as
// "analysis passes" to keep the extraction a single, behavior-preserving move.)

#include <pulp/view/design_import.hpp>

#include "design_import_internal.hpp"

#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace pulp::view {

// ── import report ────────────────────────────────────────────────────────────
static void collect_report_visit(const IRNode& node, ImportReport& report,
                                 float low_confidence_threshold) {
    for (const auto& e : node.interactive_elements) {
        ImportReportEntry entry;
        entry.source_node_id = e.source_node_id.value_or("");
        entry.kind = interactive_kind_id(e.kind);
        entry.resolution_rung = e.resolution_rung;
        entry.confidence_score = e.confidence_score;
        entry.conflict_signals = e.conflict_signals;
        entry.verification_pass = e.verification_pass;
        if (!entry.conflict_signals.empty()) report.conflicted++;
        if (entry.confidence_score < low_confidence_threshold) report.low_confidence++;
        if (entry.resolution_rung == 5) report.unresolved++;  // inert (warn) rung
        report.controls.push_back(std::move(entry));
    }
    for (const auto& c : node.children)
        collect_report_visit(c, report, low_confidence_threshold);
    // Alternate frames are a sibling axis to children, so their controls are only
    // reported if we descend here too — otherwise every control on frame 1+ would
    // be invisible to the report and to --fail-on-unresolved.
    for (const auto& f : node.alternate_frames)
        collect_report_visit(f, report, low_confidence_threshold);
}

ImportReport collect_import_report(const IRNode& root, float low_confidence_threshold) {
    ImportReport report;
    collect_report_visit(root, report, low_confidence_threshold);
    return report;
}

// Render-placement verification (structural half of the render-golden gate).
static int verify_placement_visit(IRNode& node, float fw, float fh) {
    int flagged = 0;
    for (auto& e : node.interactive_elements) {
        // A knob/fader/xy_pad carries its hit circle (hit_radius); the overlays
        // carry a box (w,h). A control with neither can't render anywhere.
        const bool has_box = e.w > 0.0f && e.h > 0.0f;
        const bool has_radius = e.hit_radius > 0.0f;
        std::string issue;
        if (!has_box && !has_radius) {
            issue = "control has no renderable extent (zero hit-radius and zero-area box)";
        } else if (fw > 0.0f && fh > 0.0f) {
            // Does the control's region fall ENTIRELY outside the frame [0,0,fw,fh]?
            float x0, y0, x1, y1;
            if (has_box) {
                x0 = e.x; y0 = e.y; x1 = e.x + e.w; y1 = e.y + e.h;
            } else {
                x0 = e.cx - e.hit_radius; y0 = e.cy - e.hit_radius;
                x1 = e.cx + e.hit_radius; y1 = e.cy + e.hit_radius;
            }
            if (x1 <= 0.0f || y1 <= 0.0f || x0 >= fw || y0 >= fh)
                issue = "control falls entirely outside the frame render region";
        }
        if (!issue.empty()) {
            e.verification_pass = false;
            e.conflict_signals.push_back(issue);
            ++flagged;
        }
    }
    // Overlays are checked against the ROOT faithful frame's region. The importer
    // emits interactive_elements only on the single top-level faithful_svg node
    // (children carry none), so the root frame is the correct coordinate space.
    // If nested faithful_svg nodes ever carry their own overlays, pass each such
    // node's own render dimensions here instead of inheriting the root's.
    for (auto& c : node.children) flagged += verify_placement_visit(c, fw, fh);
    // Each alternate frame is its own render region — a mode toggle routinely
    // swaps to a frame of a different size — so check its overlays against ITS
    // dimensions, not the frame we arrived from. A frame that declares no size
    // inherits, which keeps the bounds half at "unknown" rather than testing
    // against the wrong box.
    for (auto& f : node.alternate_frames) {
        flagged += verify_placement_visit(f,
                                          f.style.width.value_or(fw),
                                          f.style.height.value_or(fh));
    }
    return flagged;
}

int apply_placement_verification(IRNode& root, float frame_w, float frame_h) {
    return verify_placement_visit(root, frame_w, frame_h);
}

// Swap-target verification. A `swap` element addresses a frame POSITIONALLY
// within the DesignFrameView that owns it, so validity is decided per
// frame-owning node: frame 0 is the node itself, alternate_frames[i] is frame
// i+1.

// Flag `node`'s own swap elements that do not address one of `frame_count`
// frames. `own_index` is the frame `node` itself renders as (0 for the
// frame-owning node, i+1 for its alternate_frames[i]) — a swap that targets it
// activates the frame already showing, so the button does nothing.
static int check_swap_targets(IRNode& node, int frame_count, int own_index) {
    int flagged = 0;
    for (auto& e : node.interactive_elements) {
        if (e.kind != InteractiveElementKind::swap) continue;
        std::string issue;
        if (e.target_frame == -1) {
            issue = "swap link has no target frame (name the layer \"swap <n>\" to "
                    "address a captured frame)";
        } else if (e.target_frame < 0) {
            // Distinct from unset: a target got parsed, but to a negative index.
            issue = "swap link targets frame " + std::to_string(e.target_frame) +
                    ", which is not a frame index (valid indices 0.." +
                    std::to_string(frame_count - 1) + ")";
        } else if (e.target_frame >= frame_count) {
            issue = "swap link targets frame " + std::to_string(e.target_frame) +
                    " but only " + std::to_string(frame_count) + " frame" +
                    (frame_count == 1 ? "" : "s") + " captured (valid indices 0.." +
                    std::to_string(frame_count - 1) + ")";
        } else if (e.target_frame == own_index) {
            issue = "swap link targets frame " + std::to_string(e.target_frame) +
                    ", the frame it already sits on — clicking it would do nothing";
        }
        if (!issue.empty()) {
            e.verification_pass = false;
            e.conflict_signals.push_back(std::move(issue));
            ++flagged;
        }
    }
    return flagged;
}

static int verify_swap_visit(IRNode& node) {
    const int frame_count = 1 + static_cast<int>(node.alternate_frames.size());
    int flagged = check_swap_targets(node, frame_count, 0);
    for (std::size_t i = 0; i < node.alternate_frames.size(); ++i) {
        IRNode& f = node.alternate_frames[i];
        // An alternate's own swaps (the "back" toggle) address the OWNER's frame
        // set, so they are checked against the owner's count rather than treated
        // as a fresh owner — and the alternate renders as frame i+1, which is the
        // index a self-target would name. Its children are ordinary nodes and
        // recurse normally.
        flagged += check_swap_targets(f, frame_count, static_cast<int>(i) + 1);
        for (auto& c : f.children) flagged += verify_swap_visit(c);
    }
    for (auto& c : node.children) flagged += verify_swap_visit(c);
    return flagged;
}

int apply_swap_target_verification(IRNode& root) {
    return verify_swap_visit(root);
}

// A node's alternate frames reach a renderer only through make_faithful_svg_frame
// / emit_faithful_frame, both of which bail before touching alternate_frames
// unless the node is faithful_svg AND names an SVG asset. Mirror that exact
// admission test here so the walk reports precisely the sets that get dropped.
static void find_unrenderable_visit(const IRNode& node,
                                    std::vector<UnrenderableFrameSet>& out) {
    if (!node.alternate_frames.empty()) {
        const char* reason = nullptr;
        if (node.render_mode != NodeRenderMode::faithful_svg)
            reason = "the node it captured them on renders as native widgets "
                     "(render_mode is not faithful_svg), and only a faithful_svg "
                     "node renders alternate frames";
        else if (!node.svg_asset_id)
            reason = "the node it captured them on is faithful_svg but names no "
                     "svg_asset_id, so it has no frame 0 to add frames to";
        if (reason)
            out.push_back({node.name.empty() ? "<unnamed>" : node.name,
                           node.alternate_frames.size(), reason});
    }
    // Alternates are a sibling axis to children: a nested frame-owning node can
    // sit inside either, so both are walked.
    for (const auto& f : node.alternate_frames) find_unrenderable_visit(f, out);
    for (const auto& c : node.children) find_unrenderable_visit(c, out);
}

std::vector<UnrenderableFrameSet> find_unrenderable_alternate_frames(const IRNode& root) {
    std::vector<UnrenderableFrameSet> out;
    find_unrenderable_visit(root, out);
    return out;
}

std::string import_report_to_json(const ImportReport& r) {
    std::ostringstream out;
    out << "{\"summary\":{\"total\":" << r.controls.size()
        << ",\"conflicted\":" << r.conflicted
        << ",\"low_confidence\":" << r.low_confidence
        << ",\"unresolved\":" << r.unresolved
        << ",\"ok\":" << (r.ok() ? "true" : "false") << "},\"controls\":[";
    for (size_t i = 0; i < r.controls.size(); ++i) {
        const auto& c = r.controls[i];
        if (i) out << ',';
        out << "{\"source_node_id\":\"" << json_escape(c.source_node_id) << "\""
            << ",\"kind\":\"" << c.kind << "\""
            << ",\"resolution_rung\":" << c.resolution_rung
            << ",\"confidence_score\":" << c.confidence_score
            << ",\"verification_pass\":" << (c.verification_pass ? "true" : "false")
            << ",\"conflict_signals\":[";
        for (size_t j = 0; j < c.conflict_signals.size(); ++j) {
            if (j) out << ',';
            out << '"' << json_escape(c.conflict_signals[j]) << '"';
        }
        out << "]}";
    }
    out << "]}";
    return out.str();
}

std::string import_report_to_text(const ImportReport& r) {
    std::ostringstream out;
    out << "import report: " << r.controls.size() << " control(s), "
        << r.conflicted << " conflicted, " << r.low_confidence << " low-confidence, "
        << r.unresolved << " unresolved (inert)\n";
    for (const auto& c : r.controls) {
        out << "  - " << (c.source_node_id.empty() ? "?" : c.source_node_id)
            << "  kind=" << c.kind << " rung=" << c.resolution_rung
            << " confidence=" << c.confidence_score
            << (c.verification_pass ? "" : " [verify-FAIL]") << '\n';
        for (const auto& cf : c.conflict_signals)
            out << "      conflict: " << cf << '\n';
    }
    return out.str();
}

}  // namespace pulp::view
