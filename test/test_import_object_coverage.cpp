// test_import_object_coverage.cpp — drift guard for the import object-coverage
// matrix (compat.json `imports/object-coverage`).
//
// The matrix is a human-curated claim about which normalized DesignIR object
// types codegen actually lowers to a renderable primitive. A claim rots the
// moment codegen changes underneath it. This test keeps the matrix HONEST:
// for every `types` row it builds a synthetic IRNode of that type, runs the
// real `generate_pulp_js` native lowering, and asserts the observed codegen
// disposition matches the matrix's `codegen` level. It FAILS if:
//
//   * a type marked `codegen: handled` is dropped to an empty frame (the
//     dropped-vector invariant flags it, or its primitive is never emitted), or
//   * a type marked `codegen: missing` (always a vector/path kind here) is NOT
//     flagged by the dropped-vector invariant — i.e. the matrix under-claims a
//     silent drop, or codegen quietly started handling it (update the matrix), or
//   * the matrix lists a `types` row this test has no synthetic builder for, or
//     a builder has no matching matrix row (matrix/test drift).
//
// Source-agnostic by construction: the probes are hand-built IRNodes with no
// provenance / layer names, so this asserts the codegen contract, not any
// source. `features` rows are documented-only (cross-cutting paint/layout
// capabilities owned by separate slices) and are intentionally not probed here.
//
// Tag: [view][import][fidelity][object-coverage]

#include <catch2/catch_test_macros.hpp>

#include <pulp/view/design_ir.hpp>
#include <pulp/view/design_import.hpp>

#include <choc/text/choc_JSON.h>

#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace pulp::view;

#ifndef PULP_SRC_DIR
#error "PULP_SRC_DIR must be defined (path to the repo root holding compat.json)"
#endif

namespace {

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// One synthetic probe per drift-checkable IR type. `render_token` is a JS
// substring that MUST appear when the type lowers to a real primitive; an empty
// token means the type is expected to be DROPPED (the dropped-vector invariant
// must flag it). Root is emitted as a column container (createCol) so each
// probe's render token stays unambiguous against the root's own container call.
struct Probe {
    std::function<IRNode()> build;
    std::string render_token;  // "" => expected dropped (vector kind)
};

IRNode sized(const std::string& type, const std::string& name) {
    IRNode n;
    n.type = type;
    n.name = name;
    n.style.width = 64.0f;   // >= 16x16 so the dropped-vector area gate engages
    n.style.height = 64.0f;
    return n;
}

std::map<std::string, Probe> build_registry() {
    std::map<std::string, Probe> r;

    // Renderable types.
    r["frame"] = {[] {
        IRNode n = sized("frame", "probe_frame");
        n.layout.direction = LayoutDirection::row;  // -> createRow (root is createCol)
        return n;
    }, "createRow"};
    r["text"]  = {[] { IRNode n = sized("text",  "probe_text");  n.text_content = "Hi"; return n; }, "createLabel"};
    r["label"] = {[] { IRNode n = sized("label", "probe_label"); n.text_content = "Hi"; return n; }, "createLabel"};
    r["image"] = {[] { IRNode n = sized("image", "probe_image"); n.attributes["asset_path"] = "/tmp/x.png"; return n; }, "createImage"};

    // Vector/path kinds: bare (no asset / fill / children) -> dropped, flagged
    // by the dropped-vector invariant.
    for (const char* t : {"vector", "path", "svg_path", "rect", "svg_rect",
                          "rectangle", "line", "svg_line", "ellipse", "circle",
                          "polygon", "polyline", "star"}) {
        const std::string type = t;
        r[type] = {[type] { return sized(type, "probe_" + type); }, ""};
    }
    return r;
}

// Run a single probe through the real native codegen and report whether the
// dropped-vector invariant flagged it and the full emitted JS.
struct ProbeResult { bool dropped = false; std::string js; };

ProbeResult run_probe(const IRNode& probe) {
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.name = "probe_root";
    ir.root.layout.direction = LayoutDirection::column;  // root emits createCol
    ir.root.style.width = 400.0f;
    ir.root.style.height = 400.0f;
    ir.root.children.push_back(probe);

    std::vector<FidelityIssue> report;
    CodeGenOptions opts;                 // defaults to bridge_native_js
    opts.fidelity_report = &report;
    ProbeResult out;
    out.js = generate_pulp_js(ir, opts);
    for (const auto& iss : report)
        if (iss.kind == "dropped-vector" && iss.node_name == probe.name)
            out.dropped = true;
    return out;
}

}  // namespace

TEST_CASE("object-coverage matrix matches codegen reality (drift guard)",
          "[view][import][fidelity][object-coverage]") {
    const std::string text = read_file(std::string(PULP_SRC_DIR) + "/compat.json");
    const auto root = choc::json::parse(text);

    REQUIRE(root.isObject());
    const auto imports = root["imports"];
    REQUIRE(imports.isObject());
    const auto oc = imports["object-coverage"];
    REQUIRE(oc.isObject());
    const auto types = oc["types"];
    REQUIRE(types.isObject());
    REQUIRE(types.size() > 0);

    const auto registry = build_registry();

    // Track which builders the matrix exercised, to catch builder-without-row drift.
    std::map<std::string, bool> seen;
    for (const auto& kv : registry) seen[kv.first] = false;

    for (uint32_t i = 0; i < types.size(); ++i) {
        const auto member = types.getObjectMemberAt(i);
        const std::string type = std::string(member.name);
        const auto codegen_v = member.value["codegen"];
        REQUIRE(codegen_v.isString());
        const std::string codegen = std::string(codegen_v.getString());

        INFO("object-coverage type=" << type << " codegen=" << codegen);

        // Every matrix `types` row must have a synthetic builder, or the matrix
        // claims something this drift guard cannot verify.
        const auto it = registry.find(type);
        REQUIRE(it != registry.end());
        seen[type] = true;

        const Probe& probe = it->second;
        const ProbeResult res = run_probe(probe.build());

        if (codegen == "handled") {
            // Must emit its primitive and must NOT be flagged as a silent drop.
            CHECK_FALSE(res.dropped);
            CHECK(res.js.find(probe.render_token) != std::string::npos);
        } else if (codegen == "missing") {
            // Every `missing` type in the matrix is a vector/path kind whose bare
            // form silently degrades — the dropped-vector invariant must catch it.
            CHECK(probe.render_token.empty());
            CHECK(res.dropped);
        } else {
            // "partial" rows are documented but not strictly asserted here.
            CHECK(codegen == "partial");
        }
    }

    // No builder may lack a matrix row (the matrix is the source of truth).
    for (const auto& kv : seen) {
        INFO("builder type without a matrix row: " << kv.first);
        CHECK(kv.second);
    }
}
