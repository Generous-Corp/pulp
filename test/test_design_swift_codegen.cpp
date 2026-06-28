// Workstream B1 — baked SwiftUI emitter (generate_pulp_swift).
//
// Two layers, per "tests ship with fixes":
//   - Golden-string Catch2 assertions over the emitted SwiftUI view, the
//     code-first PulpTheme partition, and the minimal binding manifest.
//   - A swiftc compile gate: B1 emits Swift source, so golden C++-string
//     asserts alone could ship non-compiling Swift. The gate emits the real
//     PulpSwift module from apple/Sources/PulpSwift and type-checks the
//     generated view + theme against it. It SKIPS when the Swift toolchain /
//     SwiftUI SDK is unavailable (e.g. the Linux lane) and only hard-fails
//     when the baseline module emits but the generated code does not.

#include <catch2/catch_test_macros.hpp>
#include <pulp/view/design_codegen.hpp>
#include <pulp/view/design_ir.hpp>
#include <pulp/platform/child_process.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace pulp::view;
namespace fs = std::filesystem;

#ifndef PULP_TEST_SWIFTC
#define PULP_TEST_SWIFTC ""
#endif
#ifndef PULP_REPO_ROOT
#define PULP_REPO_ROOT ""
#endif

namespace {

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

IRNode frame_node(std::string id, std::string name, float w, float h,
                  LayoutDirection dir) {
    IRNode node;
    node.type = "frame";
    node.name = std::move(name);
    node.stable_anchor_id = id;
    node.style.width = w;
    node.style.height = h;
    node.layout.direction = dir;
    return node;
}

IRNode text_node(std::string name, std::string text, float font_size,
                 std::string color) {
    IRNode node;
    node.type = "text";
    node.name = std::move(name);
    node.text_content = std::move(text);
    node.style.font_size = font_size;
    node.style.color = std::move(color);
    return node;
}

// A small but representative panel: a header label, a bound knob/fader/toggle,
// and base + .dark tokens.
DesignIR build_swift_fixture() {
    DesignIR ir;
    ir.source = DesignSource::figma;

    ir.tokens.colors["color.bg"] = "#ffffff";
    ir.tokens.colors["color.bg.dark"] = "#000000";   // dynamic light/dark pair
    ir.tokens.colors["accent"] = "#ff8800";           // base-only
    ir.tokens.dimensions["spacing.sm"] = 4.0f;
    ir.tokens.strings["font.body"] = "Inter";
    // Adversarial token names: a Swift keyword must be
    // backtick-escaped, and two names that camel-case to the same identifier
    // must be de-duplicated — otherwise the generated PulpTheme.swift fails to
    // compile. The swiftc gate below proves the escaping/dedup works.
    ir.tokens.colors["default"] = "#123456";          // Swift keyword → `default`
    ir.tokens.colors["foo.bar"] = "#111111";          // camels to fooBar
    ir.tokens.colors["foo-bar"] = "#222222";          // also fooBar → must dedup
    // Keyword dimension/string tokens WITH .dark overrides: the dark companion
    // must escape the full id ("switchDark"), not append Dark to the escaped
    // base (the invalid `` `switch`Dark ``).
    ir.tokens.dimensions["switch"] = 2.0f;            // keyword base → `switch`
    ir.tokens.dimensions["switch.dark"] = 3.0f;       // companion → switchDark
    ir.tokens.strings["class"] = "regular";           // keyword base → `class`
    ir.tokens.strings["class.dark"] = "bold";         // companion → classDark

    ir.root = frame_node("root", "Panel", 320.0f, 200.0f, LayoutDirection::column);
    ir.root.layout.gap = 8.0f;
    ir.root.style.background_color = "#1e1e2e";

    auto header = frame_node("header", "Header", 320.0f, 32.0f, LayoutDirection::row);
    header.layout.gap = 4.0f;
    header.layout.padding_top = 6.0f;
    header.layout.padding_left = 10.0f;
    header.children.push_back(text_node("title", "Reverb", 18.0f, "#ffffff"));
    ir.root.children.push_back(std::move(header));

    auto knob = frame_node("drive", "Drive", 64.0f, 64.0f, LayoutDirection::column);
    knob.audio_widget = AudioWidgetType::knob;
    knob.audio_label = "Drive";
    knob.attributes["pulpParamKey"] = "Drive";
    ir.root.children.push_back(std::move(knob));

    auto fader = frame_node("mix", "Mix", 48.0f, 96.0f, LayoutDirection::column);
    fader.audio_widget = AudioWidgetType::fader;
    fader.audio_label = "Mix";
    fader.attributes["pulpParamKey"] = "Mix";
    ir.root.children.push_back(std::move(fader));

    auto toggle = frame_node("bypass", "Bypass", 60.0f, 24.0f, LayoutDirection::column);
    toggle.type = "toggle_button";
    toggle.attributes["pulpParamKey"] = "Bypass";
    ir.root.children.push_back(std::move(toggle));

    return ir;
}

} // namespace

TEST_CASE("generate_pulp_swift emits a resolver-generic SwiftUI view",
          "[view][import][swiftui]") {
    const auto ir = build_swift_fixture();
    const auto result = generate_pulp_swift(ir, ir.asset_manifest);
    const auto& view = result.view_source;

    INFO(view);
    REQUIRE(contains(view, "import SwiftUI"));
    REQUIRE(contains(view, "import PulpSwift"));
    REQUIRE(contains(view,
        "public struct ImportedPulpView<Resolver: PulpParameterResolving & ObservableObject>: View"));
    REQUIRE(contains(view, "@ObservedObject private var resolver: Resolver"));
    REQUIRE(contains(view, "public var body: some View {"));

    // Container direction → stack kind, gap → spacing.
    REQUIRE(contains(view, "VStack(spacing: 8) {"));
    REQUIRE(contains(view, "HStack(spacing: 4) {"));

    // Text + fixed style modifiers.
    REQUIRE(contains(view, "Text(\"Reverb\")"));
    REQUIRE(contains(view, ".font(.system(size: 18))"));
    REQUIRE(contains(view, ".frame(width: 320, height: 200)"));
    REQUIRE(contains(view, ".background(Color(.sRGB"));
    REQUIRE(contains(view, ".padding(EdgeInsets(top: 6"));
}

