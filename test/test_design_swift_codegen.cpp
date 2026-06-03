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
    // Adversarial token names (Codex review): a Swift keyword must be
    // backtick-escaped, and two names that camel-case to the same identifier
    // must be de-duplicated — otherwise the generated PulpTheme.swift fails to
    // compile. The swiftc gate below proves the escaping/dedup works.
    ir.tokens.colors["default"] = "#123456";          // Swift keyword → `default`
    ir.tokens.colors["foo.bar"] = "#111111";          // camels to fooBar
    ir.tokens.colors["foo-bar"] = "#222222";          // also fooBar → must dedup
    // Keyword dimension/string tokens WITH .dark overrides: the dark companion
    // must escape the full id ("switchDark"), not append Dark to the escaped
    // base (the invalid `` `switch`Dark ``). (Codex review #2.)
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

TEST_CASE("generate_pulp_swift emits a minimal name-keyed binding manifest",
          "[view][import][swiftui]") {
    const auto ir = build_swift_fixture();
    const auto manifest = generate_pulp_swift(ir, ir.asset_manifest).binding_manifest;
    INFO(manifest);

    REQUIRE(contains(manifest, "\"schema\": \"pulp-native-swiftui-binding-manifest-v1\""));
    REQUIRE(contains(manifest, "\"strategy\": \"pulp_parameter_name_exact\""));
    REQUIRE(contains(manifest, "\"native_primitive\": \"knob\""));
    // resolve_name is the display-name matched against PulpParameter.name;
    // canonical_key preserves pulpParamKey as metadata for B4.
    REQUIRE(contains(manifest, "\"resolve_name\": \"Drive\""));
    REQUIRE(contains(manifest, "\"canonical_key\": \"Drive\""));
    REQUIRE(contains(manifest, "\"native_primitive\": \"fader\""));
    REQUIRE(contains(manifest, "\"native_primitive\": \"toggle_button\""));
    REQUIRE(contains(manifest, "\"resolve_name\": \"Bypass\""));
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
        SUCCEED("skipped: Swift toolchain/SDK unavailable");
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

TEST_CASE("generate_pulp_swift handles the not-yet-supported B1 branches and compiles",
          "[view][import][swiftui][swiftc]") {
    // Exercise + compile-gate the degradation paths: an unsupported leaf widget
    // (meter → Color.clear), an unsupported widget that has children (lowered as
    // a container), a dark-only color / dark-only dimension+string (no light
    // base), a non-hex color token (skipped), and an unbound control (no
    // label/name → visible placeholder, not a silent bind).
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.tokens.colors["overlayOnly.dark"] = "#0a0b0c";          // dark-only color
    ir.tokens.colors["brandNamed"] = "rebeccapurple";          // non-hex → skipped
    ir.tokens.dimensions["inset.dark"] = 6.0f;                 // dark-only dimension
    ir.tokens.strings["caption.dark"] = "nocturne";            // dark-only string
    ir.root = frame_node("root", "Panel", 320.0f, 240.0f, LayoutDirection::column);

    auto meter = frame_node("lvl", "Level", 80.0f, 16.0f, LayoutDirection::column);
    meter.audio_widget = AudioWidgetType::meter;               // unsupported leaf in B1
    ir.root.children.push_back(std::move(meter));

    auto img = frame_node("logo", "Logo", 40.0f, 40.0f, LayoutDirection::column);
    img.type = "image";                                        // unsupported, but...
    img.children.push_back(text_node("cap", "v2", 9.0f, "#ffffff"));  // ...has a child → container
    ir.root.children.push_back(std::move(img));

    auto unbound = frame_node("", "", 50.0f, 50.0f, LayoutDirection::column);
    unbound.audio_widget = AudioWidgetType::knob;              // bound kind, but no name/label
    ir.root.children.push_back(std::move(unbound));

    const auto view = generate_pulp_swift(ir, ir.asset_manifest).view_source;
    INFO(view);
    REQUIRE(contains(view, "Color.clear"));                    // unsupported meter leaf
    REQUIRE(contains(view, "⚠︎ unbound"));                     // unbound control placeholder
    const auto theme = generate_pulp_swift(ir, ir.asset_manifest).theme_source;
    REQUIRE(contains(theme, "overlayOnly"));                   // dark-only color emitted
    REQUIRE(contains(theme, "insetDark"));                     // dark-only dimension companion
    REQUIRE_FALSE(contains(theme, "brandNamed"));              // non-hex color skipped

    require_generated_swift_compiles(ir, "degradation");       // all of it still compiles
}
