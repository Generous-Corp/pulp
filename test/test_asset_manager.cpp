#include <catch2/catch_test_macros.hpp>
#include <pulp/view/asset_manager.hpp>
#include <pulp/view/theme.hpp>
#include <chrono>
#include <fstream>
#include <filesystem>
#include <utility>

using namespace pulp::view;

namespace {

std::filesystem::path make_temp_theme_path(const char* stem) {
    auto unique = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    return std::filesystem::temp_directory_path() / (std::string(stem) + "-" + unique + ".json");
}

std::filesystem::path make_temp_asset_path(const char* stem, const char* suffix) {
    auto unique = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    return std::filesystem::temp_directory_path() / (std::string(stem) + "-" + unique + suffix);
}

std::vector<uint8_t> make_png_header(uint32_t width, uint32_t height) {
    return {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
        0x00, 0x00, 0x00, 0x0D,
        0x49, 0x48, 0x44, 0x52,
        static_cast<uint8_t>((width >> 24) & 0xFF),
        static_cast<uint8_t>((width >> 16) & 0xFF),
        static_cast<uint8_t>((width >> 8) & 0xFF),
        static_cast<uint8_t>(width & 0xFF),
        static_cast<uint8_t>((height >> 24) & 0xFF),
        static_cast<uint8_t>((height >> 16) & 0xFF),
        static_cast<uint8_t>((height >> 8) & 0xFF),
        static_cast<uint8_t>(height & 0xFF),
        0x08, 0x06, 0x00, 0x00, 0x00,
    };
}

void write_bytes(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}

} // namespace

// ── Embedded Resources ──────────────────────────────────────────────────────