TEST_CASE("generate_pulp_swift binds knob/slider/toggle via the name resolver",
          "[view][import][swiftui]") {
    const auto ir = build_swift_fixture();
    const auto view = generate_pulp_swift(ir, ir.asset_manifest).view_source;
    INFO(view);

    // Each bound control resolves by exact name and surfaces missing/duplicate.
    REQUIRE(contains(view, "switch resolver.resolveParameter(named: \"Drive\")"));
    REQUIRE(contains(view, "PulpKnob(parameter: p, size: 64)"));
    REQUIRE(contains(view, "switch resolver.resolveParameter(named: \"Mix\")"));
    REQUIRE(contains(view, "PulpSlider(parameter: p)"));
    REQUIRE(contains(view, "switch resolver.resolveParameter(named: \"Bypass\")"));
    REQUIRE(contains(view, "PulpToggle(parameter: p)"));

    // The missing/duplicate arms exist so a renamed/ambiguous param is visible.
    REQUIRE(contains(view, "case .missing:"));
    REQUIRE(contains(view, "case .duplicate:"));
}

TEST_CASE("generate_pulp_swift emits a code-first PulpTheme with .dark partition",
          "[view][import][swiftui]") {
    const auto ir = build_swift_fixture();
    const auto theme = generate_pulp_swift(ir, ir.asset_manifest).theme_source;
    INFO(theme);

    REQUIRE(contains(theme, "public enum PulpTheme {"));
    // color.bg has a .dark override → dynamic; accent is base-only → static.
    // The dynamic-color helper is nested + private (no top-level symbol that
    // would clash across multiple generated theme files).
    REQUIRE(contains(theme, "private static func dynamicColor(light: Color, dark: Color) -> Color"));
    REQUIRE(contains(theme, "public static let colorBg: Color = dynamicColor("));
    REQUIRE(contains(theme, "public static let accent: Color = Color(.sRGB"));
    // dimensions + strings.
    REQUIRE(contains(theme, "public static let spacingSm: CGFloat = 4"));
    REQUIRE(contains(theme, "public static let fontBody: String = \"Inter\""));
    // Swift keyword token → backtick-escaped identifier (else won't compile).
    REQUIRE(contains(theme, "public static let `default`: Color ="));
    // foo.bar + foo-bar both camel to fooBar → one keeps fooBar, the other is
    // de-duplicated (fooBar2). Without dedup the enum has a duplicate member.
    REQUIRE(contains(theme, "public static let fooBar: Color ="));
    REQUIRE(contains(theme, "public static let fooBar2: Color ="));
    // Keyword base escaped; its .dark companion escapes the FULL id, not
    // `` `switch`Dark `` (which would not compile).
    REQUIRE(contains(theme, "public static let `switch`: CGFloat ="));
    REQUIRE(contains(theme, "public static let switchDark: CGFloat ="));
    REQUIRE(contains(theme, "public static let `class`: String ="));
    REQUIRE(contains(theme, "public static let classDark: String ="));
}

TEST_CASE("generate_pulp_swift emits a name-keyed binding manifest with the resolution block",
          "[view][import][swiftui]") {
    const auto ir = build_swift_fixture();
    const auto manifest = generate_pulp_swift(ir, ir.asset_manifest).binding_manifest;
    INFO(manifest);

    REQUIRE(contains(manifest, "\"schema\": \"pulp-native-swiftui-binding-manifest-v1\""));
    REQUIRE(contains(manifest, "\"strategy\": \"pulp_parameter_name_exact\""));
    // B4 conventions block (gesture grouping / normalized range / poll).
    REQUIRE(contains(manifest, "\"gesture_grouping\": true"));
    REQUIRE(contains(manifest, "\"value_range\": \"normalized_0_1\""));
    REQUIRE(contains(manifest, "\"native_primitive\": \"knob\""));
    // resolve_name is the display-name matched against PulpParameter.name;
    // param_key carries pulpParamKey in the same field the C++ manifest uses.
    REQUIRE(contains(manifest, "\"resolve_name\": \"Drive\""));
    REQUIRE(contains(manifest, "\"param_key\": \"Drive\""));
    REQUIRE(contains(manifest, "\"resolution_strategy\": \"pulp_parameter_name_exact\""));
    REQUIRE(contains(manifest, "\"native_primitive\": \"fader\""));
    REQUIRE(contains(manifest, "\"native_primitive\": \"toggle_button\""));
    REQUIRE(contains(manifest, "\"resolve_name\": \"Bypass\""));
}

TEST_CASE("SwiftUI binding manifest carries the same contract fields as the C++ manifest",
          "[view][import][swiftui]") {
    // B4 parity: a node with the full pulp* binding contract must surface the
    // identical field/value pairs in BOTH the C++ manifest and the SwiftUI
    // manifest. This pins parity + the eligibility gate, so neither side drifts.
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root = frame_node("root", "Panel", 200.0f, 200.0f, LayoutDirection::column);
    auto knob = frame_node("k", "Drive", 60.0f, 60.0f, LayoutDirection::column);
    knob.audio_widget = AudioWidgetType::knob;
    knob.audio_label = "Drive";
    knob.stable_anchor_id = "anchor-knob";
    knob.attributes["pulpRouteId"] = "route.drive";
    knob.attributes["pulpRouteType"] = "native_cpp";
    knob.attributes["pulpParamKey"] = "filter.cutoff_hz";
    knob.attributes["pulpBindingModule"] = "filter";
    knob.attributes["pulpBindingParam"] = "cutoff_hz";
    knob.attributes["pulpDescription"] = "Cutoff frequency";
    ir.root.children.push_back(std::move(knob));

    const auto cpp = generate_pulp_cpp(ir, ir.asset_manifest).binding_manifest;
    const auto swift = generate_pulp_swift(ir, ir.asset_manifest).binding_manifest;
    INFO("cpp manifest:\n" << cpp);
    INFO("swift manifest:\n" << swift);
    for (const char* kv : {
             "\"id\": \"route.drive\"",
             "\"anchor_id\": \"anchor-knob\"",
             "\"native_primitive\": \"knob\"",
             "\"route_type\": \"native_cpp\"",
             "\"param_key\": \"filter.cutoff_hz\"",
             "\"binding_module\": \"filter\"",
             "\"binding_param\": \"cutoff_hz\"",
             "\"description\": \"Cutoff frequency\"",
         }) {
        REQUIRE(contains(cpp, kv));    // present in the C++ manifest
        REQUIRE(contains(swift, kv));  // and the identical field in the SwiftUI manifest
    }
    // The SwiftUI manifest additionally carries the name-resolution block.
    REQUIRE(contains(swift, "\"resolve_name\": \"Drive\""));
    REQUIRE_FALSE(contains(cpp, "pulp_parameter_name_exact"));  // cpp resolves by C++ binding, not name
}

