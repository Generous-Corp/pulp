// Multi-state design import — capturing N frames of one design into a single
// DesignFrameView, and the swap links that navigate between them.
//
// The frame index a `swap` element targets is POSITIONAL (frame 0 is the
// captured node itself, alternate_frames[i] is frame i+1), so these tests pin
// the three things that make that contract hold end to end: the IR JSON
// round-trips alternate frames IN ORDER, both generators emit exactly one frame
// per alternate in that order, and a swap that names a frame nobody captured is
// reported through the import report rather than rendering as a dead button.

#include <catch2/catch_test_macros.hpp>

#include <pulp/view/design_codegen.hpp>
#include <pulp/view/design_frame_view.hpp>
#include <pulp/view/design_import.hpp>
#include <pulp/view/design_ir.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace pulp::view;

namespace {

// Two minimal "designs", each an 80x80 panel rect at (10,10) inside a 100x100
// frame, so panel auto-detect maps an 80x80 view 1:1 onto panel coordinates.
// The fill differs per frame so a generator can't pass by emitting frame 0 twice.
std::string frame_svg(const char* fill) {
    return std::string(R"SVG(<svg width="100" height="100" xmlns="http://www.w3.org/2000/svg">)SVG"
                       R"SVG(<rect x="10" y="10" width="80" height="80" rx="4" fill=")SVG") +
           fill + R"SVG("/></svg>)SVG";
}

// Inline the SVG as a data: URI so the asset resolves with no file on disk.
IRAssetRef svg_asset(const std::string& id, const std::string& svg) {
    IRAssetRef a;
    a.asset_id = id;
    a.mime = "image/svg+xml";
    a.original_uri = "data:image/svg+xml," + svg;
    return a;
}

IRInteractiveElement swap_to(int target_frame) {
    IRInteractiveElement e;
    e.kind = InteractiveElementKind::swap;
    e.x = 20; e.y = 20; e.w = 20; e.h = 10;
    e.target_frame = target_frame;
    return e;
}

// A two-state design: frame 0 ("Typing") swaps to frame 1, frame 1 ("Piano")
// swaps back to frame 0 — the shape musical_typing_keyboard builds by hand.
DesignIR make_two_frame_ir() {
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.name = "Typing";
    ir.root.render_mode = NodeRenderMode::faithful_svg;
    ir.root.svg_asset_id = "svg-typing";
    ir.root.style.width = 100.0f;
    ir.root.style.height = 100.0f;
    ir.root.interactive_elements.push_back(swap_to(1));

    IRNode piano;
    piano.type = "frame";
    piano.name = "Piano";
    piano.render_mode = NodeRenderMode::faithful_svg;
    piano.svg_asset_id = "svg-piano";
    piano.style.width = 100.0f;
    piano.style.height = 100.0f;
    piano.interactive_elements.push_back(swap_to(0));
    ir.root.alternate_frames.push_back(piano);

    ir.asset_manifest.assets.push_back(svg_asset("svg-typing", frame_svg("#111111")));
    ir.asset_manifest.assets.push_back(svg_asset("svg-piano", frame_svg("#222222")));
    return ir;
}

// A single-state design — the common path, and the regression guard.
DesignIR make_one_frame_ir() {
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.name = "Only";
    ir.root.render_mode = NodeRenderMode::faithful_svg;
    ir.root.svg_asset_id = "svg-typing";
    ir.root.style.width = 100.0f;
    ir.root.style.height = 100.0f;
    ir.asset_manifest.assets.push_back(svg_asset("svg-typing", frame_svg("#111111")));
    return ir;
}

std::size_t count_occurrences(const std::string& haystack, const std::string& needle) {
    std::size_t n = 0;
    for (std::size_t at = haystack.find(needle); at != std::string::npos;
         at = haystack.find(needle, at + needle.size()))
        ++n;
    return n;
}

DesignFrameView* as_frame_view(View* v) { return dynamic_cast<DesignFrameView*>(v); }

}  // namespace

// ── IR contract ──────────────────────────────────────────────────────────────

