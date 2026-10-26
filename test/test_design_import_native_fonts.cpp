// A DesignIR's shipped fonts are registered when its tree is materialized, and
// every font-family stack that names them is pointed at the shipped faces.
#include <catch2/catch_test_macros.hpp>

#include "support/unique_temp_dir.hpp"
#include <pulp/canvas/bundled_fonts.hpp>
#include <pulp/canvas/font_resolver.hpp>
#include <pulp/runtime/crypto.hpp>
#include <pulp/view/design_import.hpp>
#include <pulp/view/design_ir_io.hpp>
#include <pulp/view/widgets.hpp>

#ifdef PULP_HAS_SKIA
#include "include/core/SkCanvas.h"
#include "include/core/SkFontArguments.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkSurface.h"
#include "include/core/SkTypeface.h"
#endif

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

using namespace pulp::view;
namespace fs = std::filesystem;

namespace {

const fs::path kVariableFont =
    fs::path(PULP_REPO_ROOT) / "external/fonts/FunnelDisplay-VariableFont_wght.ttf";
const fs::path kStaticFont = fs::path(PULP_REPO_ROOT) / "external/fonts/Inter-Regular.ttf";
const fs::path kFixture =
    fs::path(PULP_REPO_ROOT) / "test/fixtures/design-ir-observe/variable-font-label";

std::string file_sha256(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(in),
                                          std::istreambuf_iterator<char>()};
    return pulp::runtime::sha256_hex(bytes.data(), bytes.size());
}

// One text node naming `family` at `weight`, and `faces` shipped for it. Each
// asset is backed by its entry in `fonts`, or by `font`.
DesignIR shipped_font_design(const std::string& family, int weight,
                             const std::vector<IRFontAsset>& faces,
                             const fs::path& font = kVariableFont,
                             const std::map<std::string, fs::path>& fonts = {}) {
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.style.width = 200.0f;
    ir.root.style.height = 40.0f;
    IRNode text;
    text.type = "text";
    text.text_content = "SPECTR";
    text.style.font_family = "'" + family + "', sans-serif";
    text.style.font_size = 15.0f;
    text.style.font_weight = weight;
    text.style.width = 120.0f;
    text.style.height = 20.0f;
    ir.root.children.push_back(text);
    for (const auto& face : faces) {
        if (!ir.asset_manifest.resolve(face.asset_id)) {
            const auto it = fonts.find(face.asset_id);
            const fs::path& file = it != fonts.end() ? it->second : font;
            IRAssetRef asset;
            asset.asset_id = face.asset_id;
            asset.local_path = file.string();
            asset.mime = "font/ttf";
            if (fs::exists(file))
                asset.content_hash = file_sha256(file);
            ir.asset_manifest.assets.push_back(asset);
        }
        ir.font_family_assets.push_back(face);
    }
    return ir;
}

IRFontAsset face(const std::string& family, const std::string& asset_id,
                 const std::string& unicode_range = {}) {
    IRFontAsset f;
    f.family = family;
    f.weight = 300;
    f.weight_max = 800;
    f.font_style = "normal";
    f.unicode_range = unicode_range;
    f.asset_id = asset_id;
    return f;
}

void collect_labels(View& view, std::vector<Label*>& out) {
    if (auto* label = dynamic_cast<Label*>(&view))
        out.push_back(label);
    for (std::size_t i = 0; i < view.child_count(); ++i)
        if (auto* child = view.child_at(i))
            collect_labels(*child, out);
}

Label* only_label(View& root) {
    std::vector<Label*> labels;
    collect_labels(root, labels);
    return labels.size() == 1 ? labels.front() : nullptr;
}

// What this build's font resolver can answer, probed by registering the
// shipped files directly with the canvas, not through the importer. A build
// without text shaping (the sanitizer lanes) resolves nothing; a lite build
// resolves through CoreText. The tests below check the importer's aliases, so
// they skip, saying why, where the resolver itself cannot tell those faces
// apart, and fail where it can and the importer gets them wrong.
struct ResolverProbe {
    std::string variable_600; // the shipped variable face at 600
    std::string variable_300; // ... and at its 300 default
    std::string static_400;   // the shipped static face
};

