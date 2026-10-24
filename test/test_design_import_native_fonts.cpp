// A DesignIR's shipped fonts are registered when its tree is materialized, and
// every font-family stack that names them is pointed at the shipped faces.
#include <catch2/catch_test_macros.hpp>

#include <pulp/canvas/font_resolver.hpp>
#include <pulp/runtime/crypto.hpp>
#include <pulp/view/design_import.hpp>
#include <pulp/view/design_ir_io.hpp>
#include <pulp/view/widgets.hpp>

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

} // namespace

TEST_CASE("a shipped variable font is what an imported label resolves to",
          "[design-import][font-assets][native-fonts]") {
    REQUIRE(fs::exists(kVariableFont));
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
    bool reported = false;
    for (const auto& d : diagnostics)
        reported = reported || d.code == "font-asset-unavailable";
    CHECK(reported);
}

TEST_CASE("every captured label of the variable-font fixture resolves its captured face",
          "[design-import][font-assets][native-fonts]") {
    // The face check a Label makes before it reuses its captured line boxes:
    // the face its stack resolves to must be the face Chromium laid them out
    // with. With the shipped face registered it is, for both weights.
    std::ifstream in(kFixture / "design.ir.json", std::ios::binary);
    REQUIRE(in);
    const std::string json{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    const auto ir = parse_design_ir_json(json);
    REQUIRE_FALSE(ir.font_family_assets.empty());
    auto root = build_native_view_tree(ir, ir.asset_manifest, {.asset_base_directory = kFixture});
    REQUIRE(root);

    std::vector<std::pair<std::string, std::string>> captured; // text, basis face
    const auto walk = [&](auto&& self, const IRNode& node) -> void {
        if (node.text_layout_basis && !node.text_layout_basis->resolved_face.empty())
            captured.emplace_back(node.text_content, node.text_layout_basis->resolved_face);
        for (const auto& child : node.children)
            self(self, child);
    };
    walk(walk, ir.root);
    REQUIRE(captured.size() == 4);

    std::vector<Label*> labels;
    collect_labels(*root, labels);
    for (const auto& [text, basis_face] : captured) {
        INFO(text << " captured with " << basis_face);
        bool matched = false;
        for (auto* label : labels) {
            if (label->text() != text)
                continue;
            matched =
                matched || pulp::canvas::resolved_face_identity(
                               label->effective_font_family(),
                               static_cast<float>(label->effective_font_weight())) == basis_face;
        }
        CHECK(matched);
    }
}
