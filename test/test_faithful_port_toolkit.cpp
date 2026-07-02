// Tests for the JUCE-port-accelerator "faithful port toolkit" SDK primitives
// (Phase 6). Each primitive a faithful design port would otherwise re-roll by
// hand, tested at the level the GPU-OFF build can assert deterministically:
//
//   P6.3 — SVG fragment handles: extraction / document-build / transform math are
//          pure string+geometry and asserted directly; draw_fragment's Canvas op
//          is asserted against a RecordingCanvas subclass that captures draw_svg
//          (the base RecordingCanvas returns false for draw_svg, so this build —
//          PULP_ENABLE_GPU=OFF, no Skia raster — can't pixel-check; the comment on
//          each SECTION states which mode it is in).
//
// Additional primitives (P6.5 anchored popover, P6.6 drag-to-reorder, P6.7
// paint-space painters) append their own sections below as they land.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include <pulp/canvas/canvas.hpp>
#include <pulp/view/callout_box.hpp>
#include <pulp/view/design_frame_view.hpp>
#include <pulp/view/svg_fragment.hpp>

using Catch::Approx;
using namespace pulp;
using pulp::view::CalloutPlacement;
using pulp::view::CalloutSide;
using pulp::view::CalloutStyle;
using pulp::view::AnchoredCallout;
using pulp::view::DesignFrameElement;
using pulp::view::DesignFrameView;
using pulp::view::FragmentTransform;
using pulp::view::Rect;

namespace {

// A RecordingCanvas that ALSO captures draw_svg documents (the base class returns
// false and drops the document, since it models the Canvas2D command stream, not
// the SVG-DOM path). Lets the fragment tests assert the exact mini-document and
// draw box draw_fragment hands the backend without a Skia raster surface.
class SvgRecordingCanvas : public canvas::RecordingCanvas {
public:
    struct SvgCall {
        std::string document;
        float x = 0, y = 0, w = 0, h = 0;
    };
    std::vector<SvgCall> svg_calls;

    bool draw_svg(const std::string& doc, float x, float y, float w, float h) override {
        svg_calls.push_back({doc, x, y, w, h});
        return true;  // pretend the backend rendered it
    }
};

// A tiny hand-authored SVG frame with a big panel <rect> (so detect_panel picks
// it edge-to-edge) plus a knob whose needle is a tagged <path>. The needle `d`
// is the unique fragment marker. Coordinates are the design's own space.
const char* kFrameSvg =
    "<svg width=\"200\" height=\"200\" viewBox=\"0 0 200 200\" "
    "xmlns=\"http://www.w3.org/2000/svg\">"
    "<rect x=\"10\" y=\"10\" width=\"180\" height=\"180\" fill=\"#101418\"/>"
    "<circle cx=\"100\" cy=\"100\" r=\"30\" fill=\"#202830\"/>"
    "<path d=\"M100 100 L100 75\" stroke=\"#39ff64\" fill=\"none\"/>"
    "</svg>";

DesignFrameView make_knob_view() {
    DesignFrameElement knob;
    knob.kind = DesignFrameElement::Kind::knob;
    knob.cx = 100.0f; knob.cy = 100.0f; knob.hit_radius = 30.0f;
    knob.needle_d = "M100 100 L100 75";
    knob.value = 0.5f;
    return DesignFrameView(kFrameSvg, {knob});
}

}  // namespace

// ── P6.3: SVG fragment handles ───────────────────────────────────────────────

TEST_CASE("FragmentTransform emits composed SVG transform attribute",
          "[view][fragment][issue-juce-port]") {
    // Pure math — no canvas.
    SECTION("identity emits nothing") {
        FragmentTransform id;
        CHECK(id.is_identity());
        CHECK(id.to_svg_transform().empty());
    }
    SECTION("translate only") {
        FragmentTransform t; t.dx = 12.0f; t.dy = -4.0f;
        CHECK_FALSE(t.is_identity());
        CHECK(t.to_svg_transform() == "translate(12.000 -4.000)");
    }
    SECTION("rotate about a pivot") {
        FragmentTransform t; t.rotate_deg = 30.0f; t.pivot_x = 100.0f; t.pivot_y = 100.0f;
        CHECK(t.to_svg_transform() == "rotate(30.000 100.000 100.000)");
    }
    SECTION("scale about a pivot expands to translate/scale/translate") {
        FragmentTransform t; t.scale = 1.5f; t.pivot_x = 50.0f; t.pivot_y = 60.0f;
        const std::string s = t.to_svg_transform();
        CHECK(s.find("scale(1.5000)") != std::string::npos);
        CHECK(s.find("translate(50.000 60.000)") != std::string::npos);
        CHECK(s.find("translate(-50.000 -60.000)") != std::string::npos);
    }
    SECTION("translate + rotate compose in order") {
        FragmentTransform t; t.dx = 5.0f; t.dy = 5.0f; t.rotate_deg = 90.0f;
        const std::string s = t.to_svg_transform();
        CHECK(s.rfind("translate(5.000 5.000)", 0) == 0);        // translate first
        CHECK(s.find("rotate(90.000") != std::string::npos);      // then rotate
    }
}