const ResolverProbe& resolver_probe() {
    static const ResolverProbe probe = [] {
        const auto register_file = [](const fs::path& path, const std::string& family) {
            std::ifstream in(path, std::ios::binary);
            const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(in),
                                                  std::istreambuf_iterator<char>()};
            pulp::canvas::register_font(bytes.data(), bytes.size(), family);
        };
        register_file(kVariableFont, "Pulp Resolver Probe Variable");
        register_file(kStaticFont, "Pulp Resolver Probe Static");
        ResolverProbe out;
        out.variable_600 =
            pulp::canvas::resolved_face_identity("'Pulp Resolver Probe Variable'", 600.0f);
        out.variable_300 =
            pulp::canvas::resolved_face_identity("'Pulp Resolver Probe Variable'", 300.0f);
        out.static_400 =
            pulp::canvas::resolved_face_identity("'Pulp Resolver Probe Static'", 400.0f);
        return out;
    }();
    return probe;
}

// SKIPs (with the resolver's answers) unless a face registered from each
// shipped file resolves to that file's face.
void require_shipped_faces_resolve() {
    const auto& probe = resolver_probe();
    if (probe.variable_600.rfind("FunnelDisplay", 0) != 0 ||
        probe.static_400.rfind("Inter", 0) != 0)
        SKIP("this build's font resolver does not resolve a registered shipped face (variable "
             "font at 600: '"
             << probe.variable_600 << "', static font: '" << probe.static_400 << "')");
}

// SKIPs unless the resolver also instances the variable font's wght axis.
void require_variable_weights_resolve() {
    require_shipped_faces_resolve();
    const auto& probe = resolver_probe();
    if (probe.variable_600 == probe.variable_300)
        SKIP("this build's font resolver names the variable font's 600 and 300 instances alike ('"
             << probe.variable_600 << "')");
}

// `font` wrapped as a one-face TrueType collection ('ttcf'). A collection's
// table offsets count from the start of the file, so each moves by the
// 16-byte collection header (tag, version, face count, the one face offset).
std::vector<std::uint8_t> as_collection(const fs::path& font) {
    std::ifstream in(font, std::ios::binary);
    std::vector<std::uint8_t> face{std::istreambuf_iterator<char>(in),
                                   std::istreambuf_iterator<char>()};
    const auto u16 = [&](std::size_t at) {
        return static_cast<std::size_t>(face[at] << 8 | face[at + 1]);
    };
    const std::size_t tables = u16(4);
    for (std::size_t i = 0; i < tables; ++i) {
        const std::size_t at = 12 + i * 16 + 8;
        std::uint32_t offset = static_cast<std::uint32_t>(face[at]) << 24 |
                               static_cast<std::uint32_t>(face[at + 1]) << 16 |
                               static_cast<std::uint32_t>(face[at + 2]) << 8 | face[at + 3];
        offset += 16;
        for (int b = 0; b < 4; ++b)
            face[at + static_cast<std::size_t>(b)] =
                static_cast<std::uint8_t>(offset >> (24 - 8 * b));
    }
    std::vector<std::uint8_t> out{'t', 't', 'c', 'f', 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 16};
    out.insert(out.end(), face.begin(), face.end());
    return out;
}

#ifdef PULP_HAS_SKIA
sk_sp<SkTypeface> resolved_typeface(const std::string& stack, float weight) {
    pulp::canvas::FontOptions options;
    options.weight = weight;
    options.family_stack = pulp::canvas::split_css_font_family_list(stack);
    return pulp::canvas::FontResolver::instance().resolve_family_list(options).typeface;
}

// The face's family as its own name table spells it, and the wght coordinate
// it is instanced at (its default when no coordinate is set). Both read the
// same on every platform, unlike the PostScript name a port synthesizes for a
// variation instance.
std::string family_of(const SkTypeface& face) {
    SkString name;
    face.getFamilyName(&name);
    return std::string(name.c_str(), name.size());
}

float wght_of(const SkTypeface& face) {
    constexpr SkFourByteTag kWght = SkSetFourByteTag('w', 'g', 'h', 't');
    std::vector<SkFontArguments::VariationPosition::Coordinate> position(8);
    const int count = face.getVariationDesignPosition(SkSpan(position));
    for (int i = 0; i < count && i < static_cast<int>(position.size()); ++i)
        if (position[static_cast<std::size_t>(i)].axis == kWght)
            return position[static_cast<std::size_t>(i)].value;
    return -1.0f;
}

// Ink (sum of 255 - red) of `text` drawn black on white with `face`.
double ink_of(const sk_sp<SkTypeface>& face, const char* text) {
    auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(320, 64));
    surface->getCanvas()->clear(SK_ColorWHITE);
    SkPaint paint;
    paint.setColor(SK_ColorBLACK);
    surface->getCanvas()->drawString(text, 8, 44, SkFont(face, 32), paint);
    SkPixmap pixels;
    if (!surface->peekPixels(&pixels))
        return 0.0;
    double ink = 0;
    for (int y = 0; y < pixels.height(); ++y)
        for (int x = 0; x < pixels.width(); ++x)
            ink += 255 - SkColorGetR(pixels.getColor(x, y));
    return ink;
}
#endif

} // namespace

