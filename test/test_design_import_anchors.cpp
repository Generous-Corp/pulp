// Phase 0a: tests for stable_anchor_id assignment on IRNode trees.
// Spec: planning/2026-05-18-inspector-direct-manipulation-roadmap.md
// Mirrors the TS-side tests in packages/pulp-import-ir/tests/anchors.test.ts.

#include <catch2/catch_test_macros.hpp>
#include <pulp/view/anchor_strategy.hpp>
#include <pulp/view/design_import.hpp>

#include <string>
#include <unordered_set>

using namespace pulp::view;

namespace {

// Tiny tree builder so test cases stay readable.
IRNode make_node(std::string type, std::string text = {}, std::string role = {},
                 std::string source_node_id = {}) {
    IRNode n;
    n.type = std::move(type);
    n.text_content = std::move(text);
    if (!role.empty()) n.attributes["role"] = std::move(role);
    if (!source_node_id.empty()) n.source_node_id = std::move(source_node_id);
    return n;
}

// Recursive count of nodes whose stable_anchor_id was populated.
std::size_t count_anchored(const IRNode& node) {
    std::size_t n = (node.stable_anchor_id && !node.stable_anchor_id->empty()) ? 1 : 0;
    for (const auto& c : node.children) n += count_anchored(c);
    return n;
}

// Collect every anchor in the tree (root + descendants).
void collect_anchors(const IRNode& node, std::vector<std::string>& out) {
    if (node.stable_anchor_id) out.push_back(*node.stable_anchor_id);
    for (const auto& c : node.children) collect_anchors(c, out);
}

// Recursive total node count.
std::size_t count_nodes(const IRNode& node) {
    std::size_t n = 1;
    for (const auto& c : node.children) n += count_nodes(c);
    return n;
}

}  // namespace

// ── default_anchor_strategy() — should match DEFAULT_ANCHOR_STRATEGY in
//    @pulp/import-ir/src/anchors.ts.
TEST_CASE("default_anchor_strategy mirrors the TS map", "[view][import][anchors]") {
    REQUIRE(default_anchor_strategy(DesignSource::figma) == AnchorStrategy::adapter);
    REQUIRE(default_anchor_strategy(DesignSource::pencil) == AnchorStrategy::adapter);
    REQUIRE(default_anchor_strategy(DesignSource::stitch) == AnchorStrategy::content_hash);
    REQUIRE(default_anchor_strategy(DesignSource::v0) == AnchorStrategy::content_hash);
    REQUIRE(default_anchor_strategy(DesignSource::claude) == AnchorStrategy::content_hash);
    REQUIRE(default_anchor_strategy(DesignSource::jsx) == AnchorStrategy::content_hash);
}

// ── content-hash strategy ───────────────────────────────────────────────

TEST_CASE("content-hash anchors are populated on every node", "[view][import][anchors]") {
    IRNode root = make_node("frame", {}, "container");
    root.children.push_back(make_node("text", "Hello"));
    root.children.push_back(make_node("button", "OK"));

    assign_anchors(root, AnchorStrategy::content_hash);

    REQUIRE(count_anchored(root) == 3);
    REQUIRE(count_nodes(root) == 3);
}

TEST_CASE("content-hash anchors are deterministic across re-runs",
          "[view][import][anchors]") {
    auto build = []() {
        IRNode r = make_node("frame", {}, "container");
        r.children.push_back(make_node("text", "label"));
        r.children.push_back(make_node("text", "value"));
        return r;
    };
    IRNode a = build(), b = build();
    assign_anchors(a, AnchorStrategy::content_hash);
    assign_anchors(b, AnchorStrategy::content_hash);

    std::vector<std::string> aa, bb;
    collect_anchors(a, aa);
    collect_anchors(b, bb);
    REQUIRE(aa == bb);
}