TEST_CASE("design IR round-trips alternate frames in capture order",
          "[design-import][multi-state][ir]") {
    DesignIR ir = make_two_frame_ir();
    // A third frame makes an order regression (e.g. a reversed or sorted write)
    // observable — two frames alone can survive a swap of the two.
    IRNode third;
    third.type = "frame";
    third.name = "Drums";
    third.render_mode = NodeRenderMode::faithful_svg;
    third.svg_asset_id = "svg-drums";
    ir.root.alternate_frames.push_back(third);
    ir.asset_manifest.assets.push_back(svg_asset("svg-drums", frame_svg("#333333")));

    const DesignIR back = parse_design_ir_json(serialize_design_ir(ir));

    REQUIRE(back.root.alternate_frames.size() == 2);
    CHECK(back.root.alternate_frames[0].name == "Piano");
    CHECK(back.root.alternate_frames[1].name == "Drums");
    CHECK(back.root.alternate_frames[0].render_mode == NodeRenderMode::faithful_svg);
    CHECK(back.root.alternate_frames[0].svg_asset_id.value_or("") == "svg-piano");
    // The alternate's own swap-back link survives the trip.
    REQUIRE(back.root.alternate_frames[0].interactive_elements.size() == 1);
    CHECK(back.root.alternate_frames[0].interactive_elements[0].kind ==
          InteractiveElementKind::swap);
    CHECK(back.root.alternate_frames[0].interactive_elements[0].target_frame == 0);
}

TEST_CASE("a single-state design serializes no alternate-frames key",
          "[design-import][multi-state][ir]") {
    const std::string json = serialize_design_ir(make_one_frame_ir());
    CHECK(json.find("alternate_frames") == std::string::npos);
    CHECK(parse_design_ir_json(json).root.alternate_frames.empty());
}

// ── C++ codegen ──────────────────────────────────────────────────────────────

TEST_CASE("C++ codegen emits one add_frame per alternate frame",
          "[design-import][multi-state][codegen]") {
    const DesignIR ir = make_two_frame_ir();
    const auto result = generate_pulp_cpp(ir, ir.asset_manifest, {});

    // One alternate → exactly one add_frame, and both frames' swap targets are
    // carried through to the generated element list.
    CHECK(count_occurrences(result.source, "->add_frame(") == 1);
    CHECK(count_occurrences(result.source, "make_unique<pulp::view::DesignFrameView>") == 1);
    CHECK(result.source.find("el.target_frame = 1;") != std::string::npos);
    CHECK(result.source.find("el.target_frame = 0;") != std::string::npos);
}

TEST_CASE("C++ codegen leaves a single-state design without add_frame",
          "[design-import][multi-state][codegen]") {
    const DesignIR ir = make_one_frame_ir();
    const auto result = generate_pulp_cpp(ir, ir.asset_manifest, {});
    CHECK(count_occurrences(result.source, "make_unique<pulp::view::DesignFrameView>") == 1);
    CHECK(result.source.find("->add_frame(") == std::string::npos);
}

// ── Native materializer + runtime behavior ───────────────────────────────────

TEST_CASE("materialized multi-state design has a working swap",
          "[design-import][multi-state][materialize]") {
    const DesignIR ir = make_two_frame_ir();
    auto view = build_native_view_tree(ir, ir.asset_manifest, {});
    REQUIRE(view != nullptr);
    DesignFrameView* frame = as_frame_view(view.get());
    REQUIRE(frame != nullptr);

    REQUIRE(frame->frame_count() == 2);
    CHECK(frame->active_frame() == 0);

    // The panel is the 80x80 rect at (10,10), so an 80x80 view maps 1:1 onto it:
    // view (x,y) -> SVG (x+10, y+10). The swap rect is SVG [20,20,20,10].
    frame->set_bounds({0, 0, 80, 80});
    frame->on_mouse_down({20, 15});          // -> SVG (30,25): inside the swap rect
    CHECK(frame->active_frame() == 1);

    // Frame 1 carries its own swap back to frame 0 at the same rect.
    frame->on_mouse_down({20, 15});
    CHECK(frame->active_frame() == 0);

    // A click outside the swap rect changes nothing.
    frame->on_mouse_down({70, 70});
    CHECK(frame->active_frame() == 0);
}

TEST_CASE("materialized single-state design still has exactly one frame",
          "[design-import][multi-state][materialize]") {
    const DesignIR ir = make_one_frame_ir();
    auto view = build_native_view_tree(ir, ir.asset_manifest, {});
    REQUIRE(view != nullptr);
    DesignFrameView* frame = as_frame_view(view.get());
    REQUIRE(frame != nullptr);
    CHECK(frame->frame_count() == 1);
    CHECK(frame->active_frame() == 0);
}

// ── Missing-frame honesty ────────────────────────────────────────────────────