TEST_CASE("a shipped variable font is what an imported label resolves to",
          "[design-import][font-assets][native-fonts]") {
    REQUIRE(fs::exists(kVariableFont));
    require_variable_weights_resolve();
    const std::string family = "Pulp Shipped Variable";
    auto ir = shipped_font_design(family, 600, {face(family, "font-variable")});
    auto root = build_native_view_tree(ir, ir.asset_manifest);
    REQUIRE(root);
    auto* label = only_label(*root);
    REQUIRE(label != nullptr);

    const std::string stack = label->effective_font_family();
    INFO("stack " << stack);
    // The family is replaced by its private alias, so no same-named host face
    // can stand in for it; the stack's own fallback stays.
    CHECK(stack.find(family + " [") != std::string::npos);
    CHECK(stack.find("'" + family + "'") == std::string::npos);
    CHECK(stack.find("sans-serif") != std::string::npos);

    const auto shipped = pulp::canvas::resolved_face_identity(stack, 600.0f);
    INFO("face " << shipped);
    CHECK(shipped.rfind("FunnelDisplay", 0) == 0);
    CHECK(shipped != pulp::canvas::resolved_face_identity("sans-serif", 600.0f));
    // The wght axis is instanced at 600, not the 300 default face drawn with a
    // synthesized weight.
    CHECK(shipped != pulp::canvas::resolved_face_identity(stack, 300.0f));
    // The probe's own face, registered without the importer, is the same face.
    CHECK(shipped == resolver_probe().variable_600);
#if defined(__APPLE__)
    // CoreText names the instance the way Chromium reported this font at 600
    // in the variable-font-label fixture's capture.
    CHECK(shipped == "FunnelDisplay-Light_SemiBold");
#endif
}

TEST_CASE("a family split by unicode-range checks its last-defined subset first",
          "[design-import][font-assets][native-fonts]") {
    // Two subsets of one family in different files. The stack lists the
    // last-defined first, so that is the face the family resolves to.
    require_shipped_faces_resolve();
    const std::string family = "Pulp Shipped Subsets";
    const std::map<std::string, fs::path> fonts{{"font-cyrillic", kStaticFont},
                                                {"font-latin", kVariableFont}};
    const auto first_face = [&](const std::vector<IRFontAsset>& faces) {
        auto ir = shipped_font_design(family, 400, faces, kVariableFont, fonts);
        auto root = build_native_view_tree(ir, ir.asset_manifest);
        REQUIRE(root);
        auto* label = only_label(*root);
        REQUIRE(label != nullptr);
        const auto stack = pulp::canvas::split_css_font_family_list(label->effective_font_family());
        REQUIRE(stack.size() == 3);
        CHECK(stack[0] != stack[1]);
        CHECK(stack[0].find(family + " [") != std::string::npos);
        CHECK(stack[1].find(family + " [") != std::string::npos);
        CHECK(stack[2] == "sans-serif");
        return pulp::canvas::resolved_face_identity(label->effective_font_family(), 400.0f);
    };
    const auto cyrillic = face(family, "font-cyrillic", "U+0400-045F");
    const auto latin = face(family, "font-latin", "U+0000-00FF");
    CHECK(first_face({cyrillic, latin}).rfind("FunnelDisplay", 0) == 0);
    CHECK(first_face({latin, cyrillic}).rfind("Inter", 0) == 0);
}