TEST_CASE("extract_svg_fragment pulls the marker's element out whole",
          "[view][fragment][issue-juce-port]") {
    SECTION("self-closing element by its path d marker") {
        const std::string frag =
            pulp::view::extract_svg_fragment(kFrameSvg, "M100 100 L100 75");
        CHECK(frag == "<path d=\"M100 100 L100 75\" stroke=\"#39ff64\" fill=\"none\"/>");
    }
    SECTION("unknown marker yields empty") {
        CHECK(pulp::view::extract_svg_fragment(kFrameSvg, "no-such-marker").empty());
    }
    SECTION("depth-matched container comes out whole") {
        const std::string doc =
            "<svg xmlns=\"http://www.w3.org/2000/svg\">"
            "<g id=\"outer\"><g id=\"inner\"><path d=\"MARK\"/></g><rect/></g>"
            "<circle/></svg>";
        const std::string frag = pulp::view::extract_svg_fragment(doc, "id=\"outer\"");
        CHECK(frag ==
              "<g id=\"outer\"><g id=\"inner\"><path d=\"MARK\"/></g><rect/></g>");
    }
}

TEST_CASE("svg_open_tag preserves the coordinate-space header",
          "[view][fragment][issue-juce-port]") {
    const std::string header = pulp::view::svg_open_tag(kFrameSvg);
    CHECK(header.find("width=\"200\"") != std::string::npos);
    CHECK(header.find("viewBox=\"0 0 200 200\"") != std::string::npos);
    CHECK(header.back() == '>');
    CHECK(pulp::view::svg_open_tag("no svg here").empty());
}

TEST_CASE("recolor_svg_fragment rewrites paint values, leaves none alone",
          "[view][fragment][issue-juce-port]") {
    const std::string frag =
        "<path d=\"X\" stroke=\"#39ff64\" fill=\"none\"/>";
    SECTION("fill=none is preserved; stroke opt-in recolors") {
        const std::string out =
            pulp::view::recolor_svg_fragment(frag, "#ffffff", /*include_stroke=*/true);
        CHECK(out.find("fill=\"none\"") != std::string::npos);   // none untouched
        CHECK(out.find("stroke=\"#ffffff\"") != std::string::npos);
    }
    SECTION("a real fill is replaced") {
        const std::string out = pulp::view::recolor_svg_fragment(
            "<circle fill=\"#202830\"/>", "#ff0000");
        CHECK(out == "<circle fill=\"#ff0000\"/>");
    }
    SECTION("empty hex is a no-op") {
        CHECK(pulp::view::recolor_svg_fragment(frag, "") == frag);
    }
}

TEST_CASE("build_svg_fragment_document composites one fragment in the source space",
          "[view][fragment][issue-juce-port]") {
    const std::string frag =
        pulp::view::extract_svg_fragment(kFrameSvg, "M100 100 L100 75");
    REQUIRE_FALSE(frag.empty());

    SECTION("identity + opaque + no recolor emits the bare fragment") {
        const std::string doc = pulp::view::build_svg_fragment_document(
            kFrameSvg, frag, {}, 1.0f, {});
        // Shares the source header, contains ONLY the fragment (no other element),
        // and needs no wrapping <g>.
        CHECK(doc.rfind("<svg", 0) == 0);
        CHECK(doc.find("viewBox=\"0 0 200 200\"") != std::string::npos);
        CHECK(doc.find(frag) != std::string::npos);
        CHECK(doc.find("<circle") == std::string::npos);   // sibling excluded
        CHECK(doc.find("<g") == std::string::npos);         // no wrapper needed
        CHECK(doc.find("</svg>") != std::string::npos);
    }
    SECTION("transform + opacity + recolor wrap in a <g>") {
        FragmentTransform t; t.rotate_deg = 45.0f; t.pivot_x = 100.0f; t.pivot_y = 100.0f;
        const std::string doc = pulp::view::build_svg_fragment_document(
            kFrameSvg, frag, t, 0.5f, "#ffffff");
        CHECK(doc.find("<g transform=\"rotate(45.000 100.000 100.000)\"") !=
              std::string::npos);
        CHECK(doc.find("opacity=\"0.5000\"") != std::string::npos);
        CHECK(doc.find("stroke=\"#39ff64\"") == std::string::npos);  // recolored...
        CHECK(doc.find("#ffffff") != std::string::npos);             // ...to white
        CHECK(doc.find("fill=\"none\"") != std::string::npos);       // none kept
    }
    SECTION("missing header / empty fragment yields empty") {
        CHECK(pulp::view::build_svg_fragment_document("nope", frag, {}).empty());
        CHECK(pulp::view::build_svg_fragment_document(kFrameSvg, "", {}).empty());
    }
}

