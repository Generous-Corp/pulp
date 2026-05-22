// Phase 4a — Lock-to-source, Path A (generated-TSX/JS rewrite).
//
// Spec: planning/2026-05-18-inspector-direct-manipulation-roadmap.md,
//       "Phase 4 — Lock-to-source" / "Phase 4a — Path A".
//
// These tests prove the round-trip the roadmap calls for:
//   tweak (anchor_id → property_path → value)
//     → lock-to-source
//     → generated source now carries the new value at the right line
//     → a re-import / re-codegen of the tweaked IR yields the same value.
//
// Scope is Path A only — the generated web-compat JS/TSX artifact that
// `pulp import-design` produces. Path B (live React bundle) and Path C
// (DESIGN.md tokens) are out of scope and untested here.

#include <catch2/catch_test_macros.hpp>

#include <pulp/view/design_import.hpp>
#include <pulp/view/anchor_strategy.hpp>
#include <pulp/view/lock_to_source.hpp>

#include <string>

using namespace pulp::view;

namespace {

// Build a small two-element design IR: a frame container with one child
// rectangle. Anchors are assigned via the content-hash strategy (the
// default for v0 / stitch / claude imports).
DesignIR make_panel_ir() {
    IRNode root;
    root.type = "frame";
    root.name = "Panel";
    root.layout.direction = LayoutDirection::column;
    root.layout.padding_top = root.layout.padding_right =
        root.layout.padding_bottom = root.layout.padding_left = 8.0f;
    root.style.background_color = "#222222";
    root.style.width = 320.0f;
    root.style.height = 200.0f;

    IRNode child;
    child.type = "frame";
    child.name = "Card";
    child.style.background_color = "#888888";
    child.style.width = 80.0f;
    child.style.border_radius = 4.0f;
    root.children.push_back(child);

    DesignIR ir;
    ir.root = std::move(root);
    ir.source = DesignSource::v0;
    ir.source_file = "panel.tsx";
    assign_anchors(ir.root, AnchorStrategy::content_hash);
    return ir;
}

// Generated web-compat JS for the panel, with anchor trail comments on.
std::string make_generated_source() {
    CodeGenOptions opts;
    opts.mode = CodeGenMode::web_compat;
    opts.include_comments = true;
    return generate_pulp_js(make_panel_ir(), opts);
}

// Extract the anchor id of the first child node.
std::string first_child_anchor() {
    auto ir = make_panel_ir();
    REQUIRE_FALSE(ir.root.children.empty());
    REQUIRE(ir.root.children.front().stable_anchor_id.has_value());
    return *ir.root.children.front().stable_anchor_id;
}

std::string root_anchor() {
    auto ir = make_panel_ir();
    REQUIRE(ir.root.stable_anchor_id.has_value());
    return *ir.root.stable_anchor_id;
}

// WYSIWYG T5 — a frame with TWO sibling children (Card + Panel) so a reparent
// (drop Card inside Panel) has a real new-parent target.
DesignIR make_two_child_ir() {
    IRNode root;
    root.type = "frame";
    root.name = "Root";
    root.style.background_color = "#222222";
    root.style.width = 320.0f;
    root.style.height = 200.0f;

    IRNode card;
    card.type = "frame";
    card.name = "Card";
    card.style.background_color = "#888888";
    card.style.width = 80.0f;
    root.children.push_back(card);

    IRNode panel;
    panel.type = "frame";
    panel.name = "Panel";
    panel.style.background_color = "#444444";
    panel.style.width = 120.0f;
    root.children.push_back(panel);

    DesignIR ir;
    ir.root = std::move(root);
    ir.source = DesignSource::v0;
    ir.source_file = "two.tsx";
    assign_anchors(ir.root, AnchorStrategy::content_hash);
    return ir;
}

std::string two_child_source() {
    CodeGenOptions opts;
    opts.mode = CodeGenMode::web_compat;
    opts.include_comments = true;
    return generate_pulp_js(make_two_child_ir(), opts);
}

// WYSIWYG T5 (gap #2) — Root has two children: Card (with a nested Inner child)
// and Panel. Dropping Card inside Panel must move the WHOLE Card subtree
// (Card + Inner) physically under Panel in source.
DesignIR make_nested_reparent_ir() {
    IRNode root;
    root.type = "frame";
    root.name = "Root";
    root.style.background_color = "#222222";
    root.style.width = 320.0f;
    root.style.height = 200.0f;

    IRNode card;
    card.type = "frame";
    card.name = "Card";
    card.style.background_color = "#888888";
    card.style.width = 80.0f;
    IRNode inner;
    inner.type = "frame";
    inner.name = "Inner";
    inner.style.background_color = "#aaaaaa";
    inner.style.width = 40.0f;
    card.children.push_back(inner);
    root.children.push_back(card);

    IRNode panel;
    panel.type = "frame";
    panel.name = "Panel";
    panel.style.background_color = "#444444";
    panel.style.width = 120.0f;
    root.children.push_back(panel);

    DesignIR ir;
    ir.root = std::move(root);
    ir.source = DesignSource::v0;
    ir.source_file = "nested.tsx";
    assign_anchors(ir.root, AnchorStrategy::content_hash);
    return ir;
}

std::string nested_reparent_source() {
    CodeGenOptions opts;
    opts.mode = CodeGenMode::web_compat;
    opts.include_comments = true;
    return generate_pulp_js(make_nested_reparent_ir(), opts);
}

// Index (line ordinal) of the first `// @pulp-anchor <id>` comment in `src`,
// or std::string::npos when absent. Used to assert physical ordering: a block
// that moved "under" another sits at a LATER source position.
std::size_t anchor_pos(const std::string& src, const std::string& id) {
    return src.find("// @pulp-anchor " + id);
}

}  // namespace