TEST_CASE("content-hash discriminates duplicate-signature siblings",
          "[view][import][anchors]") {
    // Three identical {tag, role, text} buttons get three distinct
    // anchors via the sigIndex discriminator. This is the bug the TS
    // discriminator fixes (sibling collision).
    IRNode root = make_node("frame");
    for (int i = 0; i < 3; ++i) {
        root.children.push_back(make_node("button", "OK"));
    }

    assign_anchors(root, AnchorStrategy::content_hash);

    std::unordered_set<std::string> seen;
    for (const auto& c : root.children) {
        REQUIRE(c.stable_anchor_id.has_value());
        REQUIRE(seen.insert(*c.stable_anchor_id).second);
    }
    REQUIRE(seen.size() == 3);
}

TEST_CASE("content-hash whitespace is normalized", "[view][import][anchors]") {
    // Two semantically-identical text-only trees that differ only by
    // surrounding whitespace should produce identical anchors at the
    // text node.
    IRNode a = make_node("text", "  hello  world  ");
    IRNode b = make_node("text", "hello world");
    assign_anchors(a, AnchorStrategy::content_hash);
    assign_anchors(b, AnchorStrategy::content_hash);
    REQUIRE(a.stable_anchor_id == b.stable_anchor_id);
}

// ── path strategy ───────────────────────────────────────────────────────

TEST_CASE("path anchors encode tree position", "[view][import][anchors]") {
    IRNode root = make_node("frame");
    root.children.push_back(make_node("text", "A"));   // frame[0]/text[0]
    root.children.push_back(make_node("text", "B"));   // frame[0]/text[1]
    root.children.push_back(make_node("button", "C")); // frame[0]/button[0]

    assign_anchors(root, AnchorStrategy::path);

    REQUIRE(root.stable_anchor_id == "frame[0]");
    REQUIRE(root.children[0].stable_anchor_id == "frame[0]/text[0]");
    REQUIRE(root.children[1].stable_anchor_id == "frame[0]/text[1]");
    REQUIRE(root.children[2].stable_anchor_id == "frame[0]/button[0]");
}

TEST_CASE("path anchors survive text edits but break on sibling reorder",
          "[view][import][anchors]") {
    // Edit the text of a leaf — path is unchanged.
    IRNode a = make_node("frame");
    a.children.push_back(make_node("text", "before"));
    IRNode b = make_node("frame");
    b.children.push_back(make_node("text", "after"));
    assign_anchors(a, AnchorStrategy::path);
    assign_anchors(b, AnchorStrategy::path);
    REQUIRE(a.children[0].stable_anchor_id == b.children[0].stable_anchor_id);

    // Reorder same-tag siblings — anchors swap, demonstrating the
    // documented trade-off.
    IRNode c = make_node("frame");
    c.children.push_back(make_node("text", "X"));
    c.children.push_back(make_node("text", "Y"));
    IRNode d = make_node("frame");
    d.children.push_back(make_node("text", "Y"));
    d.children.push_back(make_node("text", "X"));
    assign_anchors(c, AnchorStrategy::path);
    assign_anchors(d, AnchorStrategy::path);
    REQUIRE(c.children[0].stable_anchor_id == d.children[0].stable_anchor_id);
}

// ── adapter strategy ────────────────────────────────────────────────────

TEST_CASE("adapter anchors use source_node_id when present",
          "[view][import][anchors]") {
    IRNode root = make_node("frame", {}, {}, "0:1");
    root.children.push_back(make_node("button", "OK", {}, "0:42"));
    root.children.push_back(make_node("text", "Hello", {}, "0:99"));

    assign_anchors(root, AnchorStrategy::adapter, "figma");

    REQUIRE(root.stable_anchor_id == "figma:0:1");
    REQUIRE(root.children[0].stable_anchor_id == "figma:0:42");
    REQUIRE(root.children[1].stable_anchor_id == "figma:0:99");
}