TEST_CASE("DesignFrameView::draw_fragment issues one draw_svg over the panel box",
          "[view][fragment][issue-juce-port]") {
    // Mode: RecordingCanvas capture of the draw_svg op (no Skia raster in this
    // GPU-OFF build), asserting the mini-document + draw box are correct.
    DesignFrameView v = make_knob_view();
    v.set_bounds({0, 0, 200, 200});   // bounds == frame size → panel fits at scale 1
    v.layout_children();

    v.register_fragment("needle", "M100 100 L100 75");
    CHECK(v.has_fragment("needle"));
    CHECK_FALSE(v.has_fragment("missing"));

    SECTION("registered fragment draws, transform+recolor reach the document") {
        SvgRecordingCanvas canvas;
        FragmentTransform t; t.rotate_deg = 20.0f; t.pivot_x = 100.0f; t.pivot_y = 100.0f;
        const bool ok = v.draw_fragment(canvas, "needle", t, 0.6f, "#ff8800");
        CHECK(ok);
        REQUIRE(canvas.svg_calls.size() == 1);
        const auto& call = canvas.svg_calls[0];
        // Panel is the 180x180 rect at (10,10) but the SVG is drawn at the frame
        // origin so its coords line up: draw box maps panel top-left to view.
        CHECK(call.document.find("rotate(20.000 100.000 100.000)") != std::string::npos);
        CHECK(call.document.find("opacity=\"0.6000\"") != std::string::npos);
        CHECK(call.document.find("#ff8800") != std::string::npos);
        // Draw box is finite and positive — the panel is laid out.
        CHECK(call.w > 0.0f);
        CHECK(call.h > 0.0f);
    }
    SECTION("unknown id draws nothing") {
        SvgRecordingCanvas canvas;
        CHECK_FALSE(v.draw_fragment(canvas, "missing"));
        CHECK(canvas.svg_calls.empty());
    }
    SECTION("marker absent from the SVG draws nothing") {
        SvgRecordingCanvas canvas;
        CHECK_FALSE(v.draw_fragment_marker(canvas, "not-in-svg"));
        CHECK(canvas.svg_calls.empty());
    }
    SECTION("un-laid-out view (zero bounds) draws nothing") {
        DesignFrameView unlaid = make_knob_view();  // no set_bounds
        SvgRecordingCanvas canvas;
        CHECK_FALSE(unlaid.draw_fragment_marker(canvas, "M100 100 L100 75"));
        CHECK(canvas.svg_calls.empty());
    }
}

TEST_CASE("DesignFrameView hover/bypass restyle rides draw_fragment",
          "[view][fragment][issue-juce-port]") {
    // paint() draws the full frame then, for elements with a fragment handle,
    // redraws that fragment brightened (hovered) or desaturated (disabled). We
    // assert the EXTRA draw_svg calls beyond the one full-frame draw.
    DesignFrameView v = make_knob_view();
    v.set_bounds({0, 0, 200, 200});
    v.layout_children();

    SECTION("idle: only the full frame is drawn") {
        SvgRecordingCanvas canvas;
        v.paint(canvas);
        CHECK(canvas.svg_calls.size() == 1);   // just the frame
    }
    SECTION("hovered element adds a brightened fragment redraw") {
        SvgRecordingCanvas canvas;
        v.simulate_hover({100, 100});          // over the knob
        REQUIRE(v.element_hovered() == 0);
        v.paint(canvas);
        REQUIRE(canvas.svg_calls.size() == 2);
        CHECK(canvas.svg_calls[1].document.find("#ffffff") != std::string::npos);
    }
    SECTION("disabled element adds a desaturated fragment redraw") {
        SvgRecordingCanvas canvas;
        v.set_element_enabled(0, false);
        v.paint(canvas);
        REQUIRE(canvas.svg_calls.size() == 2);
        CHECK(canvas.svg_calls[1].document.find("#6b7280") != std::string::npos);
    }
}

// ── P6.5: Anchored popover with pointer triangle (AnchoredCallout parity) ─────────
// Pure placement math (place_callout) — no canvas needed.

namespace {
constexpr float kWinW = 400.0f, kWinH = 400.0f;
const Rect kWindow{0, 0, kWinW, kWinH};
}  // namespace

TEST_CASE("place_callout keeps the preferred side when it fits",
          "[view][callout][issue-juce-port]") {
    // Anchor mid-window: everything fits, so preferred survives.
    const Rect anchor{190, 190, 20, 20};
    auto p = pulp::view::place_callout(anchor, 120, 80, kWindow, CalloutSide::below);
    CHECK(p.side == CalloutSide::below);
    // Body sits below the anchor, centered on it.
    CHECK(p.body.y > anchor.bottom());
    CHECK(p.body.center().x == Approx(anchor.center().x));
}