// ── Property-path mapping ───────────────────────────────────────────────

TEST_CASE("lock_property_to_style_name maps dotted paths", "[lock-to-source][issue-1307]") {
    SECTION("paint namespace") {
        REQUIRE(lock_property_to_style_name("paint.backgroundColor") == "backgroundColor");
        REQUIRE(lock_property_to_style_name("paint.color") == "color");
    }
    SECTION("style namespace") {
        REQUIRE(lock_property_to_style_name("style.width") == "width");
        REQUIRE(lock_property_to_style_name("style.borderRadius") == "borderRadius");
    }
    SECTION("layout namespace") {
        REQUIRE(lock_property_to_style_name("layout.padding") == "padding");
        REQUIRE(lock_property_to_style_name("layout.gap") == "gap");
    }
    SECTION("hyphen + snake fragments camelCase") {
        REQUIRE(lock_property_to_style_name("paint.background-color") == "backgroundColor");
        REQUIRE(lock_property_to_style_name("layout.padding_top") == "paddingTop");
    }
    SECTION("gradient tweaks map to generated background style") {
        REQUIRE(lock_property_to_style_name("paint.backgroundGradient") == "background");
        REQUIRE(lock_property_to_style_name("style.backgroundGradient") == "background");
    }
    SECTION("bare path with no namespace") {
        REQUIRE(lock_property_to_style_name("width") == "width");
    }
    SECTION("empty paths are rejected after trimming") {
        REQUIRE_FALSE(lock_property_to_style_name("   ").has_value());
        REQUIRE_FALSE(lock_property_to_style_name("layout.   ").has_value());
    }
    SECTION("unknown namespace rejected") {
        REQUIRE_FALSE(lock_property_to_style_name("audio.gain").has_value());
    }
    SECTION("unknown property rejected") {
        REQUIRE_FALSE(lock_property_to_style_name("paint.unicornHorn").has_value());
    }
    SECTION("nested layout path is out of Path A scope") {
        REQUIRE_FALSE(lock_property_to_style_name("layout.padding.top").has_value());
    }
    // WYSIWYG T4 — reorder + proportional-resize edits round-trip to source.
    SECTION("layout.order maps to the order style property") {
        REQUIRE(lock_property_to_style_name("layout.order") == "order");
    }
    SECTION("transform.scale collapses onto the transform style property") {
        REQUIRE(lock_property_to_style_name("transform.scale") == "transform");
    }
}

// ── Generated-source detection (the @generated boundary guard) ──────────

TEST_CASE("is_generated_source recognizes the Pulp codegen banner", "[lock-to-source][issue-1307]") {
    const std::string gen = make_generated_source();
    REQUIRE(is_generated_source(gen));

    SECTION("conventional @generated marker also accepted") {
        REQUIRE(is_generated_source("// @generated by some tool\nconst x = 1;\n"));
    }
    SECTION("hand-authored source is not flagged generated") {
        REQUIRE_FALSE(is_generated_source(
            "// MyPanel.tsx — hand written\nexport const Panel = () => <div/>;\n"));
    }
    SECTION("late generated marker is ignored") {
        REQUIRE_FALSE(is_generated_source(
            "// one\n// two\n// three\n// four\n// five\n// six\n// seven\n// eight\n"
            "// @generated too late\n"));
    }
}

// ── Locate by anchor + rewrite a style property ─────────────────────────

TEST_CASE("lock_tweak_into_source rewrites a style property by anchor", "[lock-to-source][issue-1307]") {
    const std::string gen = make_generated_source();
    const std::string anchor = first_child_anchor();

    // The child was generated with backgroundColor '#888888'.
    REQUIRE(gen.find("'#888888'") != std::string::npos);

    LockToSourceTweak tweak{anchor, "paint.backgroundColor", "#5a5a5a"};
    LockResult r = lock_tweak_into_source(gen, tweak);

    REQUIRE(r.status == LockStatus::rewritten);
    REQUIRE(r.ok());
    REQUIRE(r.mutated());
    REQUIRE(r.style_property == "backgroundColor");
    REQUIRE(r.line > 0);

    // The new value is present, the old one gone.
    REQUIRE(r.source.find("backgroundColor = '#5a5a5a'") != std::string::npos);
    REQUIRE(r.source.find("'#888888'") == std::string::npos);

    // Only the child changed — the root still carries its own bg color.
    REQUIRE(r.source.find("'#222222'") != std::string::npos);
}

