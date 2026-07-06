// pulp #1035 — `pulp import-design --from claude` classname extraction.
//
// The CLI extends Spectr's `tools/extract-html-bundle/extract.mjs`
// classname pass into the import pipeline so plugin authors get a
// `classnames.json` artifact alongside `tokens.json` without running a
// separate Node-side script.
//
// Layered coverage so the regression doesn't depend on `pulp-cli`
// being built in every CI lane:
//   - Library-level (`extract_claude_classnames`, `serialize_…`):
//       fixture-driven, parses HTML and asserts the JSON output is
//       structurally equal to the checked-in expected file.
//   - CLI-level (shell-out): runs the built `pulp-cli` against the
//       same fixture and asserts the artifact lands at the requested
//       output path. Skips politely when the binary isn't built so
//       the library tests still gate the feature.
//
// Tests live under [issue-1035] so coverage harness can attribute
// them to the slice that introduced them.

#include <catch2/catch_test_macros.hpp>
#include <pulp/view/design_import.hpp>
#include <pulp/platform/child_process.hpp>
#include <choc/text/choc_JSON.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace pulp::view;
using namespace pulp::platform;
namespace fs = std::filesystem;

namespace {

fs::path repo_root() {
    // Resolve from <repo>/test → <repo>. The fixture path can also be
    // overridden via PULP_REPO_ROOT for adversarial CI layouts (mirrors
    // the convention pulp-test-cli-skew-banner uses).
    if (const char* env = std::getenv("PULP_REPO_ROOT"); env && *env) {
        return fs::path(env);
    }
    return fs::path(__FILE__).parent_path().parent_path();
}

fs::path fixture_dir() {
    return repo_root() / "test" / "fixtures" / "imports" / "claude" / "2024.10";
}

std::string read_text(const fs::path& path) {
    std::ifstream f(path);
    REQUIRE(f.is_open());
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Compare two JSON strings for structural equality. choc parses both,
// then we walk the trees member-by-member. Avoids brittle byte-equality
// over whitespace/indentation choices the serializer makes.
bool json_structurally_equal(const std::string& a, const std::string& b) {
    auto va = choc::json::parse(a);
    auto vb = choc::json::parse(b);
    return choc::json::toString(va, false) == choc::json::toString(vb, false);
}

// Locate a runnable CLI that implements `import-design`. The C++ delegate
// target `pulp-cli` emits `pulp-cpp` (CMake OUTPUT_NAME) in <build>/tools/cli;
// the Rust front-end lands at <build>/pulp and forwards import-design to that
// delegate. The test runner's working directory at invocation is <build>/test.
// Prefer an explicit override, then the C++ delegate (the real implementation
// of this command), then the legacy/Rust names. Returning a non-existent path
// makes binary_exists() false so the case skips when nothing is built.
fs::path pulp_binary() {
    if (const char* env = std::getenv("PULP_CLI_PATH"); env && *env) {
        return fs::path(env);
    }
    const auto build = fs::current_path() / "..";
    for (const auto& candidate : {
             build / "tools" / "cli" / "pulp-cpp",  // C++ delegate (current name)
             build / "tools" / "cli" / "pulp",      // legacy name
             build / "pulp",                         // Rust front-end
         }) {
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec)) return candidate;
    }
    return build / "tools" / "cli" / "pulp-cpp";
}

bool binary_exists() { return fs::exists(pulp_binary()); }

ProcessResult run_pulp(const std::vector<std::string>& args, int timeout_ms = 30000) {
    auto bin = pulp_binary();
    if (!fs::exists(bin)) {
        ProcessResult r;
        r.exit_code = -1;
        r.stderr_output = "pulp binary not at " + bin.string();
        return r;
    }
    return exec(bin.string(), args, timeout_ms);
}

fs::path unique_temp_dir(const std::string& prefix) {
    auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    auto dir = fs::temp_directory_path() / (prefix + "-" + std::to_string(tick));
    fs::create_directories(dir);
    return dir;
}

} // namespace

// ── Library-level: fixture-driven extraction ────────────────────────────

TEST_CASE("extract_claude_classnames matches the expected fixture",
          "[cli][import-design][issue-1035]") {
    const auto html = read_text(fixture_dir() / "example.html");
    const auto expected = read_text(fixture_dir() / "expected-classnames.json");

    const auto rules = extract_claude_classnames(html);
    const auto actual = serialize_claude_classnames(rules);

    INFO("expected:\n" << expected);
    INFO("actual:\n" << actual);
    REQUIRE(json_structurally_equal(actual, expected));
}