TEST_CASE("generate_pulp_swift sanitizes a caller-supplied theme type name",
          "[view][import][swiftui]") {
    const auto ir = build_swift_fixture();
    SwiftExportOptions opts;
    opts.theme_type_name = "my-theme";  // not a valid Swift type name
    REQUIRE(contains(generate_pulp_swift(ir, ir.asset_manifest, opts).theme_source,
                     "public enum MyTheme {"));
    opts.theme_type_name = "Type";      // reserved type name → backtick-escaped
    REQUIRE(contains(generate_pulp_swift(ir, ir.asset_manifest, opts).theme_source,
                     "public enum `Type` {"));
}

TEST_CASE("generate_pulp_swift root view name is sanitized to a Swift type",
          "[view][import][swiftui]") {
    const auto ir = build_swift_fixture();
    SwiftExportOptions opts;
    opts.root_view_name = "my-reverb panel";
    const auto view = generate_pulp_swift(ir, ir.asset_manifest, opts).view_source;
    INFO(view);
    REQUIRE(contains(view, "public struct MyReverbPanel<"));
}

// ── swiftc compile gate ──────────────────────────────────────────────────

namespace {

fs::path swiftc_path() {
    if (std::string p = PULP_TEST_SWIFTC; !p.empty() && fs::exists(p)) return p;
    if (fs::exists("/usr/bin/swiftc")) return "/usr/bin/swiftc";
    return {};
}

fs::path unique_temp_dir(const std::string& prefix) {
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    auto dir = fs::temp_directory_path() / (prefix + "-" + std::to_string(tick));
    fs::create_directories(dir);
    return dir;
}

void write_file(const fs::path& path, const std::string& body) {
    std::ofstream f(path);
    REQUIRE(f.is_open());
    f << body;
}

struct SwiftGate { bool runnable = false; bool ok = false; std::string diagnostics; };

// Type-check the given generated .swift sources against the real PulpSwift
// module. The module is emitted once per process (cached). PulpBridge.swift is
// the link seam, so -emit-module needs no native host. `runnable=false` means
// the environment lacks the Swift/SwiftUI SDK (e.g. the Linux lane) → the test
// skips; `ok` is the actual type-check verdict and is what the gate asserts.
SwiftGate swiftc_typecheck(const std::vector<std::string>& sources) {
    static bool tried = false, module_ok = false;
    static fs::path module_dir;
    const auto swiftc = swiftc_path();
    if (swiftc.empty()) return {false, false, "swiftc unavailable"};
    const fs::path pulp_swift = fs::path(PULP_REPO_ROOT) / "apple" / "Sources" / "PulpSwift";
    if (!fs::exists(pulp_swift / "PulpViews.swift"))
        return {false, false, "PulpSwift sources not found under PULP_REPO_ROOT"};
    if (!tried) {
        tried = true;
        module_dir = unique_temp_dir("pulp-swiftui-module");
        std::vector<std::string> emit = {
            "-emit-module", "-module-name", "PulpSwift",
            "-emit-module-path", (module_dir / "PulpSwift.swiftmodule").string(),
            (pulp_swift / "PulpBridge.swift").string(),
            (pulp_swift / "PulpParameter.swift").string(),
            (pulp_swift / "PulpViews.swift").string(),
        };
        auto b = pulp::platform::exec(swiftc.string(), emit, 120000);
        module_ok = (b.exit_code == 0 && !b.timed_out);
        if (!module_ok) return {false, false, "baseline module emit failed:\n" + b.stderr_output};
    }
    if (!module_ok) return {false, false, "baseline module unavailable"};
    std::vector<std::string> args = {"-typecheck", "-I", module_dir.string()};
    for (const auto& s : sources) args.push_back(s);
    auto c = pulp::platform::exec(swiftc.string(), args, 120000);
    return {true, c.exit_code == 0 && !c.timed_out, c.stderr_output};
}

// Run the swiftc gate over the result of generate_pulp_swift for `ir`.
void require_generated_swift_compiles(const DesignIR& ir, const std::string& tag) {
    const auto result = generate_pulp_swift(ir, ir.asset_manifest);
    auto tmp = unique_temp_dir("pulp-swiftui-gate-" + tag);
    const fs::path view_swift = tmp / "ImportedPulpView.swift";
    const fs::path theme_swift = tmp / "PulpTheme.swift";
    write_file(view_swift, result.view_source);
    write_file(theme_swift, result.theme_source);
    auto gate = swiftc_typecheck({view_swift.string(), theme_swift.string()});
    if (!gate.runnable) {
        WARN("swiftc gate skipped (" << tag << "): " << gate.diagnostics);
        SKIP("Swift toolchain/SDK unavailable");
        return;
    }
    INFO("generated view:\n" << result.view_source);
    INFO("generated theme:\n" << result.theme_source);
    INFO("swiftc stderr:\n" << gate.diagnostics);
    REQUIRE(gate.ok);
}

} // namespace

TEST_CASE("generated SwiftUI type-checks against the real PulpSwift module",
          "[view][import][swiftui][swiftc]") {
    // Exercises stacks/text/knob/slider/toggle + the keyword-escaped and
    // de-duplicated theme identifiers from build_swift_fixture.
    require_generated_swift_compiles(build_swift_fixture(), "fixture");
}

TEST_CASE("generated SwiftUI type-checks with >100 children (recursive batching)",
          "[view][import][swiftui][swiftc]") {
    // ViewBuilder caps a container at 10 direct children; the emitter batches
    // recursively into nested Groups. 101 children would overflow a one-level
    // batch (11 Groups), so this proves the recursion keeps it compiling.
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root = frame_node("root", "Big", 400.0f, 4000.0f, LayoutDirection::column);
    for (int i = 0; i < 101; ++i)
        ir.root.children.push_back(text_node("t" + std::to_string(i),
                                             "row " + std::to_string(i), 12.0f, "#ffffff"));
    require_generated_swift_compiles(ir, "big");
}