TEST_CASE("a shipped font that cannot be read leaves the stack alone and says so",
          "[design-import][font-assets][native-fonts]") {
    const std::string family = "Pulp Shipped Missing";
    std::vector<ImportDiagnostic> diagnostics;
    SECTION("missing file") {
        auto ir = shipped_font_design(family, 600, {face(family, "font-missing")},
                                      fs::path(PULP_REPO_ROOT) / "no/such/font.ttf");
        auto root =
            build_native_view_tree(ir, ir.asset_manifest, {.diagnostics_out = &diagnostics});
        REQUIRE(root);
        CHECK(only_label(*root)->effective_font_family() == "'" + family + "', sans-serif");
    }
    SECTION("bytes that are not the ones the manifest recorded") {
        auto ir = shipped_font_design(family, 600, {face(family, "font-stale")});
        ir.asset_manifest.assets[0].content_hash = std::string(64, '0');
        auto root =
            build_native_view_tree(ir, ir.asset_manifest, {.diagnostics_out = &diagnostics});
        REQUIRE(root);
        CHECK(only_label(*root)->effective_font_family() == "'" + family + "', sans-serif");
    }
    SECTION("a file that is not a TrueType, OpenType or WOFF2 font") {
        // A font collection holding the shipped face: the platform managers
        // would load it, but it is not a container a capture ships.
        const auto collection = pulp::test::unique_temp_path("pulp-shipped-font", ".ttc");
        {
            std::ofstream out(collection, std::ios::binary);
            const auto bytes = as_collection(kVariableFont);
            out.write(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size()));
        }
        auto ir = shipped_font_design(family, 600, {face(family, "font-collection")}, collection);
        REQUIRE_FALSE(ir.asset_manifest.assets[0].content_hash.empty());
        auto root =
            build_native_view_tree(ir, ir.asset_manifest, {.diagnostics_out = &diagnostics});
        std::error_code ec;
        fs::remove(collection, ec);
        REQUIRE(root);
        CHECK(only_label(*root)->effective_font_family() == "'" + family + "', sans-serif");
    }
    SECTION("a font file larger than any real face") {
        const auto big = pulp::test::unique_temp_path("pulp-shipped-font", ".ttf");
        // The shipped face padded past the cap: loadable, but never read.
        auto ir = shipped_font_design(family, 600, {face(family, "font-oversized")}, big);
        fs::copy_file(kVariableFont, big);
        fs::resize_file(big, 32u * 1024u * 1024u + 1u);
        auto root =
            build_native_view_tree(ir, ir.asset_manifest, {.diagnostics_out = &diagnostics});
        std::error_code ec;
        fs::remove(big, ec);
        REQUIRE(root);
        CHECK(only_label(*root)->effective_font_family() == "'" + family + "', sans-serif");
    }
    bool reported = false;
    for (const auto& d : diagnostics)
        reported = reported || d.code == "font-asset-unavailable";
    CHECK(reported);
}

TEST_CASE("a shipped font's alias changes when its file's bytes do",
          "[design-import][font-assets][native-fonts]") {
    // Same family, range and asset id; only the file behind the id differs.
    require_shipped_faces_resolve();
    const std::string family = "Pulp Shipped Rewritten";
    const auto stack_with = [&](const fs::path& font) {
        auto ir = shipped_font_design(family, 400, {face(family, "font-rewritten")}, font);
        auto root = build_native_view_tree(ir, ir.asset_manifest);
        REQUIRE(root);
        auto* label = only_label(*root);
        REQUIRE(label != nullptr);
        return label->effective_font_family();
    };
    const auto variable = stack_with(kVariableFont);
    const auto rewritten = stack_with(kStaticFont);
    CHECK(variable.find(family + " [") != std::string::npos);
    CHECK(rewritten.find(family + " [") != std::string::npos);
    CHECK(variable != rewritten);
    // The alias still names the face the file holds now, not the old one.
    CHECK(pulp::canvas::resolved_face_identity(rewritten, 400.0f).rfind("Inter", 0) == 0);
    CHECK(stack_with(kVariableFont) == variable);
}