TEST_CASE("place_callout auto-flips when the preferred side would clip",
          "[view][callout][issue-juce-port]") {
    CalloutStyle style;  // defaults
    SECTION("near top edge, prefer above -> flips below") {
        const Rect anchor{190, 4, 20, 20};
        auto p = pulp::view::place_callout(anchor, 120, 120, kWindow,
                                           CalloutSide::above, style);
        CHECK(p.side == CalloutSide::below);
        CHECK(p.body.y >= anchor.bottom());
    }
    SECTION("near bottom edge, prefer below -> flips above") {
        const Rect anchor{190, kWinH - 24, 20, 20};
        auto p = pulp::view::place_callout(anchor, 120, 120, kWindow,
                                           CalloutSide::below, style);
        CHECK(p.side == CalloutSide::above);
        CHECK(p.body.bottom() <= anchor.y);
    }
    SECTION("near left edge, prefer left_of -> flips right_of") {
        const Rect anchor{4, 190, 20, 20};
        auto p = pulp::view::place_callout(anchor, 120, 120, kWindow,
                                           CalloutSide::left_of, style);
        CHECK(p.side == CalloutSide::right_of);
        CHECK(p.body.x >= anchor.right());
    }
    SECTION("no room either side keeps preferred (best effort)") {
        const Rect tiny{0, 0, 40, 40};
        const Rect anchor{10, 10, 20, 20};
        auto p = pulp::view::place_callout(anchor, 120, 120, tiny,
                                           CalloutSide::below, style);
        CHECK(p.side == CalloutSide::below);  // neither side fits -> no flip
    }
}

TEST_CASE("place_callout clamps the body inside the window margins",
          "[view][callout][issue-juce-port]") {
    CalloutStyle style;  // margin = 8
    // Anchor hugging the left edge with a wide body: centering would push the body
    // off the left of the window, so the cross axis clamps to the margin.
    const Rect anchor{2, 190, 12, 12};
    auto p = pulp::view::place_callout(anchor, 160, 80, kWindow,
                                       CalloutSide::below, style);
    CHECK(p.body.x >= style.margin - 0.01f);
    CHECK(p.body.right() <= kWinW - style.margin + 0.01f);
}

TEST_CASE("place_callout arrow tracks the anchor and stays on the body edge",
          "[view][callout][issue-juce-port]") {
    CalloutStyle style;
    SECTION("centered anchor: arrow tip aligns with the anchor center") {
        const Rect anchor{190, 190, 20, 20};
        auto p = pulp::view::place_callout(anchor, 120, 80, kWindow,
                                           CalloutSide::below, style);
        CHECK(p.arrow_tip_x == Approx(anchor.center().x));
        // Tip points UP at the anchor: above the body top edge.
        CHECK(p.arrow_tip_y < p.body.y);
        CHECK(p.arrow_base_y == Approx(p.body.y));
    }
    SECTION("clamped body: arrow base stays within the body edge, clear of corners") {
        // Anchor at far left forces the body to clamp; the arrow should still
        // point roughly at the anchor but never leave the body's flat edge.
        const Rect anchor{2, 190, 12, 12};
        auto p = pulp::view::place_callout(anchor, 160, 80, kWindow,
                                           CalloutSide::below, style);
        const float half_aw = style.arrow_width * 0.5f;
        CHECK(p.arrow_base_x >= p.body.x + style.corner_radius + half_aw - 0.01f);
        CHECK(p.arrow_base_x <= p.body.right() - style.corner_radius - half_aw + 0.01f);
    }
    SECTION("horizontal side: arrow points sideways at the anchor") {
        const Rect anchor{190, 190, 20, 20};
        auto p = pulp::view::place_callout(anchor, 120, 80, kWindow,
                                           CalloutSide::right_of, style);
        CHECK(p.side == CalloutSide::right_of);
        CHECK(p.arrow_tip_x < p.body.x);          // tip left of the body (toward anchor)
        CHECK(p.arrow_tip_y == Approx(anchor.center().y));
    }
}

TEST_CASE("AnchoredCallout hosts content and mounts as an overlay",
          "[view][callout][issue-juce-port]") {
    // View-level: mode is geometry assertion (no Skia raster in this build).
    auto root = std::make_unique<pulp::view::View>();
    root->set_bounds({0, 0, kWinW, kWinH});

    auto content = std::make_unique<pulp::view::View>();
    const Rect anchor{190, 20, 20, 20};  // near top -> should flip below
    AnchoredCallout* box = AnchoredCallout::show(root.get(), anchor, std::move(content),
                                       CalloutSide::above);
    REQUIRE(box != nullptr);
    box->set_content_size(100, 60);
    box->set_bounds({0, 0, kWinW, kWinH});
    box->layout_children();

    auto p = box->compute_placement({0, 0, kWinW, kWinH});
    CHECK(p.side == CalloutSide::below);           // flipped away from the top edge
    // Content child is inset inside the body by the padding.
    REQUIRE(box->content() != nullptr);
    const Rect cb = box->content()->bounds();
    CHECK(cb.x > p.body.x);
    CHECK(cb.y > p.body.y);
    CHECK(cb.width == Approx(100.0f));
    CHECK(cb.height == Approx(60.0f));
}