// ── Rewrite a layout dimension ──────────────────────────────────────────

TEST_CASE("lock_tweak_into_source rewrites a layout dimension", "[lock-to-source][issue-1307]") {
    const std::string gen = make_generated_source();
    const std::string anchor = first_child_anchor();

    // The child was generated with width '80px'.
    REQUIRE(gen.find("width = '80px'") != std::string::npos);

    LockToSourceTweak tweak{anchor, "layout.width", "120px"};
    LockResult r = lock_tweak_into_source(gen, tweak);

    REQUIRE(r.status == LockStatus::rewritten);
    REQUIRE(r.style_property == "width");
    REQUIRE(r.source.find("width = '120px'") != std::string::npos);
    REQUIRE(r.source.find("width = '80px'") == std::string::npos);
}

// ── Insert a property that the codegen did not emit ─────────────────────

TEST_CASE("lock_tweak_into_source inserts a missing property", "[lock-to-source][issue-1307]") {
    const std::string gen = make_generated_source();
    const std::string anchor = first_child_anchor();

    // The child has no opacity assignment in the generated source.
    REQUIRE(gen.find("opacity") == std::string::npos);

    LockToSourceTweak tweak{anchor, "paint.opacity", "0.5"};
    LockResult r = lock_tweak_into_source(gen, tweak);

    REQUIRE(r.status == LockStatus::inserted);
    REQUIRE(r.ok());
    REQUIRE(r.mutated());
    REQUIRE(r.style_property == "opacity");
    REQUIRE(r.source.find(".style.opacity = '0.5'") != std::string::npos);

    // The inserted line sits inside the child's block — i.e. before the
    // next anchor comment or block boundary, not at file scope.
    const auto anchor_pos = r.source.find("// @pulp-anchor " + anchor);
    const auto opacity_pos = r.source.find(".style.opacity");
    REQUIRE(anchor_pos != std::string::npos);
    REQUIRE(opacity_pos != std::string::npos);
    REQUIRE(opacity_pos > anchor_pos);
}

// ── WYSIWYG T4 — reorder + proportional-resize round-trip ───────────────

TEST_CASE("lock_tweak_into_source persists a layout.order reorder",
          "[lock-to-source][wysiwyg][t4]") {
    const std::string gen = make_generated_source();
    const std::string anchor = first_child_anchor();

    // The child has no order assignment in the generated source.
    REQUIRE(gen.find(".style.order") == std::string::npos);

    // A drag-to-reorder gesture rewrote flex().order to 2; locking it should
    // insert `el.style.order = '2'` so the new sibling order survives a
    // re-import.
    LockToSourceTweak tweak{anchor, "layout.order", "2"};
    LockResult r = lock_tweak_into_source(gen, tweak);

    REQUIRE(r.ok());
    REQUIRE(r.mutated());
    REQUIRE(r.style_property == "order");
    REQUIRE(r.source.find(".style.order = '2'") != std::string::npos);
}

TEST_CASE("lock_tweak_into_source persists a transform.scale proportional resize",
          "[lock-to-source][wysiwyg][t4]") {
    const std::string gen = make_generated_source();
    const std::string anchor = first_child_anchor();

    REQUIRE(gen.find(".style.transform") == std::string::npos);

    // The proportional Shift-resize gesture persists a bare scale factor under
    // `transform.scale`. Locking it must wrap the factor into the CSS function
    // form `scale(1.5)` on the single `transform` style line.
    LockToSourceTweak tweak{anchor, "transform.scale", "1.5"};
    LockResult r = lock_tweak_into_source(gen, tweak);

    REQUIRE(r.ok());
    REQUIRE(r.mutated());
    REQUIRE(r.style_property == "transform");
    REQUIRE(r.source.find(".style.transform = 'scale(1.5)'") != std::string::npos);

    // Re-locking the same value is idempotent (the wrapped form already
    // matches), proving the round-trip is stable.
    LockResult again = lock_tweak_into_source(r.source, tweak);
    REQUIRE(again.status == LockStatus::already_current);
    REQUIRE(again.source == r.source);
}

// ── WYSIWYG T5 — structural reparent round-trip ─────────────────────────