TEST_CASE("generated SwiftUI type-checks with hostile control chars in names/text/tokens",
          "[view][import][swiftui][swiftc]") {
    // Embedded CR/LF must not break out of a `// ...` comment, and any C0
    // control byte (ESC 0x1b, BEL 0x07, DEL 0x7f) inside a Text literal, token
    // string, or binding resolve name must be escaped, not emitted bare —
    // otherwise the generated Swift won't compile. include_comments defaults true.
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.tokens.strings["evil.string"] = std::string("a\x1b") + "b\x07" + "c";  // ESC/BEL in token value
    ir.root = frame_node("root", "Panel\nnotSwift( {[ \r evil", 200.0f, 100.0f,
                         LayoutDirection::column);
    ir.root.children.push_back(
        text_node("child\nbreak", std::string("hi\x1b there\x7f"), 12.0f, "#ffffff"));
    auto knob = frame_node("k", "Drive", 60.0f, 60.0f, LayoutDirection::column);
    knob.audio_widget = AudioWidgetType::knob;
    knob.audio_label = std::string("Gain\x1b\x07");  // control bytes in the bound resolve name
    ir.root.children.push_back(std::move(knob));
    require_generated_swift_compiles(ir, "hostile-controls");
}

// ── B2: full style, text-runs, flex-fidelity warnings ─────────────────────

namespace {

// Generate the view with a fidelity sink attached and return both.
struct B2Out { std::string view; std::vector<FidelityIssue> issues; };
B2Out generate_with_fidelity(const DesignIR& ir, SwiftExportOptions opts = {}) {
    B2Out o;
    opts.fidelity_report = &o.issues;
    o.view = generate_pulp_swift(ir, ir.asset_manifest, opts).view_source;
    return o;
}

bool has_kind(const std::vector<FidelityIssue>& issues, const std::string& kind) {
    for (const auto& i : issues) if (i.kind == kind) return true;
    return false;
}

} // namespace

TEST_CASE("generate_pulp_swift emits the full B2 visual style set",
          "[view][import][swiftui]") {
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root = frame_node("card", "Card", 200.0f, 120.0f, LayoutDirection::column);
    auto& st = ir.root.style;
    st.opacity = 0.8f;
    st.border_radius = 12.0f;
    st.border_width = 2.0f;
    st.border_color = "#334455";
    st.background_gradient = "linear-gradient(90deg, #ff0000, #0000ff)";
    st.transform = "rotate(5deg) scale(1.1)";
    IRBoxShadow sh; sh.offset_x = 0.0f; sh.offset_y = 4.0f; sh.blur = 8.0f;
    sh.color = "rgba(0, 0, 0, 0.25)";
    st.box_shadow.push_back(sh);
    ir.root.children.push_back(text_node("t", "hi", 12.0f, "#ffffff"));

    const auto view = generate_pulp_swift(ir, ir.asset_manifest).view_source;
    INFO(view);
    REQUIRE(contains(view, ".opacity(0.8)"));
    REQUIRE(contains(view, ".cornerRadius(12)"));
    REQUIRE(contains(view, ".overlay(RoundedRectangle(cornerRadius: 12).stroke(Color(.sRGB"));
    REQUIRE(contains(view, ", lineWidth: 2))"));
    REQUIRE(contains(view, ".background(LinearGradient(colors: ["));
    REQUIRE(contains(view, "startPoint: .leading, endPoint: .trailing"));
    REQUIRE(contains(view, ".shadow(color: Color(.sRGB"));
    REQUIRE(contains(view, "radius: 4, x: 0, y: 4)"));
    REQUIRE(contains(view, ".rotationEffect(.degrees(5))"));
    REQUIRE(contains(view, ".scaleEffect(1.1)"));
}

TEST_CASE("generate_pulp_swift parses rgb()/rgba() colour tokens",
          "[view][import][swiftui]") {
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root = frame_node("r", "R", 50.0f, 50.0f, LayoutDirection::column);
    ir.root.style.background_color = "rgb(255, 128, 0)";
    const auto view = generate_pulp_swift(ir, ir.asset_manifest).view_source;
    INFO(view);
    // 255→1, 128→~0.501961, 0→0, default alpha 1.
    REQUIRE(contains(view, ".background(Color(.sRGB, red: 1, green: 0.50"));
}

TEST_CASE("generate_pulp_swift parses gradients with rgba stops (internal spaces)",
          "[view][import][swiftui]") {
    // rgba() stops carry commas AND spaces inside their own parens; the stop
    // splitter must not truncate the colour at the first internal space.
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root = frame_node("r", "R", 50.0f, 50.0f, LayoutDirection::column);
    ir.root.style.background_gradient =
        "linear-gradient(to right, rgba(10, 20, 30, 0.5), #ffffff)";
    const auto view = generate_pulp_swift(ir, ir.asset_manifest).view_source;
    INFO(view);
    REQUIRE(contains(view, "LinearGradient(colors: ["));
    // Both stops survived: the rgba red 10/255≈0.0392 (not truncated at the
    // internal space) and the trailing white hex.
    REQUIRE(contains(view, "red: 0.039"));
    REQUIRE(contains(view, "Color(.sRGB, red: 1, green: 1, blue: 1"));
    REQUIRE(contains(view, "startPoint: .leading, endPoint: .trailing"));
}

TEST_CASE("generate_pulp_swift lowers mixed-style text_runs to concatenated Text",
          "[view][import][swiftui]") {
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root = frame_node("r", "R", 200.0f, 40.0f, LayoutDirection::column);
    auto label = text_node("label", "Hello bold world", 14.0f, "#111111");
    IRTextRun bold; bold.start = 6; bold.end = 10; bold.font_weight = 700;  // "bold"
    label.text_runs.push_back(bold);
    ir.root.children.push_back(std::move(label));

    const auto view = generate_pulp_swift(ir, ir.asset_manifest).view_source;
    INFO(view);
    REQUIRE(contains(view, "Text(\"Hello \")"));
    REQUIRE(contains(view, "+ Text(\"bold\")"));
    REQUIRE(contains(view, ".fontWeight(.bold)"));
    REQUIRE(contains(view, "+ Text(\" world\")"));
}

TEST_CASE("generate_pulp_swift flags per-side borders and multi/inset shadows",
          "[view][import][swiftui]") {
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root = frame_node("r", "R", 80.0f, 80.0f, LayoutDirection::column);
    auto& st = ir.root.style;
    st.border_top_width = 4.0f;
    st.border_bottom_width = 1.0f;   // differs → per-side
    st.border_color = "#222222";
    IRBoxShadow a; a.blur = 4.0f; a.color = "#000000";
    IRBoxShadow b; b.blur = 2.0f; b.color = "#111111";
    st.box_shadow = {a, b};          // >1 layer → informational drop
    auto out = generate_with_fidelity(ir);
    INFO(out.view);
    REQUIRE(contains(out.view, ", lineWidth: 4))"));  // heaviest side
    REQUIRE(has_kind(out.issues, "swiftui-per-side-border"));
    REQUIRE(has_kind(out.issues, "swiftui-multi-shadow"));
    // Both lose visible appearance (a dropped glow / a side's distinct stroke),
    // so they are hard divergences --strict-fidelity must catch.
    REQUIRE(count_strict_fidelity_failures(out.issues) >= 2);
}