// ── P6.6: Drag-to-reorder container ──────────────────────────────────────────

#include <pulp/view/reorder_list.hpp>
using pulp::view::ReorderList;

TEST_CASE("reorder_target_index rounds the drag offset to a slot",
          "[view][reorder][issue-juce-port]") {
    const float pitch = 46.0f;  // extent 40 + gap 6
    SECTION("no offset keeps the source index") {
        CHECK(pulp::view::reorder_target_index(1, 0.0f, pitch, 4) == 1);
    }
    SECTION("dragging just past two slots lands on +2") {
        CHECK(pulp::view::reorder_target_index(0, pitch * 2.1f, pitch, 4) == 2);
    }
    SECTION("upward drag rounds negative") {
        CHECK(pulp::view::reorder_target_index(3, -pitch * 1.6f, pitch, 4) == 1);
    }
    SECTION("clamps into range") {
        CHECK(pulp::view::reorder_target_index(0, pitch * 99.0f, pitch, 4) == 3);
        CHECK(pulp::view::reorder_target_index(3, -pitch * 99.0f, pitch, 4) == 0);
    }
    SECTION("degenerate pitch / empty list is a no-op") {
        CHECK(pulp::view::reorder_target_index(2, 100.0f, 0.0f, 4) == 2);
        CHECK(pulp::view::reorder_target_index(0, 100.0f, pitch, 0) == 0);
    }
}

namespace {
// Build a 4-item vertical ReorderList (extent 40, gap 6 -> pitch 46) laid out.
std::unique_ptr<ReorderList> make_reorder_list(std::vector<pulp::view::View*>& out) {
    auto list = std::make_unique<ReorderList>();
    list->set_item_extent(40.0f);
    list->set_gap(6.0f);
    list->set_bounds({0, 0, 200, 4 * 46.0f});
    for (int i = 0; i < 4; ++i) {
        auto item = std::make_unique<pulp::view::View>();
        out.push_back(item.get());
        list->add_item(std::move(item));
    }
    list->layout_children();
    return list;
}
}  // namespace

TEST_CASE("ReorderList commits a drag that moves item 0 past item 2",
          "[view][reorder][issue-juce-port]") {
    std::vector<pulp::view::View*> original;
    auto list = make_reorder_list(original);

    int from_seen = -1, to_seen = -1;
    list->on_reorder = [&](int f, int t) { from_seen = f; to_seen = t; };

    // Drag from within item 0 (y ~20) down past item 2 (offset ~2.2 * pitch).
    const float pitch = list->pitch();
    list->simulate_drag({100.0f, 20.0f}, {100.0f, 20.0f + pitch * 2.2f});

    CHECK(from_seen == 0);
    CHECK(to_seen == 2);
    // Display order updated: the old item 0 is now at display index 2; the old
    // items 1 and 2 shifted up to 0 and 1.
    CHECK(list->item_at(2) == original[0]);
    CHECK(list->item_at(0) == original[1]);
    CHECK(list->item_at(1) == original[2]);
    CHECK(list->item_at(3) == original[3]);
    CHECK(list->dragging_index() == -1);  // drag released
}

TEST_CASE("ReorderList no-op drop does not fire on_reorder",
          "[view][reorder][issue-juce-port]") {
    std::vector<pulp::view::View*> original;
    auto list = make_reorder_list(original);
    bool fired = false;
    list->on_reorder = [&](int, int) { fired = true; };

    // Tiny drag within the same slot: target == source.
    list->simulate_drag({100.0f, 20.0f}, {100.0f, 25.0f});
    CHECK_FALSE(fired);
    CHECK(list->item_at(0) == original[0]);  // order unchanged
}

TEST_CASE("ReorderList opens a gap for the lifted item during a drag",
          "[view][reorder][issue-juce-port]") {
    std::vector<pulp::view::View*> original;
    auto list = make_reorder_list(original);
    const float pitch = list->pitch();

    // Begin a drag on item 0 and move it toward slot 2 WITHOUT releasing.
    list->on_mouse_down({100.0f, 20.0f});
    list->on_mouse_drag({100.0f, 20.0f + pitch * 2.0f});
    CHECK(list->dragging_index() == 0);
    CHECK(list->drop_target_index() == 2);
    // Neighbour that was at slot 1 slid up toward the gap (y decreased by a pitch).
    const Rect n1 = original[1]->bounds();
    CHECK(n1.y == Approx(1 * pitch - pitch));  // slot 1 shifted to slot 0's y
    // The lifted item tracks the pointer (offset ~2 pitches from its rest slot).
    const Rect lifted = original[0]->bounds();
    CHECK(lifted.y == Approx(pitch * 2.0f).margin(0.5f));
}