TEST_CASE("reparent_in_source rewrites the child's appendChild receiver",
          "[lock-to-source][wysiwyg][t5]") {
    const std::string gen = two_child_source();
    auto ir = make_two_child_ir();
    REQUIRE(ir.root.children.size() == 2);
    const std::string card_anchor = *ir.root.children[0].stable_anchor_id;
    const std::string panel_anchor = *ir.root.children[1].stable_anchor_id;

    // Resolve the two child vars + the original root receiver from the source
    // so the assertions don't hardcode codegen's var-numbering scheme.
    // Both children are appended to the root var initially.
    ReparentToSourceEdit edit{card_anchor, panel_anchor};
    LockResult r = reparent_in_source(gen, edit);

    REQUIRE(r.status == LockStatus::rewritten);
    REQUIRE(r.ok());
    REQUIRE(r.mutated());
    REQUIRE(r.line > 0);
    // The rewrite message records the receiver change.
    REQUIRE(r.message.find("reparented") != std::string::npos);

    // The Card block's appendChild now targets the Panel's var. Find the
    // Panel's var name and assert the Card appends to it.
    // Panel var: the `const <v> = document.createElement` in Panel's block.
    const auto panel_anchor_pos = r.source.find("// @pulp-anchor " + panel_anchor);
    REQUIRE(panel_anchor_pos != std::string::npos);
    const auto panel_const = r.source.find("const ", panel_anchor_pos);
    REQUIRE(panel_const != std::string::npos);
    const auto panel_eq = r.source.find(" =", panel_const);
    const std::string panel_var =
        r.source.substr(panel_const + 6, panel_eq - (panel_const + 6));

    // The Card block's appendChild receiver is now panel_var.
    REQUIRE(r.source.find(panel_var + ".appendChild(") != std::string::npos);
}

TEST_CASE("reparent_in_source is idempotent + graceful on bad anchors",
          "[lock-to-source][wysiwyg][t5]") {
    const std::string gen = two_child_source();
    auto ir = make_two_child_ir();
    const std::string card_anchor = *ir.root.children[0].stable_anchor_id;
    const std::string panel_anchor = *ir.root.children[1].stable_anchor_id;

    LockResult first = reparent_in_source(gen, {card_anchor, panel_anchor});
    REQUIRE(first.status == LockStatus::rewritten);

    SECTION("re-applying the same reparent is a no-op") {
        LockResult again =
            reparent_in_source(first.source, {card_anchor, panel_anchor});
        REQUIRE(again.status == LockStatus::already_current);
        REQUIRE(again.source == first.source);  // byte-identical
    }
    SECTION("unknown child anchor fails gracefully") {
        LockResult bad = reparent_in_source(gen, {"nope", panel_anchor});
        REQUIRE(bad.status == LockStatus::anchor_not_found);
        REQUIRE(bad.source == gen);  // untouched
    }
    SECTION("unknown parent anchor fails gracefully") {
        LockResult bad = reparent_in_source(gen, {card_anchor, "nope"});
        REQUIRE(bad.status == LockStatus::anchor_not_found);
        REQUIRE(bad.source == gen);
    }
}

// WYSIWYG T5 gap #2 — physical block relocation. The receiver rewrite alone
// changes the live DOM parent, but a full reparent must also move the element's
// source block (and its whole subtree) to sit physically under the new parent so
// the generated artifact reads correctly and round-trips a fresh re-import.
TEST_CASE("reparent_in_source physically relocates the child subtree",
          "[lock-to-source][wysiwyg][t5]") {
    const std::string gen = nested_reparent_source();
    auto ir = make_nested_reparent_ir();
    REQUIRE(ir.root.children.size() == 2);
    const std::string card_anchor = *ir.root.children[0].stable_anchor_id;
    const std::string inner_anchor =
        *ir.root.children[0].children[0].stable_anchor_id;
    const std::string panel_anchor = *ir.root.children[1].stable_anchor_id;

    // Before: source order is Card, Inner (under Card), Panel — Card precedes
    // Panel. The Inner block sits inside the Card subtree.
    REQUIRE(anchor_pos(gen, card_anchor) < anchor_pos(gen, panel_anchor));

    LockResult r = reparent_in_source(gen, {card_anchor, panel_anchor});
    REQUIRE(r.status == LockStatus::rewritten);
    REQUIRE(r.mutated());
    REQUIRE(r.message.find("relocated") != std::string::npos);

    // After: the WHOLE Card subtree (Card + Inner) now sits AFTER the Panel
    // anchor in source order — physically relocated under the new parent.
    const std::size_t panel_p = anchor_pos(r.source, panel_anchor);
    const std::size_t card_p = anchor_pos(r.source, card_anchor);
    const std::size_t inner_p = anchor_pos(r.source, inner_anchor);
    REQUIRE(panel_p != std::string::npos);
    REQUIRE(card_p != std::string::npos);
    REQUIRE(inner_p != std::string::npos);
    REQUIRE(panel_p < card_p);   // Card moved below Panel's header
    REQUIRE(card_p < inner_p);   // Inner still nested under Card (subtree intact)

    // The Card block's appendChild receiver now targets the Panel var.
    const auto panel_const = r.source.find("const ", panel_p);
    REQUIRE(panel_const != std::string::npos);
    const auto panel_eq = r.source.find(" =", panel_const);
    const std::string panel_var =
        r.source.substr(panel_const + 6, panel_eq - (panel_const + 6));
    REQUIRE(r.source.find(panel_var + ".appendChild(") != std::string::npos);

    // Inner still appends to Card (the subtree's internal wiring is untouched).
    const auto card_const = r.source.find("const ", card_p);
    const auto card_eq = r.source.find(" =", card_const);
    const std::string card_var =
        r.source.substr(card_const + 6, card_eq - (card_const + 6));
    REQUIRE(r.source.find(card_var + ".appendChild(") != std::string::npos);
}