TEST_CASE("AssetManager register and query embedded", "[view][assets]") {
    auto& mgr = AssetManager::instance();

    static const uint8_t test_data[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    mgr.register_embedded("test_icon", test_data, sizeof(test_data));

    REQUIRE(mgr.has_embedded("test_icon"));
    REQUIRE_FALSE(mgr.has_embedded("nonexistent"));
}

// ── Cache Management ────────────────────────────────────────────────────────

TEST_CASE("AssetManager cache clear", "[view][assets]") {
    auto& mgr = AssetManager::instance();
    mgr.clear_cache();
    REQUIRE(mgr.cache_usage() == 0);
    REQUIRE(mgr.cache_count() == 0);
}

TEST_CASE("AssetManager max cache size", "[view][assets]") {
    auto& mgr = AssetManager::instance();
    size_t old_max = mgr.max_cache_size();

    mgr.set_max_cache_size(1024 * 1024); // 1 MB
    REQUIRE(mgr.max_cache_size() == 1024 * 1024);

    mgr.set_max_cache_size(old_max); // Restore
}

TEST_CASE("AssetManager LRU eviction", "[view][assets]") {
    auto& mgr = AssetManager::instance();
    mgr.clear_cache();

    // Set very small cache
    size_t old_max = mgr.max_cache_size();
    mgr.set_max_cache_size(100);

    // Register multiple large embedded resources
    static const uint8_t big1[50] = {};
    static const uint8_t big2[50] = {};
    static const uint8_t big3[50] = {};
    mgr.register_embedded("big1", big1, 50);
    mgr.register_embedded("big2", big2, 50);
    mgr.register_embedded("big3", big3, 50);

    // Load them — should trigger eviction
    mgr.load_blob_embedded("big1");
    mgr.load_blob_embedded("big2");
    mgr.load_blob_embedded("big3");

    // Cache should not exceed max
    REQUIRE(mgr.cache_usage() <= 100);

    mgr.set_max_cache_size(old_max);
    mgr.clear_cache();
}

// ── Shader Management ───────────────────────────────────────────────────────

TEST_CASE("AssetManager register and retrieve shader", "[view][assets]") {
    auto& mgr = AssetManager::instance();

    mgr.register_shader("test_effect", "uniform float time; void main() {}");
    auto shader = mgr.shader("test_effect");
    REQUIRE(shader.valid());
    REQUIRE(shader.source.find("uniform") != std::string::npos);
}

TEST_CASE("AssetManager missing shader returns invalid", "[view][assets]") {
    auto& mgr = AssetManager::instance();
    auto shader = mgr.shader("nonexistent_shader_xyz");
    REQUIRE_FALSE(shader.valid());
}

TEST_CASE("AssetManager embedded shader loads into shader cache", "[view][assets]") {
    auto& mgr = AssetManager::instance();
    mgr.clear_cache();

    static const uint8_t source[] = "void main() { }";
    mgr.register_embedded("embedded_shader_asset", source, sizeof(source) - 1);

    auto shader = mgr.load_shader_embedded("embedded_shader_asset");
    REQUIRE(shader.valid());
    REQUIRE(shader.source == "void main() { }");

    auto cached = mgr.shader("embedded_shader_asset");
    REQUIRE(cached.valid());
    REQUIRE(cached.source == shader.source);
}

TEST_CASE("AssetManager missing embedded shader reports error", "[view][assets]") {
    auto& mgr = AssetManager::instance();
    auto shader = mgr.load_shader_embedded("missing_embedded_shader_asset");
    REQUIRE_FALSE(shader.valid());
    REQUIRE(shader.error.find("missing_embedded_shader_asset") != std::string::npos);
}

// ── Font System ─────────────────────────────────────────────────────────────

TEST_CASE("AssetManager font family registration", "[view][assets]") {
    auto& mgr = AssetManager::instance();

    static const uint8_t fake_font[] = {0x00, 0x01, 0x00, 0x00}; // Fake TTF header
    mgr.register_embedded("inter_ttf", fake_font, sizeof(fake_font));
    mgr.register_font_family("Inter", "inter_ttf");

    auto font = mgr.font_for_family("Inter");
    REQUIRE(font.valid());
    REQUIRE(font.family_name == "Inter");
}

TEST_CASE("AssetManager font fallback chain", "[view][assets]") {
    auto& mgr = AssetManager::instance();

    static const uint8_t fallback_font[] = {0x00, 0x01};
    mgr.register_embedded("fallback_ttf", fallback_font, sizeof(fallback_font));
    mgr.register_font_family("Fallback", "fallback_ttf");
    mgr.set_font_fallback({"Fallback"});

    // Requesting unknown family should fall back
    auto font = mgr.font_for_family("UnknownFont");
    REQUIRE(font.valid());
    REQUIRE(font.family_name == "Fallback");
}

TEST_CASE("AssetManager missing embedded font and blob return invalid data", "[view][assets]") {
    auto& mgr = AssetManager::instance();

    auto font = mgr.load_font_embedded("missing_embedded_font_asset");
    REQUIRE_FALSE(font.valid());

    auto blob = mgr.load_blob_embedded("missing_embedded_blob_asset");
    REQUIRE_FALSE(blob.valid());
}

TEST_CASE("AssetManager fallback skips families without embedded data", "[view][assets]") {
    auto& mgr = AssetManager::instance();

    static const uint8_t fallback_font[] = {0x00, 0x01, 0x02};
    mgr.register_font_family("MissingFamily", "not_registered_font_asset");
    mgr.register_embedded("present_fallback_font", fallback_font, sizeof(fallback_font));
    mgr.register_font_family("PresentFallback", "present_fallback_font");
    mgr.set_font_fallback({"MissingFamily", "PresentFallback"});

    auto font = mgr.font_for_family("MissingFamily");
    REQUIRE(font.valid());
    REQUIRE(font.family_name == "PresentFallback");
}

// ── Image Loading ───────────────────────────────────────────────────────────

TEST_CASE("AssetManager load image from memory with PNG header", "[view][assets]") {
    // Minimal valid-ish PNG header for testing dimension parsing
    // Real PNG: signature (8) + IHDR chunk (4 len + 4 type + 13 data + 4 crc = 25)
    static const uint8_t mini_png[] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,  // PNG signature
        0x00, 0x00, 0x00, 0x0D,                              // IHDR length
        0x49, 0x48, 0x44, 0x52,                              // "IHDR"
        0x00, 0x00, 0x00, 0x10,                              // width = 16
        0x00, 0x00, 0x00, 0x08,                              // height = 8
        0x08, 0x06, 0x00, 0x00, 0x00,                        // depth/color/etc
    };

    auto& mgr = AssetManager::instance();
    auto img = mgr.load_image_from_memory(mini_png, sizeof(mini_png));
    REQUIRE(img.width == 16);
    REQUIRE(img.height == 8);
}

TEST_CASE("AssetManager load image from embedded", "[view][assets]") {
    auto& mgr = AssetManager::instance();

    static const uint8_t png[] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
        0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
        0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x04,
        0x08, 0x06, 0x00, 0x00, 0x00,
    };
    mgr.register_embedded("test_img", png, sizeof(png));

    auto img = mgr.load_image_embedded("test_img");
    REQUIRE(img.valid());
    REQUIRE(img.width == 4);
    REQUIRE(img.height == 4);
}

// ── File I/O ────────────────────────────────────────────────────────────────

TEST_CASE("AssetManager load nonexistent file returns empty", "[view][assets]") {
    auto& mgr = AssetManager::instance();
    auto img = mgr.load_image("/tmp/pulp_test_nonexistent_file.png");
    REQUIRE_FALSE(img.valid());
}