// ── P6.7: Skinnable paint-space control painters ─────────────────────────────
// Mode: RecordingCanvas command-stream assertions (no Skia raster in this
// GPU-OFF build) — each painter's geometry must be value-dependent.

#include <pulp/view/control_painters.hpp>
namespace paint = pulp::view::painters;
using Cmd = pulp::canvas::DrawCommand::Type;

namespace {
// Sweep (radians) of the LAST stroke_arc in a command stream, or -1 if none.
float last_arc_sweep(const pulp::canvas::RecordingCanvas& c) {
    for (auto it = c.commands().rbegin(); it != c.commands().rend(); ++it)
        if (it->type == Cmd::stroke_arc) return it->f[4] - it->f[3];
    return -1.0f;
}
// The LAST fill_rounded_rect (thumb), or nullptr.
const pulp::canvas::DrawCommand* last_rrect(const pulp::canvas::RecordingCanvas& c) {
    const pulp::canvas::DrawCommand* out = nullptr;
    for (auto& cmd : c.commands())
        if (cmd.type == Cmd::fill_rounded_rect) out = &cmd;
    return out;
}
}  // namespace

TEST_CASE("paint_mod_ring_knob active arc sweep grows with value",
          "[view][painters][issue-juce-port]") {
    const pulp::view::Rect r{0, 0, 80, 80};
    pulp::canvas::RecordingCanvas lo, hi;
    paint::paint_mod_ring_knob(lo, r, 0.25f);
    paint::paint_mod_ring_knob(hi, r, 0.75f);

    // Two arcs each (track + active) plus the indicator line.
    CHECK(lo.count(Cmd::stroke_arc) == 2);
    CHECK(lo.count(Cmd::stroke_line) == 1);
    const float sweep_lo = last_arc_sweep(lo);
    const float sweep_hi = last_arc_sweep(hi);
    CHECK(sweep_hi > sweep_lo);
    // 0.75 sweep should be ~3x the 0.25 sweep (both a fraction of 270deg).
    CHECK(sweep_hi == Approx(sweep_lo * 3.0f).epsilon(0.02));
}

TEST_CASE("paint_level_fader thumb tracks the value",
          "[view][painters][issue-juce-port]") {
    const pulp::view::Rect r{10, 10, 20, 200};
    SECTION("vertical: higher value -> thumb higher (smaller y)") {
        pulp::canvas::RecordingCanvas lo, hi;
        paint::paint_level_fader(lo, r, 0.2f);
        paint::paint_level_fader(hi, r, 0.8f);
        const auto* tlo = last_rrect(lo);
        const auto* thi = last_rrect(hi);
        REQUIRE(tlo);
        REQUIRE(thi);
        CHECK(thi->f[1] < tlo->f[1]);   // thumb y decreases as value rises
    }
    SECTION("horizontal: higher value -> thumb further right") {
        pulp::view::painters::FaderStyle st; st.horizontal = true;
        pulp::canvas::RecordingCanvas lo, hi;
        paint::paint_level_fader(lo, {10, 10, 200, 20}, 0.2f, st);
        paint::paint_level_fader(hi, {10, 10, 200, 20}, 0.8f, st);
        CHECK(last_rrect(hi)->f[0] > last_rrect(lo)->f[0]);  // thumb x increases
    }
}

TEST_CASE("paint_range_slider draws two thumbs spanning [lo,hi]",
          "[view][painters][issue-juce-port]") {
    pulp::canvas::RecordingCanvas c;
    paint::paint_range_slider(c, {0, 0, 200, 20}, 0.25f, 0.75f);
    // Two thumb circles.
    REQUIRE(c.count(Cmd::fill_circle) == 2);
    // First circle = lo, second = hi; hi is further right (larger x).
    const pulp::canvas::DrawCommand* first = nullptr;
    const pulp::canvas::DrawCommand* second = nullptr;
    for (auto& cmd : c.commands()) {
        if (cmd.type != Cmd::fill_circle) continue;
        if (!first) first = &cmd; else second = &cmd;
    }
    CHECK(second->f[0] > first->f[0]);

    SECTION("swapped lo/hi is normalized (lo <= hi)") {
        pulp::canvas::RecordingCanvas s;
        paint::paint_range_slider(s, {0, 0, 200, 20}, 0.9f, 0.1f);
        const pulp::canvas::DrawCommand* a = nullptr;
        const pulp::canvas::DrawCommand* b = nullptr;
        for (auto& cmd : s.commands()) {
            if (cmd.type != Cmd::fill_circle) continue;
            if (!a) a = &cmd; else b = &cmd;
        }
        CHECK(a->f[0] <= b->f[0]);   // still lo-then-hi
    }
}