// pulp #1035 — direct unit coverage of the multi-class /
// hyphen-classname / cascade behaviour the CLI relies on.
TEST_CASE("extract_claude_classnames parses the issue body's example", "[cli][import-design][issue-1035]") {
    const std::string html = R"HTML(
        <html><head><style>
        .mono { font-family: ui-monospace, SFMono-Regular, monospace; }
        .tnum { font-variant-numeric: tabular-nums; }
        </style></head><body><div class="mono tnum"></div></body></html>
    )HTML";

    const auto rules = extract_claude_classnames(html);

    REQUIRE(rules.size() == 2);
    REQUIRE(rules.at("mono").at("fontFamily") == "ui-monospace, SFMono-Regular, monospace");
    REQUIRE(rules.at("tnum").at("fontVariantNumeric") == "tabular-nums");
}

TEST_CASE("extract_claude_classnames merges across multiple <style> blocks", "[cli][import-design][issue-1035]") {
    // Earlier blocks set the floor; later blocks override per-property.
    // Unrelated declarations from the earlier block must be preserved.
    const std::string html = R"HTML(
        <style>.card { padding: 12px; color: red; }</style>
        <style>.card { padding: 16px 20px; }</style>
    )HTML";

    const auto rules = extract_claude_classnames(html);

    REQUIRE(rules.size() == 1);
    REQUIRE(rules.at("card").at("padding") == "16px 20px");
    REQUIRE(rules.at("card").at("color") == "red");
}

TEST_CASE("extract_claude_classnames skips pseudo-classes and at-rule wrappers",
          "[cli][import-design][issue-1035]") {
    const std::string html = R"HTML(
        <style>
        .ok { color: green; }
        .skip:hover { color: red; }
        .skip[data-x] { color: red; }
        .skip > .child { color: red; }
        @media (max-width: 600px) { .responsive { display: none; } }
        @keyframes pulse { from { opacity: 0; } to { opacity: 1; } }
        </style>
    )HTML";

    const auto rules = extract_claude_classnames(html);

    // `.ok` is the only plain-classname rule outside of `@media` /
    // `@keyframes`. The pseudo-class, attribute selector, and
    // descendant combinator all disqualify their `.skip` rules.
    REQUIRE(rules.size() == 1);
    REQUIRE(rules.count("ok") == 1);
    REQUIRE(rules.count("skip") == 0);
    REQUIRE(rules.count("responsive") == 0);
}

TEST_CASE("extract_claude_classnames skips .scheme-* and empty bodies",
          "[cli][import-design][issue-1035]") {
    const std::string html = R"HTML(
        <style>
        .scheme-paper { --bg: #fff; }
        .ghost {}
        .real { color: blue; }
        </style>
    )HTML";

    const auto rules = extract_claude_classnames(html);

    REQUIRE(rules.size() == 1);
    REQUIRE(rules.count("real") == 1);
    REQUIRE(rules.count("scheme-paper") == 0);
    REQUIRE(rules.count("ghost") == 0);
}

TEST_CASE("extract_claude_classnames returns empty for HTML with no style blocks",
          "[cli][import-design][issue-1035]") {
    const std::string html = "<!DOCTYPE html><html><body><p>hi</p></body></html>";
    const auto rules = extract_claude_classnames(html);
    REQUIRE(rules.empty());

    // Serializer must still produce valid (empty-object) JSON so the
    // artifact is consumable by downstream tools without a missing-file
    // branch.
    const auto json = serialize_claude_classnames(rules);
    auto parsed = choc::json::parse(json);
    REQUIRE(parsed.isObject());
    REQUIRE(parsed.size() == 0);
}

TEST_CASE("extract_claude_classnames handles comma-separated selector lists",
          "[cli][import-design][issue-1035]") {
    const std::string html =
        "<style>.btn-primary, .btn-secondary { cursor: pointer; }</style>";

    const auto rules = extract_claude_classnames(html);

    REQUIRE(rules.size() == 2);
    REQUIRE(rules.at("btn-primary").at("cursor") == "pointer");
    REQUIRE(rules.at("btn-secondary").at("cursor") == "pointer");
}

// ── CLI-level: shell-out against the fixture ────────────────────────────