TEST_CASE("generate_pulp_swift flags a per-side border that differs only in colour",
          "[view][import][swiftui]") {
    // Uniform widths but a distinct per-side colour still loses a side; the
    // width-only check would have missed it.
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root = frame_node("r", "R", 60.0f, 60.0f, LayoutDirection::column);
    ir.root.style.border_width = 1.0f;
    ir.root.style.border_top_color = "#ff0000";
    ir.root.style.border_right_color = "#00ff00";  // differs → per-side
    auto out = generate_with_fidelity(ir);
    REQUIRE(has_kind(out.issues, "swiftui-per-side-border"));
    REQUIRE(count_strict_fidelity_failures(out.issues) >= 1);
}

TEST_CASE("generate_pulp_swift flags a single side overriding the border shorthand",
          "[view][import][swiftui]") {
    // border-color:#fff (shorthand) + border-top-color:#f00 → effective sides
    // [#f00, #fff, #fff, #fff]; comparing side overrides alone would miss it,
    // so the effective-value comparison is what catches it.
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root = frame_node("r", "R", 60.0f, 60.0f, LayoutDirection::column);
    ir.root.style.border_width = 1.0f;
    ir.root.style.border_color = "#ffffff";       // shorthand
    ir.root.style.border_top_color = "#ff0000";   // overrides only the top
    auto out = generate_with_fidelity(ir);
    REQUIRE(has_kind(out.issues, "swiftui-per-side-border"));
    REQUIRE(count_strict_fidelity_failures(out.issues) >= 1);
}

TEST_CASE("generate_pulp_swift treats a uniform border (shorthand only) as uniform",
          "[view][import][swiftui]") {
    // No side overrides → no per-side divergence; the overlay is emitted clean.
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root = frame_node("r", "R", 60.0f, 60.0f, LayoutDirection::column);
    ir.root.style.border_width = 2.0f;
    ir.root.style.border_color = "#334455";
    auto out = generate_with_fidelity(ir);
    REQUIRE(contains(out.view, ", lineWidth: 2))"));
    REQUIRE_FALSE(has_kind(out.issues, "swiftui-per-side-border"));
}

TEST_CASE("generate_pulp_swift does not mis-parse repeating-linear-gradient as linear",
          "[view][import][swiftui]") {
    // `repeating-linear-gradient(...)` contains the substring `linear-gradient`;
    // fn_args' boundary check must reject it so it falls back to flat fill, not
    // a silently-non-repeating LinearGradient.
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root = frame_node("r", "R", 50.0f, 50.0f, LayoutDirection::column);
    ir.root.style.background_gradient =
        "repeating-linear-gradient(45deg, #f00, #00f 10px)";
    auto out = generate_with_fidelity(ir);
    INFO(out.view);
    REQUIRE_FALSE(contains(out.view, "LinearGradient(colors:"));
    REQUIRE(has_kind(out.issues, "swiftui-gradient"));  // unsupported → flagged
}

TEST_CASE("generate_pulp_swift approximates single-child space-between as flex-start",
          "[view][import][swiftui]") {
    // CSS space-between with one item is flex-start; SwiftUI needs a trailing
    // Spacer to push it to the main-axis start.
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root = frame_node("r", "Row", 200.0f, 40.0f, LayoutDirection::row);
    ir.root.layout.justify = LayoutAlign::space_between;
    ir.root.children.push_back(text_node("only", "X", 12.0f, "#fff"));
    auto out = generate_with_fidelity(ir);
    INFO(out.view);
    REQUIRE(contains(out.view, "Spacer()"));
}

TEST_CASE("generate_pulp_swift maps cross-axis alignment and approximates justify",
          "[view][import][swiftui]") {
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root = frame_node("r", "Row", 300.0f, 40.0f, LayoutDirection::row);
    ir.root.layout.align = LayoutAlign::flex_end;          // → .bottom on an HStack
    ir.root.layout.justify = LayoutAlign::space_between;   // → Spacer interposition
    ir.root.children.push_back(text_node("a", "A", 12.0f, "#fff"));
    ir.root.children.push_back(text_node("b", "B", 12.0f, "#fff"));
    auto out = generate_with_fidelity(ir);
    INFO(out.view);
    REQUIRE(contains(out.view, "HStack(alignment: .bottom, spacing:"));
    REQUIRE(contains(out.view, "Spacer()"));
    REQUIRE(has_kind(out.issues, "swiftui-flex-justify"));
    REQUIRE(count_strict_fidelity_failures(out.issues) == 0);  // Spacer approx is advisory
}

TEST_CASE("generate_pulp_swift flags absolute/wrap/skew as hard divergences",
          "[view][import][swiftui]") {
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root = frame_node("r", "R", 200.0f, 200.0f, LayoutDirection::column);
    ir.root.layout.wrap = true;                  // flex container (not grid)
    ir.root.style.transform = "skewX(10deg)";

    auto abs_child = frame_node("a", "Abs", 40.0f, 40.0f, LayoutDirection::column);
    abs_child.style.position = "absolute";
    abs_child.style.left = 10.0f;
    abs_child.style.top = 20.0f;
    abs_child.children.push_back(text_node("c", "x", 10.0f, "#fff"));
    ir.root.children.push_back(std::move(abs_child));

    auto out = generate_with_fidelity(ir);
    INFO(out.view);
    REQUIRE(contains(out.view, ".offset(x: 10, y: 20)"));
    REQUIRE(has_kind(out.issues, "swiftui-flex-wrap"));
    REQUIRE(has_kind(out.issues, "swiftui-transform"));
    REQUIRE(has_kind(out.issues, "swiftui-absolute-position"));
    // These genuinely render wrong, so --strict-fidelity must be able to gate.
    REQUIRE(count_strict_fidelity_failures(out.issues) >= 3);
}