TEST_CASE("every captured label of the variable-font fixture resolves its captured face",
          "[design-import][font-assets][native-fonts]") {
    // The face check a Label makes before it reuses its captured line boxes:
    // the face its stack resolves to must be the face Chromium laid them out
    // with. With the shipped face registered it is, for both weights.
    require_variable_weights_resolve();
    std::ifstream in(kFixture / "design.ir.json", std::ios::binary);
    REQUIRE(in);
    const std::string json{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    const auto ir = parse_design_ir_json(json);
    REQUIRE_FALSE(ir.font_family_assets.empty());
    auto root = build_native_view_tree(ir, ir.asset_manifest, {.asset_base_directory = kFixture});
    REQUIRE(root);

    struct Captured {
        std::string text;
        std::string basis_face;
        int weight = 400;
    };
    std::vector<Captured> captured;
    const auto walk = [&](auto&& self, const IRNode& node) -> void {
        if (node.text_layout_basis && !node.text_layout_basis->resolved_face.empty())
            captured.push_back({node.text_content, node.text_layout_basis->resolved_face,
                                node.style.font_weight.value_or(400)});
        for (const auto& child : node.children)
            self(self, child);
    };
    walk(walk, ir.root);
    REQUIRE(captured.size() == 4);

    std::vector<Label*> labels;
    collect_labels(*root, labels);
    for (const auto& [text, basis_face, weight] : captured) {
        INFO(text << " captured at " << weight << " with " << basis_face);
        std::size_t seen = 0;
        for (auto* label : labels) {
            if (label->text() != text)
                continue;
            ++seen;
            const auto stack = label->effective_font_family();
            const auto label_weight = static_cast<float>(label->effective_font_weight());
            INFO("stack " << stack << " resolves to "
                          << pulp::canvas::resolved_face_identity(stack, label_weight));
            CHECK(label_weight == static_cast<float>(weight));
#ifdef PULP_HAS_SKIA
            // On every platform: the shipped family, instanced at the captured
            // weight.
            const auto face = resolved_typeface(stack, label_weight);
            REQUIRE(face);
            // Name ID 1 is "Funnel Display Light", ID 16 "Funnel Display";
            // ports differ in which they report.
            CHECK(family_of(*face).rfind("Funnel Display", 0) == 0);
            CHECK(std::abs(wght_of(*face) - static_cast<float>(weight)) < 0.5f);
#endif
#if defined(__APPLE__)
            // CoreText names the instance exactly as Chromium on macOS
            // reported it, which is what lets the Label reuse its captured
            // line boxes. FreeType synthesizes a different PostScript name
            // for the same instance.
            CHECK(pulp::canvas::resolved_face_identity(stack, label_weight) == basis_face);
#endif
        }
        CHECK(seen > 0);
    }
#ifdef PULP_HAS_SKIA
    // The coordinate reaches the outlines: the 600 instance draws heavier.
    REQUIRE_FALSE(labels.empty());
    const auto stack = labels.front()->effective_font_family();
    const double semibold = ink_of(resolved_typeface(stack, 600.0f), "SPECTR Sweep");
    const double regular = ink_of(resolved_typeface(stack, 400.0f), "SPECTR Sweep");
    REQUIRE(regular > 0.0);
    CHECK(semibold > regular * 1.1);
#endif
}

TEST_CASE("a shipped face's declared weight range is carried by its alias",
          "[design-import][font-assets][native-fonts][synthetic-bold]") {
    // Chromium synthesizes bold against the rule's declared range, so the
    // alias the stack names must carry that range, not the bare family.
    require_shipped_faces_resolve();
    const auto alias_of = [](const std::string& family, int weight, int weight_max) {
        auto shipped = face(family, "font-" + family);
        shipped.weight = weight;
        shipped.weight_max = weight_max;
        auto ir = shipped_font_design(family, 700, {shipped}, kStaticFont);
        auto root = build_native_view_tree(ir, ir.asset_manifest);
        REQUIRE(root);
        auto* label = only_label(*root);
        REQUIRE(label != nullptr);
        const auto stack = pulp::canvas::split_css_font_family_list(label->effective_font_family());
        REQUIRE_FALSE(stack.empty());
        const auto alias = pulp::canvas::parse_css_font_family(stack.front()).name;
        REQUIRE(alias.find(family + " [") == 0);
        return alias;
    };
    const auto range = alias_of("Pulp Declared Range", 400, 700);
    CHECK(pulp::canvas::declared_face_synthesizes_bold(range, 700) == false);
    const auto single = alias_of("Pulp Declared Single", 400, 0);
    CHECK(pulp::canvas::declared_face_synthesizes_bold(single, 700) == true);
    // The bare family is not what text names, so it declares nothing.
    CHECK_FALSE(pulp::canvas::has_declared_font_face_weights("Pulp Declared Range"));
}

TEST_CASE("two families binding one file each keep their own declared range",
          "[design-import][font-assets][native-fonts][synthetic-bold]") {
    // A page may bind one Regular file under two families, declared 400 700
    // and 400. Each family's alias carries its own rule's range.
    require_shipped_faces_resolve();
    auto wide = face("Pulp Shared Wide", "font-shared");
    wide.weight = 400;
    wide.weight_max = 700;
    auto narrow = face("Pulp Shared Narrow", "font-shared");
    narrow.weight = 400;
    narrow.weight_max = 0;
    const auto alias_for = [&](const std::string& family) {
        auto ir = shipped_font_design(family, 700, {wide, narrow}, kStaticFont);
        auto root = build_native_view_tree(ir, ir.asset_manifest);
        REQUIRE(root);
        auto* label = only_label(*root);
        REQUIRE(label != nullptr);
        const auto stack = pulp::canvas::split_css_font_family_list(label->effective_font_family());
        REQUIRE_FALSE(stack.empty());
        return pulp::canvas::parse_css_font_family(stack.front()).name;
    };
    CHECK(pulp::canvas::declared_face_synthesizes_bold(alias_for("Pulp Shared Wide"), 700) ==
          false);
    CHECK(pulp::canvas::declared_face_synthesizes_bold(alias_for("Pulp Shared Narrow"), 700) ==
          true);
}