TEST_CASE("pulp import-design --from claude emits classnames.json by default",
          "[cli][import-design][issue-1035][shellout]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto tmp = unique_temp_dir("pulp-import-design-classnames");
    auto html_in = fixture_dir() / "example.html";
    auto js_out = tmp / "ui.js";
    auto tokens_out = tmp / "tokens.json";
    auto bridge_out = tmp / "bridge.cpp";
    auto classnames_out = tmp / "classnames.json";

    auto r = run_pulp({"import-design",
                       "--from", "claude",
                       "--file", html_in.string(),
                       "--output", js_out.string(),
                       "--tokens", tokens_out.string(),
                       "--bridge-output", bridge_out.string(),
                       "--classnames", classnames_out.string()});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    REQUIRE(fs::exists(classnames_out));

    const auto actual = read_text(classnames_out);
    const auto expected = read_text(fixture_dir() / "expected-classnames.json");
    INFO("expected:\n" << expected);
    INFO("actual:\n" << actual);
    REQUIRE(json_structurally_equal(actual, expected));

    // Stdout reports the artifact so users notice when it lands.
    const auto combined = r.stdout_output + r.stderr_output;
    REQUIRE(combined.find(classnames_out.string()) != std::string::npos);
}

TEST_CASE("pulp import-design --dry-run --strict-fidelity exits 0 on a clean import",
          "[cli][import-design][fidelity][shellout]") {
    // #3267: the dry-run branch must honor --strict-fidelity (return
    // fidelity_failed ? 4 : 0), not an unconditional 0. A clean import has no
    // hard findings, so a harness running --dry-run --strict-fidelity sees 0 —
    // and the new return path is exercised. (The failure→4 path needs real
    // skewed PNG assets; the geometry logic is unit-covered in
    // test_design_fidelity.cpp / test_design_import.cpp.)
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto tmp = unique_temp_dir("pulp-import-design-dryrun-strict");
    auto html_in = fixture_dir() / "example.html";
    auto js_out = tmp / "ui.js";

    auto r = run_pulp({"import-design",
                       "--from", "claude",
                       "--file", html_in.string(),
                       "--output", js_out.string(),
                       "--dry-run",
                       "--strict-fidelity"});
    REQUIRE_FALSE(r.timed_out);
    CHECK(r.exit_code == 0);
    // --dry-run is a preview: it must not write the output file.
    CHECK_FALSE(fs::exists(js_out));
}

TEST_CASE("pulp import-design --from claude --no-emit-classnames suppresses the artifact",
          "[cli][import-design][issue-1035][shellout]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto tmp = unique_temp_dir("pulp-import-design-classnames-off");
    auto html_in = fixture_dir() / "example.html";
    auto js_out = tmp / "ui.js";
    auto classnames_out = tmp / "classnames.json";

    auto r = run_pulp({"import-design",
                       "--from", "claude",
                       "--file", html_in.string(),
                       "--output", js_out.string(),
                       "--no-bridge-scaffold",
                       "--classnames", classnames_out.string(),
                       "--no-emit-classnames"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    REQUIRE_FALSE(fs::exists(classnames_out));
}


// ── pulp friction-fix #3 — native-react detection ────────────────────
// The Claude-Design static-HTML parser sees only literal DOM. When
// the input is a bundler entry (mount-point + script tag, or pulp's
// own `__bundler_*` shell), the parser returns the placeholder chrome
// only. `looks_like_bundler_entry` triggers a soft warning telling
// the user to run the bundle through pulp-design-tool instead.

TEST_CASE("looks_like_bundler_entry matches mount + script", "[cli][import-design][friction-3]") {
    const std::string html = R"HTML(
        <html><body><div id="root"></div>
        <script src="bundle.js"></script></body></html>
    )HTML";
    REQUIRE(looks_like_bundler_entry(html));
}

TEST_CASE("looks_like_bundler_entry matches @pulp/react bundle shell", "[cli][import-design][friction-3]") {
    // Mirrors the structure of Spectr's editor.html.
    const std::string html = R"HTML(
        <html><body>
          <div id="__bundler_thumbnail"></div>
          <div id="__bundler_loading">Unpacking...</div>
        </body></html>
    )HTML";
    REQUIRE(looks_like_bundler_entry(html));
}

TEST_CASE("looks_like_bundler_entry skips hand-authored Claude Design HTML",
          "[cli][import-design][friction-3]") {
    const std::string html = R"HTML(
        <html><body>
          <h1>Sliders</h1>
          <input type="range" min="0" max="100" />
          <input type="range" min="0" max="100" />
          <style>.mono { font-family: monospace; }</style>
        </body></html>
    )HTML";
    REQUIRE_FALSE(looks_like_bundler_entry(html));
}

TEST_CASE("looks_like_bundler_entry on empty / pathological input",
          "[cli][import-design][friction-3]") {
    REQUIRE_FALSE(looks_like_bundler_entry(""));
    REQUIRE_FALSE(looks_like_bundler_entry("<!doctype html><html></html>"));
}