TEST_CASE("generate_pulp_swift lowers a CSS grid to LazyVGrid (B5)",
          "[view][import][swiftui]") {
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root = frame_node("r", "Grid", 300.0f, 200.0f, LayoutDirection::row);
    ir.root.layout.display = "grid";
    ir.root.layout.grid_template_columns = "1fr 1fr 1fr";  // 3 columns
    ir.root.layout.column_gap = 8.0f;
    ir.root.layout.row_gap = 12.0f;
    for (int i = 0; i < 5; ++i)
        ir.root.children.push_back(text_node("c" + std::to_string(i),
                                             "cell" + std::to_string(i), 12.0f, "#fff"));
    auto out = generate_with_fidelity(ir);
    INFO(out.view);
    // 3 flexible columns, row spacing from row_gap, column spacing from column_gap.
    REQUIRE(contains(out.view, "LazyVGrid(columns: [GridItem(.flexible(), spacing: 8), "
                               "GridItem(.flexible(), spacing: 8), GridItem(.flexible(), spacing: 8)], "
                               "spacing: 12) {"));
    // Track sizing is approximated, but it RENDERS — informational, not gating.
    REQUIRE(has_kind(out.issues, "swiftui-grid-tracks"));
    REQUIRE_FALSE(has_kind(out.issues, "swiftui-grid"));  // no longer a hard "lost" divergence
    REQUIRE(count_strict_fidelity_failures(out.issues) == 0);
}

TEST_CASE("generate_pulp_swift counts grid columns from repeat() and mixed tracks",
          "[view][import][swiftui]") {
    auto grid_with = [](const std::string& tracks) {
        DesignIR ir;
        ir.source = DesignSource::figma;
        ir.root = frame_node("r", "G", 200.0f, 200.0f, LayoutDirection::column);
        ir.root.layout.grid_template_columns = tracks;
        ir.root.children.push_back(text_node("c", "x", 10.0f, "#fff"));
        return generate_pulp_swift(ir, ir.asset_manifest).view_source;
    };
    auto count_items = [](const std::string& view) {
        std::size_t n = 0, pos = 0;
        while ((pos = view.find("GridItem(.flexible", pos)) != std::string::npos) { ++n; pos += 4; }
        return n;
    };
    REQUIRE(count_items(grid_with("repeat(4, 1fr)")) == 4);
    REQUIRE(count_items(grid_with("minmax(100px, 1fr) 2fr")) == 2);
    REQUIRE(count_items(grid_with("100px auto 1fr")) == 3);
    // repeat() followed by more tracks must SUM, not stop at the repeat count
    // repeat(2,1fr) 2fr → 2 + 1 = 3.
    REQUIRE(count_items(grid_with("repeat(2, 1fr) 2fr")) == 3);
    REQUIRE(count_items(grid_with("repeat(2, 1fr 2fr)")) == 4);        // pattern has 2 tracks
    REQUIRE(count_items(grid_with("repeat(auto-fill, 120px)")) == 1);  // unknowable → pattern count
    // Hostile/degenerate track lists must not blow up: a huge repeat count is
    // clamped (bounds the emitted GridItem array), and deep nesting returns a
    // finite count without recursing to a stack overflow.
    REQUIRE(count_items(grid_with("repeat(100000, 1fr)")) <= 1024);
    REQUIRE(count_items(grid_with(
        "repeat(2, repeat(2, repeat(2, repeat(2, repeat(2, 1fr)))))")) >= 1);
}

TEST_CASE("generate_pulp_swift emits no inf/nan for non-finite dimensions",
          "[view][import][swiftui]") {
    // The transform parser casts std::stod to float; a huge value overflows to
    // +inf, which has no valid Swift Float literal. format_float must clamp
    // non-finite values to 0 rather than emit `.scaleEffect(inf)` (won't
    // compile) — likewise for rotate/translate and any degenerate dimension.
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root = frame_node("r", "R", 50.0f, 50.0f, LayoutDirection::column);
    ir.root.style.transform = "scale(1e40) rotate(1e40deg) translate(1e40px, 1e40px)";
    ir.root.children.push_back(text_node("c", "x", 10.0f, "#fff"));
    const auto view = generate_pulp_swift(ir, ir.asset_manifest).view_source;
    INFO(view);
    REQUIRE_FALSE(contains(view, "inf)"));   // e.g. .scaleEffect(inf) — not .infinity
    REQUIRE_FALSE(contains(view, "inf,"));   // e.g. .offset(x: inf, …)
    REQUIRE_FALSE(contains(view, "nan"));
    REQUIRE_FALSE(contains(view, "1e40"));
    REQUIRE(contains(view, ".scaleEffect(0)"));        // clamped, not inf
    require_generated_swift_compiles(ir, "non-finite");  // and it compiles
}

TEST_CASE("generate_pulp_swift flags dropped grid item placement as a hard divergence",
          "[view][import][swiftui]") {
    // grid-column/grid-row item placement is lost in a LazyVGrid auto-flow, so
    // it must be a hard (gating) fidelity issue, not the advisory track note.
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root = frame_node("r", "Grid", 300.0f, 200.0f, LayoutDirection::row);
    ir.root.layout.grid_template_columns = "1fr 1fr";
    auto cell = text_node("c", "spanning", 12.0f, "#fff");
    cell.layout.grid_column = "1 / 3";   // explicit placement
    ir.root.children.push_back(std::move(cell));
    ir.root.children.push_back(text_node("c2", "plain", 12.0f, "#fff"));
    auto out = generate_with_fidelity(ir);
    INFO(out.view);
    REQUIRE(contains(out.view, "LazyVGrid(columns: ["));
    REQUIRE(has_kind(out.issues, "swiftui-grid-placement"));
    REQUIRE(count_strict_fidelity_failures(out.issues) >= 1);  // gates --strict-fidelity
}

TEST_CASE("generate_pulp_swift flags an inline/data-URI image as a hard divergence",
          "[view][import][swiftui]") {
    // A data: URI has no catalog name and isn't remote, so Image("id") renders
    // nothing — a hard divergence, not the advisory bundled-asset note.
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root = frame_node("root", "R", 100.0f, 100.0f, LayoutDirection::column);
    auto img = frame_node("inline", "Inline", 32.0f, 32.0f, LayoutDirection::column);
    img.type = "image";
    img.attributes["srcAssetId"] = "inline_icon";
    ir.root.children.push_back(std::move(img));
    IRAssetManifest manifest;
    IRAssetRef a; a.asset_id = "inline_icon";
    a.original_uri = "data:image/png;base64,iVBORw0KGgo=";
    manifest.assets = {a};
    ir.asset_manifest = manifest;

    std::vector<FidelityIssue> issues;
    SwiftExportOptions opts; opts.fidelity_report = &issues;
    const auto view = generate_pulp_swift(ir, manifest, opts).view_source;
    INFO(view);
    REQUIRE(contains(view, "Image(\"inline_icon\")"));
    REQUIRE(has_kind(issues, "swiftui-inline-asset"));
    REQUIRE(count_strict_fidelity_failures(issues) >= 1);
}