TEST_CASE("adapter strategy falls back to content-hash for nodes without IDs",
          "[view][import][anchors]") {
    // Phase 0a soft-fail: missing source_node_id shouldn't crash the
    // pipeline. The fallback uses the content-hash branch so the node
    // still gets SOME anchor — better for downstream inspector use than
    // an empty anchor.
    IRNode root = make_node("frame", {}, {}, "0:1");
    root.children.push_back(make_node("button", "OK"));  // no source_node_id

    assign_anchors(root, AnchorStrategy::adapter, "figma");

    REQUIRE(root.stable_anchor_id == "figma:0:1");
    REQUIRE(root.children[0].stable_anchor_id.has_value());
    // Fallback is content-hash, so the value is NOT prefixed with "figma:".
    REQUIRE(root.children[0].stable_anchor_id->find("figma:") == std::string::npos);
}

// ── pre-existing anchor preservation ────────────────────────────────────

TEST_CASE("assign_anchors preserves pre-existing stable_anchor_id values",
          "[view][import][anchors]") {
    // If an authored override has already set stable_anchor_id, the
    // walker must leave it alone. This is how Phase 1 will honor
    // user-pinned anchors for elements they want to track precisely.
    IRNode root = make_node("frame");
    root.stable_anchor_id = "pinned-by-user";
    root.children.push_back(make_node("text", "Hello"));

    assign_anchors(root, AnchorStrategy::content_hash);

    REQUIRE(root.stable_anchor_id == "pinned-by-user");
    REQUIRE(root.children[0].stable_anchor_id.has_value());
    REQUIRE(root.children[0].stable_anchor_id != "pinned-by-user");
}

// ── parser entry-point integration ──────────────────────────────────────
//
// These verify the parsers we wired in Phase 0a actually call
// assign_anchors with the right strategy and produce stable output
// across re-imports of the same source.

TEST_CASE("parse_figma_json populates anchors via the adapter strategy",
          "[view][import][anchors]") {
    const std::string json = R"({
        "type": "frame",
        "id": "0:1",
        "children": [
            { "type": "button", "id": "0:42" },
            { "type": "text", "content": "Hi", "id": "0:99" }
        ]
    })";

    auto ir = parse_figma_json(json);

    REQUIRE(ir.root.stable_anchor_id == "figma:0:1");
    REQUIRE(ir.root.children.size() == 2);
    REQUIRE(ir.root.children[0].stable_anchor_id == "figma:0:42");
    REQUIRE(ir.root.children[1].stable_anchor_id == "figma:0:99");
    REQUIRE(ir.root.provenance.has_value());
    REQUIRE(ir.root.provenance->adapter == "figma");
    REQUIRE(ir.root.confidence == IRConfidence::pass);
}

TEST_CASE("parse_pencil_json populates anchors via the adapter strategy",
          "[view][import][anchors]") {
    const std::string json = R"({
        "type": "frame",
        "id": "pencil-1",
        "children": [
            { "type": "button", "id": "pencil-42" }
        ]
    })";

    auto ir = parse_pencil_json(json);

    REQUIRE(ir.root.stable_anchor_id == "pencil:pencil-1");
    REQUIRE(ir.root.children[0].stable_anchor_id == "pencil:pencil-42");
    REQUIRE(ir.root.provenance->adapter == "pencil");
}

TEST_CASE("parse_stitch_html JSON path populates content-hash anchors",
          "[view][import][anchors]") {
    const std::string json = R"({
        "type": "frame",
        "children": [
            { "type": "text", "content": "Hello" }
        ]
    })";

    auto a = parse_stitch_html(json);
    auto b = parse_stitch_html(json);

    REQUIRE(a.root.stable_anchor_id.has_value());
    REQUIRE(a.root.stable_anchor_id == b.root.stable_anchor_id);
    REQUIRE(a.root.children[0].stable_anchor_id == b.root.children[0].stable_anchor_id);
    REQUIRE(a.root.provenance->adapter == "stitch-html");
}