TEST_CASE("a swap targeting an uncaptured frame is reported, not silently dead",
          "[design-import][multi-state][honesty]") {
    DesignIR ir = make_two_frame_ir();
    // Two frames captured (0 and 1); this button names a third that nobody
    // captured — clicking it would do nothing at all.
    ir.root.interactive_elements.push_back(swap_to(2));

    const int flagged = apply_swap_target_verification(ir.root);
    CHECK(flagged == 1);

    const auto& bad = ir.root.interactive_elements[1];
    CHECK_FALSE(bad.verification_pass);
    REQUIRE(bad.conflict_signals.size() == 1);
    CHECK(bad.conflict_signals[0].find("targets frame 2") != std::string::npos);
    CHECK(bad.conflict_signals[0].find("only 2 frames captured") != std::string::npos);

    // The valid swaps on both frames are untouched.
    CHECK(ir.root.interactive_elements[0].verification_pass);
    CHECK(ir.root.alternate_frames[0].interactive_elements[0].verification_pass);

    // It reaches the same channel --import-report / --fail-on-unresolved read.
    const auto report = collect_import_report(ir.root);
    CHECK(report.conflicted == 1);
    CHECK_FALSE(report.ok());
}

TEST_CASE("a swap with no target frame is reported",
          "[design-import][multi-state][honesty]") {
    DesignIR ir = make_two_frame_ir();
    ir.root.interactive_elements.push_back(swap_to(-1));   // e.g. a layer named "swap"

    CHECK(apply_swap_target_verification(ir.root) == 1);
    const auto& bad = ir.root.interactive_elements[1];
    CHECK_FALSE(bad.verification_pass);
    REQUIRE(bad.conflict_signals.size() == 1);
    CHECK(bad.conflict_signals[0].find("no target frame") != std::string::npos);
    CHECK_FALSE(collect_import_report(ir.root).ok());
}

TEST_CASE("a swap on an alternate frame is checked against the owner's frame set",
          "[design-import][multi-state][honesty]") {
    DesignIR ir = make_two_frame_ir();
    // The swap-back button on frame 1, re-pointed at a frame that doesn't exist.
    ir.root.alternate_frames[0].interactive_elements[0].target_frame = 5;

    CHECK(apply_swap_target_verification(ir.root) == 1);
    CHECK_FALSE(ir.root.alternate_frames[0].interactive_elements[0].verification_pass);
    // Controls on an alternate frame reach the report at all.
    const auto report = collect_import_report(ir.root);
    CHECK(report.controls.size() == 2);
    CHECK(report.conflicted == 1);
    CHECK_FALSE(report.ok());
}

TEST_CASE("a single-state design's swaps are still verified",
          "[design-import][multi-state][honesty]") {
    // The regression edge: before multi-state capture a swap could only ever be
    // dead, so this must flag rather than quietly pass.
    DesignIR ir = make_one_frame_ir();
    ir.root.interactive_elements.push_back(swap_to(1));
    CHECK(apply_swap_target_verification(ir.root) == 1);
    REQUIRE(ir.root.interactive_elements[0].conflict_signals.size() == 1);
    CHECK(ir.root.interactive_elements[0].conflict_signals[0].find("only 1 frame captured") !=
          std::string::npos);
    CHECK_FALSE(collect_import_report(ir.root).ok());
}

TEST_CASE("a design with no swap links is untouched by swap verification",
          "[design-import][multi-state][honesty]") {
    DesignIR ir;
    ir.root.type = "frame";
    IRInteractiveElement knob;
    knob.kind = InteractiveElementKind::knob;
    knob.cx = 50; knob.cy = 50; knob.hit_radius = 20;
    ir.root.interactive_elements.push_back(knob);

    CHECK(apply_swap_target_verification(ir.root) == 0);
    CHECK(ir.root.interactive_elements[0].verification_pass);
    CHECK(collect_import_report(ir.root).ok());
}

TEST_CASE("a swap targeting the frame it sits on is reported as a no-op",
          "[design-import][multi-state][honesty]") {
    DesignIR ir = make_two_frame_ir();
    // Frame 0's button re-pointed at frame 0: set_active_frame(0) on the frame
    // already showing, so the button renders but does nothing.
    ir.root.interactive_elements[0].target_frame = 0;

    CHECK(apply_swap_target_verification(ir.root) == 1);
    const auto& self = ir.root.interactive_elements[0];
    CHECK_FALSE(self.verification_pass);
    REQUIRE(self.conflict_signals.size() == 1);
    CHECK(self.conflict_signals[0].find("already sits on") != std::string::npos);
    CHECK_FALSE(collect_import_report(ir.root).ok());

    // The alternate's swap back to frame 0 is NOT a self-target — it sits on
    // frame 1 — so the check must not flag it just for naming frame 0.
    CHECK(ir.root.alternate_frames[0].interactive_elements[0].verification_pass);
}