// WYSIWYG sweep P1 — the reparent edit carries an insertion SLOT. When the
// edit names a preceding sibling (insert_after_anchor_id), the moved block must
// land right AFTER that sibling's subtree under the new parent — not always as
// the parent's first child (the prior behavior that discarded the drop slot).
TEST_CASE("reparent_in_source honors the requested insertion slot",
          "[lock-to-source][wysiwyg][issue-wysiwyg-reflow-slot]") {
    // Root → { Card, Panel{ PanelA, PanelB } }. Reparent Card under Panel,
    // landing AFTER PanelA (i.e. between PanelA and PanelB).
    IRNode root;
    root.type = "frame"; root.name = "Root";
    root.style.background_color = "#222222";
    root.style.width = 320.0f; root.style.height = 200.0f;

    IRNode card;
    card.type = "frame"; card.name = "Card";
    card.style.background_color = "#888888"; card.style.width = 80.0f;
    root.children.push_back(card);

    IRNode panel;
    panel.type = "frame"; panel.name = "Panel";
    panel.style.background_color = "#444444"; panel.style.width = 120.0f;
    IRNode panel_a;
    panel_a.type = "frame"; panel_a.name = "PanelA";
    panel_a.style.background_color = "#555555"; panel_a.style.width = 30.0f;
    IRNode panel_b;
    panel_b.type = "frame"; panel_b.name = "PanelB";
    panel_b.style.background_color = "#666666"; panel_b.style.width = 30.0f;
    panel.children.push_back(panel_a);
    panel.children.push_back(panel_b);
    root.children.push_back(panel);

    DesignIR ir;
    ir.root = std::move(root);
    ir.source = DesignSource::v0;
    ir.source_file = "slot.tsx";
    assign_anchors(ir.root, AnchorStrategy::content_hash);

    CodeGenOptions opts;
    opts.mode = CodeGenMode::web_compat;
    opts.include_comments = true;
    const std::string gen = generate_pulp_js(ir, opts);

    const std::string card_anchor   = *ir.root.children[0].stable_anchor_id;
    const std::string panel_anchor  = *ir.root.children[1].stable_anchor_id;
    const std::string panelA_anchor =
        *ir.root.children[1].children[0].stable_anchor_id;
    const std::string panelB_anchor =
        *ir.root.children[1].children[1].stable_anchor_id;

    SECTION("insert after PanelA → lands between PanelA and PanelB") {
        ReparentToSourceEdit edit{card_anchor, panel_anchor, panelA_anchor};
        LockResult r = reparent_in_source(gen, edit);
        REQUIRE(r.status == LockStatus::rewritten);
        REQUIRE(r.mutated());

        const std::size_t a_p = anchor_pos(r.source, panelA_anchor);
        const std::size_t card_p = anchor_pos(r.source, card_anchor);
        const std::size_t b_p = anchor_pos(r.source, panelB_anchor);
        REQUIRE(a_p != std::string::npos);
        REQUIRE(card_p != std::string::npos);
        REQUIRE(b_p != std::string::npos);
        // Card sits AFTER PanelA and BEFORE PanelB — the requested slot.
        REQUIRE(a_p < card_p);
        REQUIRE(card_p < b_p);
    }

    SECTION("empty slot → first child (PanelA), preserving prior behavior") {
        ReparentToSourceEdit edit{card_anchor, panel_anchor, ""};
        LockResult r = reparent_in_source(gen, edit);
        REQUIRE(r.status == LockStatus::rewritten);
        const std::size_t panel_p = anchor_pos(r.source, panel_anchor);
        const std::size_t card_p = anchor_pos(r.source, card_anchor);
        const std::size_t a_p = anchor_pos(r.source, panelA_anchor);
        // Card lands immediately under Panel, BEFORE PanelA (first child).
        REQUIRE(panel_p < card_p);
        REQUIRE(card_p < a_p);
    }

    SECTION("unresolved slot anchor → graceful first-child fallback") {
        ReparentToSourceEdit edit{card_anchor, panel_anchor, "no-such-anchor"};
        LockResult r = reparent_in_source(gen, edit);
        REQUIRE(r.status == LockStatus::rewritten);
        const std::size_t card_p = anchor_pos(r.source, card_anchor);
        const std::size_t a_p = anchor_pos(r.source, panelA_anchor);
        REQUIRE(card_p < a_p);  // fell back to first-child
    }
}