TEST_CASE("generate_pulp_swift maps image assets to Image / AsyncImage (B5)",
          "[view][import][swiftui]") {
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root = frame_node("root", "R", 200.0f, 200.0f, LayoutDirection::column);

    // Bundled/local asset → Image("<asset_id>") referencing the asset catalog.
    auto logo = frame_node("logo", "Logo", 48.0f, 48.0f, LayoutDirection::column);
    logo.type = "image";
    logo.attributes["srcAssetId"] = "brand_logo";
    ir.root.children.push_back(std::move(logo));

    // Remote http(s) asset → AsyncImage(url:).
    auto remote = frame_node("hero", "Hero", 200.0f, 100.0f, LayoutDirection::column);
    remote.type = "image";
    remote.attributes["srcAssetId"] = "hero_img";
    ir.root.children.push_back(std::move(remote));

    IRAssetManifest manifest;
    IRAssetRef a; a.asset_id = "brand_logo"; a.local_path = "/tmp/logo.png"; a.original_uri = "logo.png";
    IRAssetRef b; b.asset_id = "hero_img"; b.original_uri = "https://cdn.example.com/hero.png";
    manifest.assets = {a, b};
    ir.asset_manifest = manifest;

    std::vector<FidelityIssue> issues;
    SwiftExportOptions opts; opts.fidelity_report = &issues;
    const auto view = generate_pulp_swift(ir, manifest, opts).view_source;
    INFO(view);
    REQUIRE(contains(view, "Image(\"brand_logo\")"));
    REQUIRE(contains(view, ".resizable()"));
    REQUIRE(contains(view, "AsyncImage(url: URL(string: \"https://cdn.example.com/hero.png\"))"));
    REQUIRE(has_kind(issues, "swiftui-bundled-asset"));  // advises bundling brand_logo
}

TEST_CASE("generated SwiftUI with full B2 style + text-runs type-checks",
          "[view][import][swiftui][swiftc]") {
    // Compile-gate the new modifier surface: gradient, border overlay, shadow,
    // opacity, corner radius, transform, mixed-style Text concatenation, and the
    // Spacer-distributed / absolute-offset container paths all in one view.
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root = frame_node("root", "Card", 240.0f, 180.0f, LayoutDirection::column);
    ir.root.layout.justify = LayoutAlign::space_between;
    ir.root.layout.align = LayoutAlign::center;
    ir.root.style.opacity = 0.9f;
    ir.root.style.border_radius = 8.0f;
    ir.root.style.border_width = 1.5f;
    ir.root.style.border_color = "#445566";
    ir.root.style.background_gradient = "linear-gradient(to bottom, #102030, rgba(20,30,40,0.5))";
    IRBoxShadow sh; sh.offset_y = 3.0f; sh.blur = 6.0f; sh.color = "rgba(0,0,0,0.3)";
    ir.root.style.box_shadow.push_back(sh);
    ir.root.style.transform = "rotate(2deg)";

    auto label = text_node("hdr", "Mix and bold tail", 16.0f, "#eeeeee");
    IRTextRun run; run.start = 8; run.end = 12; run.font_weight = 700;
    run.color = "#ffaa00"; run.font_style = "italic";
    label.text_runs.push_back(run);
    ir.root.children.push_back(std::move(label));

    auto abs_box = frame_node("badge", "Badge", 24.0f, 24.0f, LayoutDirection::column);
    abs_box.style.position = "absolute";
    abs_box.style.left = 6.0f; abs_box.style.top = 6.0f;
    abs_box.style.border_top_left_radius = 4.0f;     // partial per-corner
    abs_box.style.border_top_right_radius = 12.0f;   // uneven → uniform fallback
    abs_box.children.push_back(text_node("n", "9", 10.0f, "#000000"));
    ir.root.children.push_back(std::move(abs_box));

    require_generated_swift_compiles(ir, "b2-style");
}

// ── B3: widget coverage (meter/xy_pad/waveform/spectrum + text button) ─────

namespace {

IRNode audio_widget_node(std::string id, std::string label, AudioWidgetType type) {
    IRNode n = frame_node(std::move(id), label, 80.0f, 40.0f, LayoutDirection::column);
    n.audio_widget = type;
    n.audio_label = label;
    return n;
}

DesignIR build_b3_widget_fixture() {
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root = frame_node("root", "Widgets", 320.0f, 240.0f, LayoutDirection::column);
    ir.root.children.push_back(audio_widget_node("m", "Level", AudioWidgetType::meter));
    ir.root.children.push_back(audio_widget_node("xy", "Pan", AudioWidgetType::xy_pad));
    ir.root.children.push_back(audio_widget_node("wf", "Scope", AudioWidgetType::waveform));
    ir.root.children.push_back(audio_widget_node("sp", "Analyzer", AudioWidgetType::spectrum));
    auto btn = frame_node("go", "Go", 60.0f, 24.0f, LayoutDirection::column);
    btn.type = "button";
    btn.text_content = "Go";
    ir.root.children.push_back(std::move(btn));
    return ir;
}

} // namespace

TEST_CASE("generate_pulp_swift binds meter/xy_pad/waveform/spectrum + emits text buttons",
          "[view][import][swiftui]") {
    const auto ir = build_b3_widget_fixture();
    auto out = generate_with_fidelity(ir);
    INFO(out.view);
    // Each visualizer/meter resolves by name and binds its parameter.
    REQUIRE(contains(out.view, "switch resolver.resolveParameter(named: \"Level\")"));
    REQUIRE(contains(out.view, "PulpMeter(parameter: p)"));
    REQUIRE(contains(out.view, "PulpXYPad(parameter: p)"));
    REQUIRE(contains(out.view, "PulpWaveform(parameter: p)"));
    REQUIRE(contains(out.view, "PulpSpectrum(parameter: p)"));
    // Text button → an inert SwiftUI Button carrying the label.
    REQUIRE(contains(out.view, "Button(action: {}) {"));
    REQUIRE(contains(out.view, "Text(\"Go\")"));
    // The approximations are flagged but advisory (they still render + bind).
    REQUIRE(has_kind(out.issues, "swiftui-xypad-single-axis"));
    REQUIRE(has_kind(out.issues, "swiftui-static-visualizer"));
    REQUIRE(has_kind(out.issues, "swiftui-inert-button"));
    REQUIRE(count_strict_fidelity_failures(out.issues) == 0);
}