TEST_CASE("parse_claude_html re-tags provenance after delegating to stitch",
          "[view][import][anchors]") {
    const std::string json = R"({
        "type": "frame",
        "children": [{ "type": "text", "content": "Hi" }]
    })";

    auto ir = parse_claude_html(json);

    REQUIRE(ir.source == DesignSource::claude);
    REQUIRE(ir.root.provenance.has_value());
    REQUIRE(ir.root.provenance->adapter == "claude-design-html");
    // Anchors should be populated by the upstream stitch parser; we
    // don't reassign in parse_claude_html (same strategy).
    REQUIRE(ir.root.stable_anchor_id.has_value());
}

TEST_CASE("parse_stitch_html regex-fallback path also assigns anchors",
          "[view][import][anchors]") {
    // Non-JSON input forces the regex fallback at the end of
    // parse_stitch_html. The Phase 0a additions stamp provenance +
    // confidence=DIVERGE and call assign_anchors on the regex-built tree.
    const std::string html = "<div><span>hello</span><span>world</span></div>";
    auto ir = parse_stitch_html(html);

    REQUIRE(ir.source == DesignSource::stitch);
    REQUIRE(ir.root.provenance.has_value());
    REQUIRE(ir.root.provenance->adapter == "stitch-html");
    REQUIRE(ir.root.confidence == IRConfidence::diverge);
    REQUIRE(ir.root.stable_anchor_id.has_value());
    // Children built by regex extraction also get anchors.
    REQUIRE(!ir.root.children.empty());
    REQUIRE(ir.root.children[0].stable_anchor_id.has_value());
}

TEST_CASE("parse_v0_tsx JSON path populates content-hash anchors",
          "[view][import][anchors]") {
    const std::string json = R"({
        "type": "frame",
        "children": [{ "type": "text", "content": "Hello" }]
    })";
    auto ir = parse_v0_tsx(json);

    REQUIRE(ir.source == DesignSource::v0);
    REQUIRE(ir.root.provenance.has_value());
    REQUIRE(ir.root.provenance->adapter == "v0-tsx");
    REQUIRE(ir.root.confidence == IRConfidence::pass);
    REQUIRE(ir.root.stable_anchor_id.has_value());
    REQUIRE(ir.root.children[0].stable_anchor_id.has_value());
}

TEST_CASE("parse_v0_tsx regex-fallback path (Tailwind classes) assigns anchors",
          "[view][import][anchors]") {
    // Non-JSON TSX input forces the regex/Tailwind extraction at the
    // tail of parse_v0_tsx. Phase 0a stamps DIVERGE confidence + anchors.
    const std::string tsx =
        "<div className=\"flex-row gap-2\">"
        "<div className=\"flex-col p-2\">child</div>"
        "</div>";
    auto ir = parse_v0_tsx(tsx);

    REQUIRE(ir.source == DesignSource::v0);
    REQUIRE(ir.root.provenance.has_value());
    REQUIRE(ir.root.provenance->adapter == "v0-tsx");
    REQUIRE(ir.root.confidence == IRConfidence::diverge);
    REQUIRE(ir.root.stable_anchor_id.has_value());
    REQUIRE(!ir.root.children.empty());
    REQUIRE(ir.root.children[0].stable_anchor_id.has_value());
}

// ── codegen integration: @pulp-anchor comment ───────────────────────────

TEST_CASE("generate_pulp_js emits @pulp-anchor comments when include_comments",
          "[view][import][anchors]") {
    const std::string json = R"({
        "type": "frame",
        "id": "0:1",
        "children": [{ "type": "text", "content": "X", "id": "0:42" }]
    })";

    auto ir = parse_figma_json(json);

    CodeGenOptions opts;
    opts.include_comments = true;
    opts.mode = CodeGenMode::web_compat;
    auto js = generate_pulp_js(ir, opts);

    REQUIRE(js.find("// @pulp-anchor figma:0:1") != std::string::npos);
    REQUIRE(js.find("// @pulp-anchor figma:0:42") != std::string::npos);
}