TEST_CASE("reparent_in_source relocation is idempotent",
          "[lock-to-source][wysiwyg][t5]") {
    const std::string gen = nested_reparent_source();
    auto ir = make_nested_reparent_ir();
    const std::string card_anchor = *ir.root.children[0].stable_anchor_id;
    const std::string panel_anchor = *ir.root.children[1].stable_anchor_id;

    LockResult first = reparent_in_source(gen, {card_anchor, panel_anchor});
    REQUIRE(first.status == LockStatus::rewritten);

    // Re-applying the same reparent to the already-rewritten source is a no-op:
    // the receiver already points at the parent, so the block is not moved again.
    LockResult again =
        reparent_in_source(first.source, {card_anchor, panel_anchor});
    REQUIRE(again.status == LockStatus::already_current);
    REQUIRE(again.source == first.source);  // byte-identical — no drift
}

// WYSIWYG T5 gap #2 — guard: a reparent that cannot be safely relocated must
// skip the physical block move WITHOUT corrupting source. Dropping a parent
// INSIDE its own descendant (Panel under Card, when Card is Panel's ancestor)
// is the canonical unsafe case; the engine rewrites the receiver but leaves the
// block in place, and the source stays well-formed (every anchor still present).
TEST_CASE("reparent_in_source refuses cyclic reparent (parent inside subtree)",
          "[lock-to-source][wysiwyg][t5]") {
    // Build Root → Outer → Inner. Try to reparent Outer UNDER Inner (its own
    // descendant). The live gesture would never allow this (is_self_or_ancestor),
    // but the source engine must defend too. WYSIWYG sweep P2 fix: rewriting the
    // receiver alone would emit `inner.appendChild(outer);` — appending a node
    // into its own descendant, which is cyclic/invalid source. The engine must
    // rewrite NOTHING and return a non-mutating failure with source UNCHANGED.
    IRNode root;
    root.type = "frame";
    root.name = "Root";
    root.style.background_color = "#222222";
    root.style.width = 320.0f;
    root.style.height = 200.0f;
    IRNode outer;
    outer.type = "frame";
    outer.name = "Outer";
    outer.style.background_color = "#888888";
    outer.style.width = 200.0f;
    IRNode inner;
    inner.type = "frame";
    inner.name = "Inner";
    inner.style.background_color = "#aaaaaa";
    inner.style.width = 40.0f;
    outer.children.push_back(inner);
    root.children.push_back(outer);
    DesignIR ir;
    ir.root = std::move(root);
    ir.source = DesignSource::v0;
    ir.source_file = "deep.tsx";
    assign_anchors(ir.root, AnchorStrategy::content_hash);

    CodeGenOptions opts;
    opts.mode = CodeGenMode::web_compat;
    opts.include_comments = true;
    const std::string gen = generate_pulp_js(ir, opts);

    const std::string outer_anchor = *ir.root.children[0].stable_anchor_id;
    const std::string inner_anchor =
        *ir.root.children[0].children[0].stable_anchor_id;

    LockResult r = reparent_in_source(gen, {outer_anchor, inner_anchor});
    // Non-mutating failure: nothing rewritten, message records the skip reason.
    REQUIRE(r.status == LockStatus::anchor_not_found);
    REQUIRE_FALSE(r.mutated());
    REQUIRE(r.message.find("inside the moved subtree") != std::string::npos);

    // Source is BYTE-IDENTICAL to the input — the engine touched nothing, so it
    // can never have produced the cyclic `inner.appendChild(outer);` line.
    REQUIRE(r.source == gen);

    // Both anchors still present; original Outer-before-Inner order is intact.
    REQUIRE(anchor_pos(r.source, outer_anchor) != std::string::npos);
    REQUIRE(anchor_pos(r.source, inner_anchor) != std::string::npos);
    REQUIRE(anchor_pos(r.source, outer_anchor) <
            anchor_pos(r.source, inner_anchor));
    REQUIRE(is_generated_source(r.source));
}

// ── Idempotent re-lock ──────────────────────────────────────────────────

TEST_CASE("lock_tweak_into_source is idempotent when value already current", "[lock-to-source][issue-1307]") {
    const std::string gen = make_generated_source();
    const std::string anchor = first_child_anchor();

    // Lock once.
    LockToSourceTweak tweak{anchor, "paint.backgroundColor", "#5a5a5a"};
    LockResult first = lock_tweak_into_source(gen, tweak);
    REQUIRE(first.status == LockStatus::rewritten);

    // Lock the same tweak again against the already-rewritten source.
    LockResult second = lock_tweak_into_source(first.source, tweak);
    REQUIRE(second.status == LockStatus::already_current);
    REQUIRE(second.ok());
    REQUIRE_FALSE(second.mutated());
    REQUIRE(second.source == first.source);  // byte-identical, no churn
}

// ── Anchor-not-found graceful failure ───────────────────────────────────