TEST_CASE("AssetManager async image load calls callback for missing file", "[view][assets]") {
    auto& mgr = AssetManager::instance();
    bool called = false;
    ImageData callback_image;

    mgr.load_image_async("/tmp/pulp_test_async_missing_file.png",
                         [&](ImageData img) {
                             called = true;
                             callback_image = std::move(img);
                         });

    REQUIRE(called);
    REQUIRE_FALSE(callback_image.valid());
}

// ── Data URI ────────────────────────────────────────────────────────────────

TEST_CASE("AssetManager data URI base64 decode", "[view][assets]") {
    auto& mgr = AssetManager::instance();
    // Base64 of the PNG signature bytes: iVBORw0KGgo=
    // This is too short for a real PNG but tests the decode path
    auto img = mgr.load_image_from_data_uri("data:image/png;base64,iVBORw0KGgo=");
    // Should decode the base64 and attempt PNG parse
    // Won't be a valid image but should not crash
    // The decoded bytes would be the PNG signature (8 bytes)
}

TEST_CASE("AssetManager rejects malformed data URIs", "[view][assets]") {
    auto& mgr = AssetManager::instance();

    auto missing_comma = mgr.load_image_from_data_uri("data:image/png;base64");
    REQUIRE_FALSE(missing_comma.valid());

    auto no_payload = mgr.load_image_from_data_uri("data:image/png;base64,");
    REQUIRE_FALSE(no_payload.valid());
}

// ── Display Scale ───────────────────────────────────────────────────────────

TEST_CASE("AssetManager display scale", "[view][assets]") {
    auto& mgr = AssetManager::instance();
    mgr.set_display_scale(2.0f);
    REQUIRE(mgr.display_scale() == 2.0f);
    mgr.set_display_scale(1.0f); // Restore
}

TEST_CASE("AssetManager display scale resolves @2x file before base image", "[view][assets]") {
    auto& mgr = AssetManager::instance();
    mgr.clear_cache();

    auto base = make_temp_asset_path("pulp-test-icon", ".png");
    auto retina = base;
    retina.replace_filename(base.stem().string() + "@2x" + base.extension().string());

    write_bytes(base, make_png_header(1, 1));
    write_bytes(retina, make_png_header(2, 2));

    mgr.set_display_scale(2.0f);
    auto img = mgr.load_image(base.string());
    REQUIRE(img.valid());
    REQUIRE(img.width == 2);
    REQUIRE(img.height == 2);

    mgr.set_display_scale(1.0f);
    mgr.clear_cache();

    std::error_code ec;
    std::filesystem::remove(base, ec);
    std::filesystem::remove(retina, ec);
}

// ── Blob Loading ────────────────────────────────────────────────────────────

TEST_CASE("AssetManager blob from embedded", "[view][assets]") {
    auto& mgr = AssetManager::instance();

    static const uint8_t json_data[] = "{\"key\":\"value\"}";
    mgr.register_embedded("test_json", json_data, sizeof(json_data) - 1);

    auto blob = mgr.load_blob_embedded("test_json");
    REQUIRE(blob.valid());
    REQUIRE(blob.data.size() == sizeof(json_data) - 1);
}

// ── Theme Import/Export (file-based) ────────────────────────────────────────

TEST_CASE("Theme save and load from file", "[view][theme][io]") {
    auto original = Theme::dark();
    auto path = make_temp_theme_path("pulp-test-theme");

    REQUIRE(original.save_to_file(path.string()));

    auto loaded = Theme::load_from_file(path.string());
    REQUIRE(loaded.color("bg.primary").has_value());

    auto orig_bg = original.color("bg.primary").value();
    auto load_bg = loaded.color("bg.primary").value();
    REQUIRE(orig_bg == load_bg);

    // Cleanup
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("Theme load from nonexistent file returns empty", "[view][theme][io]") {
    auto theme = Theme::load_from_file(make_temp_theme_path("pulp-nonexistent-theme").string());
    REQUIRE(theme.colors.empty());
}

TEST_CASE("Theme validation - missing tokens", "[view][theme][validation]") {
    Theme empty;
    auto missing = empty.missing_tokens();
    REQUIRE(missing.size() == Theme::required_color_tokens().size());
    REQUIRE_FALSE(empty.is_complete());
}

TEST_CASE("Theme validation - complete theme", "[view][theme][validation]") {
    auto theme = Theme::dark();
    REQUIRE(theme.is_complete());
    REQUIRE(theme.missing_tokens().empty());
}

TEST_CASE("Theme fill_from fills missing tokens", "[view][theme][validation]") {
    Theme partial;
    partial.colors["bg.primary"] = color_from_hex(0xFF0000);

    partial.fill_from(Theme::dark());

    REQUIRE(partial.is_complete());
    // Our override should be preserved
    REQUIRE(partial.color("bg.primary")->r8() == 0xFF);
    // Missing tokens should come from dark
    REQUIRE(partial.color("text.primary").has_value());
}