TEST_CASE("generate_pulp_swift renders a text button's label/icon children",
          "[view][import][swiftui]") {
    // A button with children must render them in the label closure, not drop
    // them for a bare Text(name).
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root = frame_node("root", "R", 120.0f, 40.0f, LayoutDirection::column);
    auto btn = frame_node("save", "Save", 80.0f, 24.0f, LayoutDirection::row);
    btn.type = "button";
    btn.children.push_back(text_node("ico", "*", 12.0f, "#ffffff"));
    btn.children.push_back(text_node("lbl", "Save", 12.0f, "#ffffff"));
    ir.root.children.push_back(std::move(btn));
    const auto view = generate_pulp_swift(ir, ir.asset_manifest).view_source;
    INFO(view);
    REQUIRE(contains(view, "Button(action: {}) {"));
    REQUIRE(contains(view, "Text(\"*\")"));     // icon child rendered
    REQUIRE(contains(view, "Text(\"Save\")"));  // label child rendered
    require_generated_swift_compiles(ir, "button-children");
}

TEST_CASE("generate_pulp_swift records the new audio widgets in the binding manifest",
          "[view][import][swiftui]") {
    const auto ir = build_b3_widget_fixture();
    const auto manifest = generate_pulp_swift(ir, ir.asset_manifest).binding_manifest;
    INFO(manifest);
    REQUIRE(contains(manifest, "\"native_primitive\": \"meter\""));
    REQUIRE(contains(manifest, "\"native_primitive\": \"xy_pad\""));
    REQUIRE(contains(manifest, "\"native_primitive\": \"waveform\""));
    REQUIRE(contains(manifest, "\"native_primitive\": \"spectrum\""));
    REQUIRE(contains(manifest, "\"resolve_name\": \"Analyzer\""));
}

TEST_CASE("generated SwiftUI with the B3 widget set type-checks",
          "[view][import][swiftui][swiftc]") {
    // Compile-gate every new widget view (PulpMeter/PulpXYPad/PulpWaveform/
    // PulpSpectrum) + the Button path against the real PulpSwift module.
    require_generated_swift_compiles(build_b3_widget_fixture(), "b3-widgets");
}

TEST_CASE("generated SwiftUI with a grid + bundled/remote images type-checks (B5)",
          "[view][import][swiftui][swiftc]") {
    // Compile-gate LazyVGrid(columns:[GridItem…]) + Image("name") + AsyncImage.
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.root = frame_node("root", "Grid", 300.0f, 200.0f, LayoutDirection::row);
    ir.root.layout.display = "grid";
    ir.root.layout.grid_template_columns = "repeat(2, 1fr)";
    ir.root.layout.column_gap = 6.0f;

    auto logo = frame_node("logo", "Logo", 48.0f, 48.0f, LayoutDirection::column);
    logo.type = "image";
    logo.attributes["srcAssetId"] = "brand_logo";
    ir.root.children.push_back(std::move(logo));

    auto hero = frame_node("hero", "Hero", 120.0f, 80.0f, LayoutDirection::column);
    hero.type = "image";
    hero.attributes["srcAssetId"] = "hero_img";
    ir.root.children.push_back(std::move(hero));

    IRAssetManifest manifest;
    IRAssetRef a; a.asset_id = "brand_logo"; a.original_uri = "logo.png"; a.local_path = "/tmp/logo.png";
    IRAssetRef b; b.asset_id = "hero_img"; b.original_uri = "https://cdn.example.com/hero.png";
    manifest.assets = {a, b};
    ir.asset_manifest = manifest;

    require_generated_swift_compiles(ir, "b5-grid-assets");
}

TEST_CASE("generate_pulp_swift handles the not-yet-supported B1 branches and compiles",
          "[view][import][swiftui][swiftc]") {
    // Exercise + compile-gate the degradation paths: an unsupported leaf widget
    // (a bare image leaf → Color.clear; assets are B5), an unsupported widget
    // that has children (lowered as a container), a dark-only color / dark-only
    // dimension+string (no light base), a non-hex color token (skipped), and an
    // unbound control (no label/name → visible placeholder, not a silent bind).
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.tokens.colors["overlayOnly.dark"] = "#0a0b0c";          // dark-only color
    ir.tokens.colors["brandNamed"] = "rebeccapurple";          // non-hex → skipped
    ir.tokens.dimensions["inset.dark"] = 6.0f;                 // dark-only dimension
    ir.tokens.strings["caption.dark"] = "nocturne";            // dark-only string
    ir.root = frame_node("root", "Panel", 320.0f, 240.0f, LayoutDirection::column);

    auto meter = frame_node("lvl", "Level", 80.0f, 16.0f, LayoutDirection::column);
    meter.audio_widget = AudioWidgetType::meter;               // B3: now a bound PulpMeter
    ir.root.children.push_back(std::move(meter));

    auto icon = frame_node("ico", "Icon", 24.0f, 24.0f, LayoutDirection::column);
    icon.type = "image";                                       // bare image leaf → Color.clear (B5)
    ir.root.children.push_back(std::move(icon));

    auto img = frame_node("logo", "Logo", 40.0f, 40.0f, LayoutDirection::column);
    img.type = "image";                                        // unsupported, but...
    img.children.push_back(text_node("cap", "v2", 9.0f, "#ffffff"));  // ...has a child → container
    ir.root.children.push_back(std::move(img));

    auto unbound = frame_node("", "", 50.0f, 50.0f, LayoutDirection::column);
    unbound.audio_widget = AudioWidgetType::knob;              // bound kind, but no name/label
    ir.root.children.push_back(std::move(unbound));

    const auto view = generate_pulp_swift(ir, ir.asset_manifest).view_source;
    INFO(view);
    REQUIRE(contains(view, "Color.clear"));                    // bare image leaf (assets are B5)
    REQUIRE(contains(view, "PulpMeter(parameter: p)"));        // B3: meter now binds
    REQUIRE(contains(view, "⚠︎ unbound"));                     // unbound control placeholder
    const auto theme = generate_pulp_swift(ir, ir.asset_manifest).theme_source;
    REQUIRE(contains(theme, "overlayOnly"));                   // dark-only color emitted
    REQUIRE(contains(theme, "insetDark"));                     // dark-only dimension companion
    REQUIRE_FALSE(contains(theme, "brandNamed"));              // non-hex color skipped

    require_generated_swift_compiles(ir, "degradation");       // all of it still compiles
}