TEST_CASE("lock_tweak_into_source fails gracefully on unknown anchor", "[lock-to-source][issue-1307]") {
    const std::string gen = make_generated_source();

    LockToSourceTweak tweak{"sha1:does-not-exist", "paint.backgroundColor", "#000000"};
    LockResult r = lock_tweak_into_source(gen, tweak);

    REQUIRE(r.status == LockStatus::anchor_not_found);
    REQUIRE_FALSE(r.ok());
    REQUIRE_FALSE(r.mutated());
    REQUIRE(r.line == 0);
    REQUIRE(r.source == gen);  // source untouched
    REQUIRE_FALSE(r.message.empty());
}

TEST_CASE("lock_tweak_into_source rejects an empty anchor id",
          "[lock-to-source][coverage][phase3]") {
    const std::string gen = make_generated_source();

    LockResult r = lock_tweak_into_source(gen, {"", "paint.backgroundColor", "#000000"});

    REQUIRE(r.status == LockStatus::anchor_not_found);
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.source == gen);
}

TEST_CASE("lock_tweak_into_source rejects an unsupported property path", "[lock-to-source][issue-1307]") {
    const std::string gen = make_generated_source();
    const std::string anchor = first_child_anchor();

    LockToSourceTweak tweak{anchor, "audio.gain", "0.8"};
    LockResult r = lock_tweak_into_source(gen, tweak);

    REQUIRE(r.status == LockStatus::unsupported_property);
    REQUIRE_FALSE(r.ok());
    REQUIRE(r.source == gen);  // source untouched
    REQUIRE(r.style_property.empty());
}

// ── Value escaping (a value with a quote must not break the JS) ─────────

TEST_CASE("lock_tweak_into_source escapes single quotes in the value", "[lock-to-source][issue-1307]") {
    const std::string gen = make_generated_source();
    const std::string anchor = first_child_anchor();

    LockToSourceTweak tweak{anchor, "paint.fontFamily", "Mike's Font"};
    LockResult r = lock_tweak_into_source(gen, tweak);

    REQUIRE(r.ok());
    // The literal must carry an escaped quote so the JS stays valid.
    REQUIRE(r.source.find("fontFamily = 'Mike\\'s Font'") != std::string::npos);
}

TEST_CASE("lock_tweak_into_source escapes backslashes in inserted values",
          "[lock-to-source][coverage][phase3]") {
    const std::string gen = make_generated_source();
    const std::string anchor = first_child_anchor();

    LockResult r = lock_tweak_into_source(gen, {anchor, "paint.fontFamily", "C:\\Fonts\\Pulp"});

    REQUIRE(r.status == LockStatus::inserted);
    REQUIRE(r.source.find("fontFamily = 'C:\\\\Fonts\\\\Pulp'") != std::string::npos);
}

TEST_CASE("lock_tweak_into_source escapes JS control characters in values",
          "[lock-to-source][issue-1307][issue-2470]") {
    const std::string gen = make_generated_source();
    const std::string anchor = first_child_anchor();

    std::string value = "Line\nTab\tCarriage\r";
    value.push_back(static_cast<char>(0x1F));
    value += "Slash\\Quote'";

    LockResult r = lock_tweak_into_source(gen, {anchor, "paint.fontFamily", value});

    REQUIRE(r.status == LockStatus::inserted);
    REQUIRE(r.source.find(
        "fontFamily = 'Line\\nTab\\tCarriage\\r\\x1FSlash\\\\Quote\\''") !=
        std::string::npos);
    REQUIRE(r.source.find("Line\nTab") == std::string::npos);
}

TEST_CASE("lock_tweak_into_source preserves missing trailing newline",
          "[lock-to-source][coverage][phase3]") {
    const std::string src =
        "// @pulp-anchor solo\n"
        "const solo = document.createElement('div');\n"
        "solo.style.width = '20px';\n"
        "setAnchor(solo._id, 'solo');";

    LockResult r = lock_tweak_into_source(src, {"solo", "layout.width", "24px"});

    REQUIRE(r.status == LockStatus::rewritten);
    REQUIRE_FALSE(r.source.empty());
    REQUIRE(r.source.back() != '\n');
    REQUIRE(r.source.find("solo.style.width = '24px';") != std::string::npos);
}

TEST_CASE("lock_tweak_into_source stops a block at the next anchor",
          "[lock-to-source][coverage][phase3]") {
    const std::string src =
        "// @pulp-anchor first\n"
        "const first = document.createElement('div');\n"
        "first.style.paddingTop = '8px';\n"
        "setAnchor(first._id, 'first');\n"
        "// @pulp-anchor second\n"
        "const second = document.createElement('div');\n"
        "second.style.padding = '2px';\n";

    LockResult r = lock_tweak_into_source(src, {"first", "layout.padding", "4px"});

    REQUIRE(r.status == LockStatus::inserted);
    const auto inserted = r.source.find("first.style.padding = '4px';");
    const auto next_anchor = r.source.find("// @pulp-anchor second");
    REQUIRE(inserted != std::string::npos);
    REQUIRE(next_anchor != std::string::npos);
    REQUIRE(inserted < next_anchor);
    REQUIRE(r.source.find("first.style.paddingTop = '8px';") != std::string::npos);
    REQUIRE(r.source.find("second.style.padding = '2px';") != std::string::npos);
}