TEST_CASE("paint_toggle knob moves with state",
          "[view][painters][issue-juce-port]") {
    const pulp::view::Rect r{0, 0, 48, 24};
    pulp::canvas::RecordingCanvas off, on;
    paint::paint_toggle(off, r, false);
    paint::paint_toggle(on, r, true);
    // One knob circle each.
    REQUIRE(off.count(Cmd::fill_circle) == 1);
    REQUIRE(on.count(Cmd::fill_circle) == 1);
    float off_x = 0, on_x = 0;
    for (auto& c : off.commands()) if (c.type == Cmd::fill_circle) off_x = c.f[0];
    for (auto& c : on.commands())  if (c.type == Cmd::fill_circle) on_x = c.f[0];
    CHECK(on_x > off_x);   // knob slides right when on
}

TEST_CASE("paint_waveform strokes one segment per sample gap",
          "[view][painters][issue-juce-port]") {
    const float samples[5] = {0.0f, 1.0f, -1.0f, 0.5f, 0.0f};
    pulp::canvas::RecordingCanvas c;
    paint::paint_waveform(c, {0, 0, 100, 40}, samples, 5);
    // baseline (1) + polyline default fallback = 4 segments for 5 points.
    CHECK(c.count(Cmd::stroke_line) == 1 + 4);

    SECTION("too few samples is a no-op") {
        pulp::canvas::RecordingCanvas empty;
        const float one[1] = {0.5f};
        paint::paint_waveform(empty, {0, 0, 100, 40}, one, 1);
        CHECK(empty.command_count() == 0);
        paint::paint_waveform(empty, {0, 0, 100, 40}, nullptr, 8);
        CHECK(empty.command_count() == 0);
    }
}

// ── P6.1: Annotated-capture import lane ──────────────────────────────────────
// Mode: parse + codegen assertions (generated stub is compile-SHAPED, not
// compiled in-process). The manifest parse is the testable core; the generated
// header/source are asserted for the class decl, the typed element table, and
// the host-param wiring.

#include "tools/import-design/annotated_capture.hpp"
namespace impd = pulp::import_design;

namespace {
const char* kManifestJson = R"JSON({
  "name": "Reverb Panel",
  "class": "ReverbPanelView",
  "elements": [
    { "selector": "#mix", "kind": "knob", "param_key": "mix",
      "geometry": {"cx": 120, "cy": 90, "hit_radius": 34, "value": 0.5},
      "needle": "M120 90 L120 60" },
    { "selector": "#gain", "kind": "fader", "param_key": "gain",
      "geometry": {"x": 40, "y": 140, "w": 24, "h": 180, "cx": 52, "cy": 230},
      "needle": "M52 230 L52 230" },
    { "selector": "#type", "kind": "dropdown", "param_key": "type",
      "geometry": {"x": 200, "y": 40, "w": 120, "h": 28},
      "options": ["Hall", "Room", "Plate"], "selected_index": 1 },
    { "selector": "#bypass", "kind": "toggle", "param_key": "bypass",
      "geometry": {"x": 340, "y": 20, "w": 44, "h": 22} }
  ]
})JSON";
}  // namespace

TEST_CASE("parse_annotated_manifest builds the typed element table",
          "[view][annotated-capture][issue-juce-port]") {
    impd::AnnotatedCaptureManifest m;
    std::string err;
    REQUIRE(impd::parse_annotated_manifest(kManifestJson, m, err));
    CHECK(err.empty());
    CHECK(m.name == "Reverb Panel");
    CHECK(m.class_name == "ReverbPanelView");
    REQUIRE(m.elements.size() == 4);

    // Kinds in order.
    CHECK(m.elements[0].kind == DesignFrameElement::Kind::knob);
    CHECK(m.elements[1].kind == DesignFrameElement::Kind::fader);
    CHECK(m.elements[2].kind == DesignFrameElement::Kind::dropdown);
    CHECK(m.elements[3].kind == DesignFrameElement::Kind::toggle);

    // Geometry + attributes for the knob.
    CHECK(m.elements[0].cx == Approx(120.0f));
    CHECK(m.elements[0].cy == Approx(90.0f));
    CHECK(m.elements[0].hit_radius == Approx(34.0f));
    CHECK(m.elements[0].needle_d == "M120 90 L120 60");
    CHECK(m.elements[0].param_key == "mix");
    CHECK(m.elements[0].source_node_id == "#mix");

    // Fader geometry.
    CHECK(m.elements[1].x == Approx(40.0f));
    CHECK(m.elements[1].h == Approx(180.0f));
    CHECK(m.elements[1].param_key == "gain");

    // Dropdown options + selection.
    REQUIRE(m.elements[2].options.size() == 3);
    CHECK(m.elements[2].options[2] == "Plate");
    CHECK(m.elements[2].selected_index == 1);

    CHECK(m.has_param_bindings());
}