TEST_CASE("an alternate frame's swap onto itself is reported against its own index",
          "[design-import][multi-state][honesty]") {
    DesignIR ir = make_two_frame_ir();
    ir.root.alternate_frames[0].interactive_elements[0].target_frame = 1;  // frame 1 → frame 1

    CHECK(apply_swap_target_verification(ir.root) == 1);
    const auto& self = ir.root.alternate_frames[0].interactive_elements[0];
    CHECK_FALSE(self.verification_pass);
    REQUIRE(self.conflict_signals.size() == 1);
    CHECK(self.conflict_signals[0].find("already sits on") != std::string::npos);
    CHECK(ir.root.interactive_elements[0].verification_pass);
}

TEST_CASE("a negative swap target is reported as an index, not as unset",
          "[design-import][multi-state][honesty]") {
    // -1 means "no target parsed"; any other negative means a target WAS parsed
    // and is out of range. Reporting the second as the first sends the fix at the
    // layer's name instead of its number.
    DesignIR ir = make_two_frame_ir();
    ir.root.interactive_elements.push_back(swap_to(-5));

    CHECK(apply_swap_target_verification(ir.root) == 1);
    const auto& bad = ir.root.interactive_elements[1];
    REQUIRE(bad.conflict_signals.size() == 1);
    CHECK(bad.conflict_signals[0].find("targets frame -5") != std::string::npos);
    CHECK(bad.conflict_signals[0].find("not a frame index") != std::string::npos);
    CHECK(bad.conflict_signals[0].find("no target frame") == std::string::npos);
}

// ── frames nobody can render ────────────────────────────────────────────────

TEST_CASE("alternate frames on a non-faithful node are reported as unrenderable",
          "[design-import][multi-state][honesty]") {
    // What every capture lane that emits a widget-recognition tree produces: the
    // states merge into the IR, but no generator lowers them, so they would be
    // dropped and the import would look like it worked.
    DesignIR ir = make_two_frame_ir();
    ir.root.render_mode = NodeRenderMode::normal;

    const auto dropped = find_unrenderable_alternate_frames(ir.root);
    REQUIRE(dropped.size() == 1);
    CHECK(dropped[0].node_name == "Typing");
    CHECK(dropped[0].alternates == 1);
    CHECK(dropped[0].reason.find("faithful_svg") != std::string::npos);
}

TEST_CASE("alternate frames on a faithful node with no asset are reported",
          "[design-import][multi-state][honesty]") {
    DesignIR ir = make_two_frame_ir();
    ir.root.svg_asset_id.reset();

    const auto dropped = find_unrenderable_alternate_frames(ir.root);
    REQUIRE(dropped.size() == 1);
    CHECK(dropped[0].reason.find("svg_asset_id") != std::string::npos);
}

TEST_CASE("a renderable frame set reports nothing unrenderable",
          "[design-import][multi-state][honesty]") {
    const DesignIR ir = make_two_frame_ir();
    CHECK(find_unrenderable_alternate_frames(ir.root).empty());
    // And a design with no alternates at all is trivially fine.
    CHECK(find_unrenderable_alternate_frames(make_one_frame_ir().root).empty());
}

TEST_CASE("an unrenderable frame set nested under the root is found",
          "[design-import][multi-state][honesty]") {
    // The walk has to descend children AND alternates: a frame-owning node can
    // sit on either axis, and one missed is a silently dropped state.
    DesignIR ir = make_one_frame_ir();
    IRNode child;
    child.type = "frame";
    child.name = "Inner";
    child.render_mode = NodeRenderMode::normal;
    child.alternate_frames.push_back(IRNode{});
    ir.root.children.push_back(child);

    IRNode buried;
    buried.type = "frame";
    buried.name = "Buried";
    buried.render_mode = NodeRenderMode::normal;
    buried.alternate_frames.push_back(IRNode{});
    IRNode alt;
    alt.type = "frame";
    alt.name = "Alt";
    alt.render_mode = NodeRenderMode::faithful_svg;
    alt.svg_asset_id = "svg-piano";
    alt.children.push_back(buried);
    ir.root.alternate_frames.push_back(alt);

    // Both are reported; the order the walk happens to reach them in is not part
    // of the contract, so this asserts membership rather than pinning it.
    const auto dropped = find_unrenderable_alternate_frames(ir.root);
    REQUIRE(dropped.size() == 2);
    std::vector<std::string> names;
    for (const auto& d : dropped) names.push_back(d.node_name);
    CHECK(std::find(names.begin(), names.end(), "Inner") != names.end());
    CHECK(std::find(names.begin(), names.end(), "Buried") != names.end());
}