TEST_CASE("pulp import-design --from claude emits native-react hint on bundler entry",
          "[cli][import-design][friction-3][shellout]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto tmp = unique_temp_dir("pulp-import-design-friction3");
    // Tiny bundler entry — should produce few elements + the hint.
    auto html_in = tmp / "shell.html";
    {
        std::ofstream f(html_in);
        f << R"HTML(<html><head><title>Spectr</title></head><body>
          <div id="__bundler_loading">Unpacking...</div>
          <div id="__bundler_thumbnail"></div>
          <script type="module" src="./bundle.js"></script>
          </body></html>)HTML";
    }
    auto js_out = tmp / "ui.js";
    auto r = run_pulp({"import-design",
                       "--from", "claude",
                       "--file", html_in.string(),
                       "--output", js_out.string(),
                       "--no-bridge-scaffold",
                       "--no-emit-classnames"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    // ui.js still emitted (warning is soft).
    REQUIRE(fs::exists(js_out));
    // Warning fires on stderr.
    INFO("stderr: " << r.stderr_output);
    REQUIRE(r.stderr_output.find("looks like a JS-bundler entry") != std::string::npos);
    REQUIRE(r.stderr_output.find("pulp-design-tool --script") != std::string::npos);
}

// ── pulp friction-fix #4 — sidecar files follow --output ──────────────
// `--output <dir>/ui.js` should anchor bridge_handlers.cpp,
// classnames.json, and tokens.json to the same directory unless the
// user explicitly set each path.

TEST_CASE("pulp import-design --output anchors sidecar files to output dir",
          "[cli][import-design][friction-4][shellout]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto tmp = unique_temp_dir("pulp-import-design-friction4");
    auto html_in = fixture_dir() / "example.html";
    auto js_out = tmp / "ui.js";

    auto r = run_pulp({"import-design",
                       "--from", "claude",
                       "--file", html_in.string(),
                       "--output", js_out.string()});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    REQUIRE(fs::exists(js_out));
    REQUIRE(fs::exists(tmp / "bridge_handlers.cpp"));
    REQUIRE(fs::exists(tmp / "classnames.json"));
}

TEST_CASE("pulp import-design respects explicit sidecar paths over --output anchor",
          "[cli][import-design][friction-4][shellout]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto tmp = unique_temp_dir("pulp-import-design-friction4-explicit");
    auto sidecar_dir = tmp / "sidecars";
    fs::create_directories(sidecar_dir);
    auto html_in = fixture_dir() / "example.html";
    auto js_out = tmp / "ui.js";
    auto bridge_out = sidecar_dir / "my_handlers.cpp";
    auto classnames_out = sidecar_dir / "my_classnames.json";

    auto r = run_pulp({"import-design",
                       "--from", "claude",
                       "--file", html_in.string(),
                       "--output", js_out.string(),
                       "--bridge-output", bridge_out.string(),
                       "--classnames", classnames_out.string()});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    // Explicit paths win — sidecars land in sidecar_dir, not tmp.
    REQUIRE(fs::exists(bridge_out));
    REQUIRE(fs::exists(classnames_out));
    REQUIRE_FALSE(fs::exists(tmp / "bridge_handlers.cpp"));
    REQUIRE_FALSE(fs::exists(tmp / "classnames.json"));
}

// Package.json overwrite and vendor-source hard-fail coverage from #1060 is
// deferred until #1180 gives shellout tests env-aware ProcessResult support.
// The production fix remains in tools/import-design/pulp_import_design.cpp;
// the earlier vendor test used POSIX-only setenv/unsetenv and broke MSVC.

// ── pulp #41 follow-up: --from figma auto-routes a figma-plugin envelope ──
// A Figma-plugin export passed to `--from figma` used to be fed to
// parse_figma_json (the old Figma format), which read none of its structure
// and silently emitted an empty root-only import. The CLI now detects the
// envelope (looks_like_figma_plugin_export) and routes to the plugin parser
// with a stderr note. This guards against the silent-empty footgun.
TEST_CASE("pulp import-design --from figma auto-routes a figma-plugin envelope",
          "[cli][import-design][issue-41][shellout]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto tmp = unique_temp_dir("pulp-import-source-routing");
    auto scene = tmp / "scene.pulp.json";
    {
        std::ofstream f(scene);
        f << R"({
  "format_version": "2026.05-figma-plugin-v1",
  "provenance": {"adapter": "figma-plugin", "version": "t",
                 "source_uri": "figma://x/1:1"},
  "root": {"type": "frame", "name": "Root", "figma_node_id": "1:1",
           "children": [{"type": "text", "name": "Hello", "figma_node_id": "1:2",
                         "content": "Hello world"}]}
})";
    }
    auto js_out = tmp / "ui.js";
    // Deliberately the WRONG flag (--from figma) to exercise the guardrail.
    auto r = run_pulp({"import-design", "--from", "figma",
                       "--file", scene.string(), "--output", js_out.string(),
                       "--no-tokens"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    // Guardrail fired: routed to the plugin parser with a notice. (The note
    // names the format with a capital F: "Figma-plugin export envelope".)
    const auto combined = r.stdout_output + r.stderr_output;
    REQUIRE(combined.find("Figma-plugin export envelope") != std::string::npos);
    // Children were actually parsed (NOT the empty root-only output): the
    // text node's content reaches the generated JS.
    REQUIRE(fs::exists(js_out));
    REQUIRE(read_text(js_out).find("Hello world") != std::string::npos);
}

// ── Interactive sprite knobs (task #22) ──────────────────────────────────
// A recognized knob whose body art lives in a CHILD image (the ELYSIUM shape:
// captured disc + a separate pointer) used to be DEMOTED to a plain image in
// sprite mode — pixel-faithful but dead (it never turned). The importer now
// HOISTS the body art onto the knob node so it stays a live Knob skinned with
// a single-frame strip, and the recovered opaque core is passed through so the
// engine fits the disc to the box and sweeps the native indicator within it.
// Synthetic envelope + a synthetic knob PNG (no proprietary export).
namespace {
fs::path write_sprite_knob_fixture(const fs::path& tmp) {
    fs::create_directories(tmp / "assets");
    const auto png_src = repo_root() / "test" / "fixtures" / "imports" /
                         "figma-plugin" / "synthetic-knob.png";
    fs::copy_file(png_src, tmp / "assets" / "synthetic-knob.png",
                  fs::copy_options::overwrite_existing);
    auto scene = tmp / "scene.pulp.json";
    std::ofstream f(scene);
    // A knob FRAME (explicit audio_widget) whose body art is a child image
    // carrying an asset_ref + renderBounds — the hoist's target shape.
    f << R"({
  "format_version": "2026.05-figma-plugin-v1",
  "provenance": {"adapter": "figma-plugin", "version": "t",
                 "source_uri": "figma://x/1:1"},
  "asset_manifest": {"version": 1, "assets": [
    {"asset_id": "knob_body", "local_path": "assets/synthetic-knob.png",
     "mime": "image/png"}]},
  "root": {"type": "frame", "name": "Root", "figma_node_id": "1:1",
    "children": [
      {"type": "frame", "name": "GainKnob", "audio_widget": "knob",
       "figma_node_id": "1:2", "style": {"width": 56, "height": 56},
       "children": [
         {"type": "image", "name": "body", "figma_node_id": "1:3",
          "asset_ref": "knob_body",
          "style": {"width": 56, "height": 56,
                    "renderBounds": {"w": 64, "h": 96, "dx": -4, "dy": -4}}}
       ]}
    ]}
})";
    return scene;
}
} // namespace