TEST_CASE("parse_annotated_manifest reports errors",
          "[view][annotated-capture][issue-juce-port]") {
    impd::AnnotatedCaptureManifest m;
    std::string err;
    SECTION("invalid JSON") {
        CHECK_FALSE(impd::parse_annotated_manifest("{not json", m, err));
        CHECK_FALSE(err.empty());
    }
    SECTION("missing elements array") {
        CHECK_FALSE(impd::parse_annotated_manifest(R"({"name":"x"})", m, err));
        CHECK(err.find("elements") != std::string::npos);
    }
    SECTION("unknown kind") {
        CHECK_FALSE(impd::parse_annotated_manifest(
            R"({"elements":[{"kind":"frobnicator"}]})", m, err));
        CHECK(err.find("frobnicator") != std::string::npos);
    }
}

TEST_CASE("snake_case matches the generated-file convention",
          "[view][annotated-capture][issue-juce-port]") {
    CHECK(impd::snake_case("ReverbPanelView") == "reverb_panel_view");
    CHECK(impd::snake_case("MyView") == "my_view");
}

TEST_CASE("generate_view_header emits a DesignFrameView subclass",
          "[view][annotated-capture][issue-juce-port]") {
    impd::AnnotatedCaptureManifest m;
    std::string err;
    REQUIRE(impd::parse_annotated_manifest(kManifestJson, m, err));
    const std::string hpp = impd::generate_view_header(m);
    CHECK(hpp.find("#pragma once") != std::string::npos);
    CHECK(hpp.find("#include <pulp/view/design_frame_view.hpp>") != std::string::npos);
    CHECK(hpp.find("class ReverbPanelView : public DesignFrameView") != std::string::npos);
    CHECK(hpp.find("ReverbPanelView();") != std::string::npos);
    CHECK(hpp.find("namespace pulp::view {") != std::string::npos);
}

TEST_CASE("generate_view_source emits the element table + host-param wiring",
          "[view][annotated-capture][issue-juce-port]") {
    impd::AnnotatedCaptureManifest m;
    std::string err;
    REQUIRE(impd::parse_annotated_manifest(kManifestJson, m, err));
    const std::string cpp = impd::generate_view_source(
        m, "pulp::view::detail::reverb_panel_view_svg_b64");

    // Ctor + element-builder shape.
    CHECK(cpp.find("ReverbPanelView::ReverbPanelView()") != std::string::npos);
    CHECK(cpp.find("build_reverb_panel_view_elements()") != std::string::npos);
    CHECK(cpp.find("decode_reverb_panel_view_svg()") != std::string::npos);

    // Typed element table content.
    CHECK(cpp.find("DesignFrameElement::Kind::knob") != std::string::npos);
    CHECK(cpp.find("DesignFrameElement::Kind::dropdown") != std::string::npos);
    CHECK(cpp.find("e.needle_d = \"M120 90 L120 60\";") != std::string::npos);
    CHECK(cpp.find("e.param_key = \"mix\";") != std::string::npos);
    // Float fields emit VALID C++ literals ("120.f", not the invalid "120f").
    CHECK(cpp.find("e.cx = 120.f;") != std::string::npos);
    CHECK(cpp.find("e.cx = 120f;") == std::string::npos);
    CHECK(cpp.find("e.options = {\"Hall\", \"Room\", \"Plate\"};") != std::string::npos);
    CHECK(cpp.find("e.selected_index = 1;") != std::string::npos);

    // Host-param wiring is turned on because the manifest declares param_keys.
    CHECK(cpp.find("route_changes_to_host_params(true);") != std::string::npos);

    // Balanced braces (a coarse compile-shape guard).
    const auto opens = std::count(cpp.begin(), cpp.end(), '{');
    const auto closes = std::count(cpp.begin(), cpp.end(), '}');
    CHECK(opens == closes);
}

TEST_CASE("generate_view_source omits host-param wiring when no keys are bound",
          "[view][annotated-capture][issue-juce-port]") {
    impd::AnnotatedCaptureManifest m;
    std::string err;
    REQUIRE(impd::parse_annotated_manifest(
        R"({"class":"PlainView","elements":[{"kind":"value_label",
             "geometry":{"x":1,"y":2,"w":3,"h":4},"text":"C2"}]})", m, err));
    CHECK_FALSE(m.has_param_bindings());
    const std::string cpp = impd::generate_view_source(m, "sym");
    CHECK(cpp.find("route_changes_to_host_params") == std::string::npos);
    CHECK(cpp.find("e.text = \"C2\";") != std::string::npos);
}