TEST_CASE("generate_pulp_js elides @pulp-anchor comments when include_comments=false",
          "[view][import][anchors]") {
    const std::string json = R"({
        "type": "frame",
        "id": "0:1"
    })";

    auto ir = parse_figma_json(json);

    CodeGenOptions opts;
    opts.include_comments = false;
    opts.mode = CodeGenMode::web_compat;
    auto js = generate_pulp_js(ir, opts);

    // Minified codegen keeps the bundle slim; tooling can re-derive the
    // anchor from re-parsing the source, so dropping the comment is safe.
    REQUIRE(js.find("@pulp-anchor") == std::string::npos);
}

TEST_CASE("generate_pulp_js native mode emits @pulp-anchor comments",
          "[view][import][anchors]") {
    const std::string json = R"({
        "type": "frame",
        "id": "0:1"
    })";

    auto ir = parse_figma_json(json);

    CodeGenOptions opts;
    opts.include_comments = true;
    opts.mode = CodeGenMode::native;
    auto js = generate_pulp_js(ir, opts);

    REQUIRE(js.find("// @pulp-anchor figma:0:1") != std::string::npos);
}

// ── Phase 0b: setAnchor() codegen — binds anchor to the live widget ─────

TEST_CASE("web-compat codegen emits setAnchor calls per node",
          "[view][import][anchors][setAnchor]") {
    const std::string json = R"({
        "type": "frame",
        "id": "0:1",
        "children": [{ "type": "text", "content": "X", "id": "0:42" }]
    })";

    auto ir = parse_figma_json(json);

    CodeGenOptions opts;
    opts.include_comments = true;
    opts.mode = CodeGenMode::web_compat;
    auto js = generate_pulp_js(ir, opts);

    // Both anchors should appear in setAnchor() calls — the inspector
    // needs this to map live widgets back to their tweak-layer key.
    REQUIRE(js.find("setAnchor(") != std::string::npos);
    REQUIRE(js.find("'figma:0:1'") != std::string::npos);
    REQUIRE(js.find("'figma:0:42'") != std::string::npos);
}

TEST_CASE("web-compat codegen emits setAnchor even when include_comments=false",
          "[view][import][anchors][setAnchor]") {
    const std::string json = R"({
        "type": "frame",
        "id": "0:1"
    })";

    auto ir = parse_figma_json(json);

    CodeGenOptions opts;
    opts.include_comments = false;
    opts.mode = CodeGenMode::web_compat;
    auto js = generate_pulp_js(ir, opts);

    // include_comments=false strips the // @pulp-anchor trail (cosmetic)
    // but setAnchor() is FUNCTIONAL — the inspector cannot find a
    // widget's anchor without it. Must survive minified codegen.
    REQUIRE(js.find("@pulp-anchor") == std::string::npos);
    REQUIRE(js.find("setAnchor(") != std::string::npos);
    REQUIRE(js.find("'figma:0:1'") != std::string::npos);
}

TEST_CASE("setAnchor escapes single quotes in anchor strings",
          "[view][import][anchors][setAnchor]") {
    // Synthesize an IR where the anchor contains a single-quote (rare
    // but possible if an adapter ever produces one). The codegen helper
    // js_single_quote_escape() must handle it so the emitted JS still
    // parses cleanly.
    pulp::view::DesignIR ir;
    ir.root.type = "frame";
    ir.root.name = "Root";
    ir.root.stable_anchor_id = "weird'anchor";

    CodeGenOptions opts;
    opts.mode = CodeGenMode::web_compat;
    auto js = generate_pulp_js(ir, opts);

    REQUIRE(js.find("setAnchor(") != std::string::npos);
    // The escaped form should be `weird\'anchor`.
    REQUIRE(js.find(R"(weird\'anchor)") != std::string::npos);
}

TEST_CASE("nodes without stable_anchor_id emit no setAnchor call",
          "[view][import][anchors][setAnchor]") {
    pulp::view::DesignIR ir;
    ir.root.type = "frame";
    ir.root.name = "Root";
    // intentionally no stable_anchor_id — like a manually constructed IR

    CodeGenOptions opts;
    opts.mode = CodeGenMode::web_compat;
    auto js = generate_pulp_js(ir, opts);

    REQUIRE(js.find("setAnchor(") == std::string::npos);
}