TEST_CASE("lock_tweak_into_source falls back to el when declaration is missing",
          "[lock-to-source][coverage][phase3]") {
    const std::string src =
        "// @pulp-anchor orphan\n"
        "setAnchor(orphan._id, 'orphan');\n";

    LockResult r = lock_tweak_into_source(src, {"orphan", "paint.opacity", "0.25"});

    REQUIRE(r.status == LockStatus::inserted);
    REQUIRE(r.source.find("el.style.opacity = '0.25';") != std::string::npos);
    REQUIRE(r.source.find("setAnchor(orphan._id, 'orphan');") != std::string::npos);
}

TEST_CASE("lock_tweak_into_source appends when no tail call is present",
          "[lock-to-source][coverage][phase3]") {
    const std::string src =
        "// @pulp-anchor solo\n"
        "const solo = document.createElement('div');\n"
        "solo.style.width = '20px';";

    LockResult r = lock_tweak_into_source(src, {"solo", "paint.opacity", "0.5"});

    REQUIRE(r.status == LockStatus::inserted);
    REQUIRE(r.line == 4);
    REQUIRE(r.source.find("solo.style.opacity = '0.5';") != std::string::npos);
    REQUIRE_FALSE(r.source.empty());
    REQUIRE(r.source.back() != '\n');
}

// ── Multi-tweak pass over the same element ──────────────────────────────

TEST_CASE("lock_tweaks_into_source chains multiple tweaks", "[lock-to-source][issue-1307]") {
    const std::string gen = make_generated_source();
    const std::string child = first_child_anchor();
    const std::string root = root_anchor();

    std::vector<LockToSourceTweak> tweaks{
        {child, "paint.backgroundColor", "#5a5a5a"},
        {child, "layout.width", "120px"},
        {root, "layout.padding", "16px"},
    };

    auto results = lock_tweaks_into_source(gen, tweaks);
    REQUIRE(results.size() == 3);
    for (const auto& r : results) REQUIRE(r.ok());

    // The final running buffer carries every change.
    const std::string& final_src = results.back().source;
    REQUIRE(final_src.find("backgroundColor = '#5a5a5a'") != std::string::npos);
    REQUIRE(final_src.find("width = '120px'") != std::string::npos);
    REQUIRE(final_src.find("padding = '16px'") != std::string::npos);
}

// ── Full round-trip: tweak → lock → re-import yields the tweaked value ──
//
// The roadmap's acceptance criterion: after locking, a fresh re-import /
// re-codegen of the (now tweaked) design must reproduce the tweaked
// value. Path A's "source" IS the generated artifact, so the round-trip
// is: locking the tweak into the generated text must produce exactly the
// text that codegen would emit had the IR carried the tweaked value all
// along.

TEST_CASE("lock-to-source round-trips against a re-codegen of the tweaked IR", "[lock-to-source][issue-1307]") {
    CodeGenOptions opts;
    opts.mode = CodeGenMode::web_compat;
    opts.include_comments = true;

    // 1. Original IR → generated source.
    DesignIR original = make_panel_ir();
    const std::string gen = generate_pulp_js(original, opts);
    const std::string anchor = *original.root.children.front().stable_anchor_id;

    // 2. Inspector tweak: the user dragged the card's width to 120.
    LockToSourceTweak tweak{anchor, "style.width", "120px"};
    LockResult locked = lock_tweak_into_source(gen, tweak);
    REQUIRE(locked.status == LockStatus::rewritten);

    // 3. The "lock to source" promotion: the same edit applied to the IR.
    //    A subsequent re-import would lower the design with width 120.
    DesignIR tweaked = make_panel_ir();
    tweaked.root.children.front().style.width = 120.0f;
    const std::string regen = generate_pulp_js(tweaked, opts);

    // 4. Round-trip: the locked text must match what a fresh re-import
    //    (re-codegen of the tweaked IR) produces — byte-for-byte.
    REQUIRE(locked.source == regen);

    // And the tweaked value is unambiguously present.
    REQUIRE(locked.source.find("width = '120px'") != std::string::npos);
}

TEST_CASE("lock-to-source round-trips a style-color tweak against re-codegen", "[lock-to-source][issue-1307]") {
    CodeGenOptions opts;
    opts.mode = CodeGenMode::web_compat;
    opts.include_comments = true;

    DesignIR original = make_panel_ir();
    const std::string gen = generate_pulp_js(original, opts);
    const std::string anchor = *original.root.children.front().stable_anchor_id;

    LockToSourceTweak tweak{anchor, "paint.backgroundColor", "#5a5a5a"};
    LockResult locked = lock_tweak_into_source(gen, tweak);
    REQUIRE(locked.status == LockStatus::rewritten);

    DesignIR tweaked = make_panel_ir();
    tweaked.root.children.front().style.background_color = "#5a5a5a";
    const std::string regen = generate_pulp_js(tweaked, opts);

    REQUIRE(locked.source == regen);
}