TEST_CASE("pulp import-design --knob-style sprite keeps a child-art knob interactive",
          "[cli][import-design][sprite][shellout]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto tmp = unique_temp_dir("pulp-sprite-knob-hoist");
    auto scene = write_sprite_knob_fixture(tmp);
    auto js_out = tmp / "ui.js";
    auto r = run_pulp({"import-design", "--from", "figma-plugin",
                       "--file", scene.string(), "--output", js_out.string(),
                       "--no-tokens", "--knob-style", "sprite"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    REQUIRE(fs::exists(js_out));
    const auto js = read_text(js_out);

    // Stays a live Knob skinned with a single-frame body strip — NOT demoted.
    REQUIRE(js.find("createKnob('GainKnob") != std::string::npos);
    REQUIRE(js.find("setKnobSpriteStrip('GainKnob") != std::string::npos);
    REQUIRE(js.find(", 1, 'vertical')") != std::string::npos);
    // Opaque core recovered end-to-end from the synthetic PNG (disc bbox is
    // 100×100 within the 128×192 image) → disc fits the box, indicator sweeps.
    REQUIRE(js.find("setKnobSpriteCore('GainKnob") != std::string::npos);
    REQUIRE(js.find(", 100, 100)") != std::string::npos);
    // Sprite mode does NOT apply the silver vector style.
    REQUIRE(js.find("setWidgetStyle('GainKnob") == std::string::npos);
}

// ── baked ir-json lane preserves the figma-plugin tree + assets ──────────
// Two bugs used to silently gut the `--emit ir-json` (and cpp/swiftui) lane for
// figma-plugin envelopes while `--emit js` worked:
//   1) looks_like_serialized_design_ir() false-matched the envelope (it has a
//      top-level "root" and nested "version" keys), short-circuiting to the
//      bare-node DesignIR parser, which dropped every child.
//   2) refresh_design_ir_asset_manifest() rebuilt the manifest from a node-URI
//      scan that does not recognize figma-plugin `asset_ref` attributes, so the
//      parsed asset_manifest was discarded.
// This guards the baked lane: children survive AND the parsed manifest is kept.
TEST_CASE("pulp import-design --emit ir-json keeps a figma-plugin tree + assets",
          "[cli][import-design][ir-json][figma-plugin][shellout]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto tmp = unique_temp_dir("pulp-irjson-figma-plugin");
    auto scene = write_sprite_knob_fixture(tmp);  // 1 asset (knob_body) + nested GainKnob
    auto ir_out = tmp / "design.ir.json";
    auto r = run_pulp({"import-design", "--from", "figma-plugin",
                       "--file", scene.string(), "--emit", "ir-json",
                       "--mode", "baked", "--output", ir_out.string(),
                       "--no-tokens"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    REQUIRE(fs::exists(ir_out));
    const auto ir = read_text(ir_out);

    // Bug 1 guard: NOT the empty bare-node fallback, and the child survived.
    REQUIRE(ir.find("parsed legacy bare-node DesignIR JSON") == std::string::npos);
    REQUIRE(ir.find("GainKnob") != std::string::npos);
    // Bug 2 guard: the parsed asset_manifest is preserved (not scanned away).
    REQUIRE(ir.find("\"assetManifest\"") != std::string::npos);
    REQUIRE(ir.find("knob_body") != std::string::npos);
}

TEST_CASE("pulp import-design normalizes + emits a Figma blend mode (end-to-end)",
          "[cli][import-design][blend][shellout]") {
    // The figma-plugin export carries the blend mode in the `figma` block in
    // UPPER_SNAKE. parse_ir_node normalizes it to the CSS keyword and codegen
    // emits setMixBlendMode — except PASS_THROUGH (= normal), which is dropped.
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto tmp = unique_temp_dir("pulp-blend-mode");
    {
        std::ofstream f(tmp / "scene.pulp.json");
        f << R"({
  "format_version": "2026.05-figma-plugin-v1",
  "provenance": {"adapter": "figma-plugin", "version": "t",
                 "source_uri": "figma://x/1:1"},
  "root": {"type": "frame", "name": "Root", "figma_node_id": "1:1",
    "children": [
      {"type": "frame", "name": "Glow", "figma_node_id": "1:2",
       "style": {"width": 80, "height": 80, "backgroundColor": "#ff8800"},
       "figma": {"blend_mode": "COLOR_DODGE"}},
      {"type": "frame", "name": "Plain", "figma_node_id": "1:3",
       "style": {"width": 80, "height": 80, "backgroundColor": "#223344"},
       "figma": {"blend_mode": "PASS_THROUGH"}}
    ]}
})";
    }
    auto js_out = tmp / "ui.js";
    auto r = run_pulp({"import-design", "--from", "figma-plugin",
                       "--file", (tmp / "scene.pulp.json").string(),
                       "--output", js_out.string(), "--no-tokens"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    const auto js = read_text(js_out);
    // COLOR_DODGE → normalized to the CSS keyword and emitted on the Glow node.
    REQUIRE(js.find("setMixBlendMode('Glow") != std::string::npos);
    REQUIRE(js.find("'color-dodge')") != std::string::npos);
    // PASS_THROUGH is a no-op → no blend call for the Plain node.
    REQUIRE(js.find("setMixBlendMode('Plain") == std::string::npos);
}

TEST_CASE("pulp import-design lowers an SVG path node to a native SvgPath (end-to-end)",
          "[cli][import-design][vector][shellout]") {
    // Full pipeline: the figma-plugin parser routes a `path` node carrying `d`
    // through parse_ir_node (which preserves it as path_data) and codegen emits
    // createSvgPath + setSvgPath — instead of silently dropping the vector.
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto tmp = unique_temp_dir("pulp-vector-path");
    {
        std::ofstream f(tmp / "scene.pulp.json");
        f << R"({
  "format_version": "2026.05-figma-plugin-v1",
  "provenance": {"adapter": "figma-plugin", "version": "t",
                 "source_uri": "figma://x/1:1"},
  "root": {"type": "frame", "name": "Root", "figma_node_id": "1:1",
    "children": [
      {"type": "path", "name": "Glyph", "figma_node_id": "1:2",
       "d": "M0 0 L64 0 L32 64 Z", "fill": "#ff8800",
       "viewBox": "0 0 64 64",
       "style": {"width": 64, "height": 64}}
    ]}
})";
    }
    auto js_out = tmp / "ui.js";
    auto r = run_pulp({"import-design", "--from", "figma-plugin",
                       "--file", (tmp / "scene.pulp.json").string(),
                       "--output", js_out.string(), "--no-tokens"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    const auto js = read_text(js_out);
    REQUIRE(js.find("createSvgPath('Glyph") != std::string::npos);
    REQUIRE(js.find("setSvgPath('Glyph") != std::string::npos);
    REQUIRE(js.find("M0 0 L64 0 L32 64 Z") != std::string::npos);
}

TEST_CASE("pulp import-design --knob-style sprite keeps multi-layer knob art (no silent drop)",
          "[cli][import-design][sprite][shellout]") {
    // Hoist regression: a knob exported as MULTIPLE asset-backed image
    // layers (body + logo/highlight) can't be a single-frame sprite skin, and
    // the leaf knob codegen would silently drop every layer after the first.
    // So when there is >1 asset-image child the importer DEMOTES to a plain
    // container and every layer renders as an image (faithful, not turnable).
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto tmp = unique_temp_dir("pulp-sprite-knob-multilayer");
    fs::create_directories(tmp / "assets");
    const auto png = repo_root() / "test" / "fixtures" / "imports" /
                     "figma-plugin" / "synthetic-knob.png";
    fs::copy_file(png, tmp / "assets" / "a.png", fs::copy_options::overwrite_existing);
    fs::copy_file(png, tmp / "assets" / "b.png", fs::copy_options::overwrite_existing);
    {
        std::ofstream f(tmp / "scene.pulp.json");
        f << R"({
  "format_version": "2026.05-figma-plugin-v1",
  "provenance": {"adapter": "figma-plugin", "version": "t",
                 "source_uri": "figma://x/1:1"},
  "asset_manifest": {"version": 1, "assets": [
    {"asset_id": "layerA", "local_path": "assets/a.png", "mime": "image/png"},
    {"asset_id": "layerB", "local_path": "assets/b.png", "mime": "image/png"}]},
  "root": {"type": "frame", "name": "Root", "figma_node_id": "1:1",
    "children": [
      {"type": "frame", "name": "MultiKnob", "audio_widget": "knob",
       "figma_node_id": "1:2", "style": {"width": 56, "height": 56},
       "children": [
         {"type": "image", "name": "body", "figma_node_id": "1:3",
          "asset_ref": "layerA",
          "style": {"width": 56, "height": 56,
                    "renderBounds": {"w": 64, "h": 96, "dx": -4, "dy": -4}}},
         {"type": "image", "name": "logo", "figma_node_id": "1:4",
          "asset_ref": "layerB", "style": {"width": 24, "height": 24}}
       ]}
    ]}
})";
    }
    auto js_out = tmp / "ui.js";
    auto r = run_pulp({"import-design", "--from", "figma-plugin",
                       "--file", (tmp / "scene.pulp.json").string(),
                       "--output", js_out.string(), "--no-tokens",
                       "--knob-style", "sprite"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    const auto js = read_text(js_out);
    // Demoted to a container: NOT a sprite knob, and BOTH layers survive as
    // images (the pre-task behavior — no silent loss of the second layer).
    REQUIRE(js.find("setKnobSpriteStrip") == std::string::npos);
    REQUIRE(js.find("setKnobSpriteCore") == std::string::npos);
    const auto count = [&](const std::string& needle) {
        size_t n = 0, p = 0;
        while ((p = js.find(needle, p)) != std::string::npos) { ++n; p += needle.size(); }
        return n;
    };
    REQUIRE(count("setImageSource(") == 2);
}

TEST_CASE("pulp import-design default (silver) knob keeps the native vector body",
          "[cli][import-design][sprite][shellout]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto tmp = unique_temp_dir("pulp-sprite-knob-silver");
    auto scene = write_sprite_knob_fixture(tmp);
    auto js_out = tmp / "ui.js";
    // No --knob-style → silver is the figma-plugin default.
    auto r = run_pulp({"import-design", "--from", "figma-plugin",
                       "--file", scene.string(), "--output", js_out.string(),
                       "--no-tokens"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    const auto js = read_text(js_out);

    REQUIRE(js.find("createKnob('GainKnob") != std::string::npos);
    REQUIRE(js.find("setWidgetStyle('GainKnob") != std::string::npos);  // silver
    REQUIRE(js.find("setKnobSpriteStrip('GainKnob") == std::string::npos);
    REQUIRE(js.find("setKnobSpriteCore('GainKnob") == std::string::npos);
}

// ── `pulp design compile` — the design contract CLI ─────────────────────────

TEST_CASE("pulp design compile --stdout --json emits the manifest", "[cli][design-compile][shellout]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }
    auto r = run_pulp({"design", "compile", "--stdout", "--json"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    REQUIRE(r.stdout_output.find("\"manifest_version\"") != std::string::npos);
    REQUIRE(r.stdout_output.find("ink-signal") != std::string::npos);
    REQUIRE(r.stdout_output.find("\"tokens\"") != std::string::npos);
    REQUIRE(r.stdout_output.find("\"components\"") != std::string::npos);
    REQUIRE(r.stdout_output.find("\"reskin_tokens\"") != std::string::npos);
}

TEST_CASE("pulp design compile --stdout --prompt emits the binding contract", "[cli][design-compile][shellout]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }
    auto r = run_pulp({"design", "compile", "--stdout", "--prompt"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    REQUIRE(r.stdout_output.find("# Design binding contract") != std::string::npos);
    REQUIRE(r.stdout_output.find("## Allowed tokens") != std::string::npos);
    REQUIRE(r.stdout_output.find("## Allowed components") != std::string::npos);
}

TEST_CASE("pulp design compile writes both artifacts to --out-dir", "[cli][design-compile][shellout]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }
    auto tmp = unique_temp_dir("pulp-design-compile");
    auto r = run_pulp({"design", "compile", "-o", tmp.string()});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    auto manifest = tmp / "design-manifest.json";
    auto prompt = tmp / "design-binding-prompt.md";
    REQUIRE(fs::exists(manifest));
    REQUIRE(fs::exists(prompt));
    const auto json = read_text(manifest);
    REQUIRE(json.find("2026.07-design-manifest-v1") != std::string::npos);
    REQUIRE(read_text(prompt).find("# Design binding contract") != std::string::npos);
    fs::remove_all(tmp);
}

TEST_CASE("pulp design compile rejects mutually exclusive sources", "[cli][design-compile][shellout]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }
    auto r = run_pulp({"design", "compile", "--design-md", "a.md", "--theme", "b.json"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 2);
    REQUIRE(r.stderr_output.find("mutually exclusive") != std::string::npos);
}

TEST_CASE("pulp design compile rejects --json with --prompt", "[cli][design-compile][shellout]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }
    auto r = run_pulp({"design", "compile", "--json", "--prompt", "--stdout"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 2);
    REQUIRE(r.stderr_output.find("mutually exclusive") != std::string::npos);
}

TEST_CASE("pulp design compile rejects an unknown flag", "[cli][design-compile][shellout]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }
    auto r = run_pulp({"design", "compile", "--nope"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 2);
}

TEST_CASE("pulp design compile --help prints usage", "[cli][design-compile][shellout]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }
    auto r = run_pulp({"design", "compile", "--help"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    REQUIRE(r.stdout_output.find("Usage: pulp design compile") != std::string::npos);
}

TEST_CASE("pulp design compile --design-md compiles a project's tokens", "[cli][design-compile][shellout]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }
    auto md = repo_root() / "test" / "fixtures" / "imports" / "designmd" / "alpha" / "DESIGN.md";
    if (!fs::exists(md)) { SUCCEED("skipped: DESIGN.md fixture absent"); return; }
    auto r = run_pulp({"design", "compile", "--design-md", md.string(), "--stdout", "--json"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    // Provenance records the source file, and its tokens are compiled in.
    REQUIRE(r.stdout_output.find("DESIGN.md") != std::string::npos);
    REQUIRE(r.stdout_output.find("\"kind\": \"color\"") != std::string::npos);
}

TEST_CASE("pulp design compile --theme on an unreadable file fails cleanly", "[cli][design-compile][shellout]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }
    auto missing = unique_temp_dir("pulp-design-theme") / "nope.json";
    auto r = run_pulp({"design", "compile", "--theme", missing.string(), "--stdout"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 1);
    REQUIRE(r.stderr_output.find("no tokens read") != std::string::npos);
}

TEST_CASE("pulp design compile --out-dir requires a value", "[cli][design-compile][shellout]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }
    auto r = run_pulp({"design", "compile", "--out-dir"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 2);
    REQUIRE(r.stderr_output.find("requires a value") != std::string::npos);
}

TEST_CASE("pulp design compile --stdout with no filter prints both artifacts", "[cli][design-compile][shellout]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }
    auto r = run_pulp({"design", "compile", "--stdout"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    REQUIRE(r.stdout_output.find("\"manifest_version\"") != std::string::npos);  // json
    REQUIRE(r.stdout_output.find("# Design binding contract") != std::string::npos);  // prompt
}
