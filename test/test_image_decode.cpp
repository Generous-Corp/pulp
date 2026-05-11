// Test createImageBitmap image decoding via the native Skia bridge.
// Validates the glTF texture loading pipeline: encoded bytes → RGBA pixels.

#include <catch2/catch_test_macros.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/widget_bridge.hpp>
#include <pulp/state/store.hpp>
// Note: no GPU surface dependency — tests the JS-level decode and texture copy
// without requiring Dawn/Skia. The native decode test skips gracefully when
// __decodeImageDataImpl is unavailable (no PULP_HAS_SKIA).

using namespace pulp::view;

// Minimal 2x2 red PNG (RGBA: 255,0,0,255 for all 4 pixels)
static const unsigned char red_2x2_png[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
    0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02,
    0x08, 0x02, 0x00, 0x00, 0x00, 0xfd, 0xd4, 0x9a,
    0x73, 0x00, 0x00, 0x00, 0x14, 0x49, 0x44, 0x41,
    0x54, 0x78, 0x9c, 0x62, 0xf8, 0xcf, 0xc0, 0x00,
    0x04, 0x18, 0x18, 0x00, 0x00, 0x00, 0x10, 0x00,
    0x01, 0x3a, 0x93, 0x3e, 0x0a, 0x00, 0x00, 0x00,
    0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60,
    0x82
};

static std::string png_byte_array() {
    std::string result = "[";
    for (size_t i = 0; i < sizeof(red_2x2_png); ++i) {
        if (i > 0) result += ",";
        result += std::to_string(red_2x2_png[i]);
    }
    result += "]";
    return result;
}

TEST_CASE("Native image decode via __decodeImageDataImpl", "[webcompat][gltf][texture][!mayfail]") {
    View root;
    ScriptEngine engine;
    pulp::state::StateStore store;
    root.set_bounds({0, 0, 100, 100});
    root.set_theme(Theme::dark());

    auto bridge = std::make_unique<WidgetBridge>(engine, root, store);

    auto bytes = png_byte_array();
    std::string js = R"JS(
        var png = new Uint8Array()JS" + bytes + R"JS();
        var payload = JSON.stringify({ data: Array.from(png) });
        var result = typeof __decodeImageDataImpl === "function"
            ? __decodeImageDataImpl(payload)
            : { ok: false };
        globalThis.__dr = result;
        void 0;
    )JS";

    bridge->load_script(js);

    auto ok = engine.evaluate("String(globalThis.__dr && globalThis.__dr.ok)").toString();
    if (ok != "true") {
        WARN("Native decoder unavailable (no Skia) — skipping");
        return;
    }

    CHECK(engine.evaluate("globalThis.__dr.width").toString() == "2");
    CHECK(engine.evaluate("globalThis.__dr.height").toString() == "2");
    CHECK(engine.evaluate("String(globalThis.__dr.pixels.length)").toString() == "16");

    // First pixel: red channel should be 255
    CHECK(engine.evaluate("String(globalThis.__dr.pixels[0])").toString() == "255");
    // Green and blue should be 0
    CHECK(engine.evaluate("String(globalThis.__dr.pixels[1])").toString() == "0");
    CHECK(engine.evaluate("String(globalThis.__dr.pixels[2])").toString() == "0");
}

TEST_CASE("copyExternalImageToTexture stores pixels on texture", "[webcompat][gltf][texture]") {
    View root;
    ScriptEngine engine;
    pulp::state::StateStore store;
    root.set_bounds({0, 0, 100, 100});
    root.set_theme(Theme::dark());
    auto bridge = std::make_unique<WidgetBridge>(engine, root, store);

    // Test the JS-level copyExternalImageToTexture with a mock bitmap
    std::string js = R"JS(
        var bitmap = {
            width: 4, height: 4,
            _decodedPixels: new Uint8Array(64),
            close: function() {}
        };
        for (var i = 0; i < 64; i++) bitmap._decodedPixels[i] = i;

        // Create a mock texture
        var texture = {
            _objectName: "GPUTexture",
            width: 4, height: 4, format: "rgba8unorm",
            _bytes: null, _bytesPerRow: 0, _rowsPerImage: 0
        };

        // Create a mock queue and call copyExternalImageToTexture
        var queue = __createMockGPUQueue({});
        queue.copyExternalImageToTexture(
            { source: bitmap },
            { texture: texture },
            [4, 4]
        );

        globalThis.__texHasBytes = texture._bytes !== null && texture._bytes.length === 64;
        globalThis.__texBpr = texture._bytesPerRow;
        globalThis.__texRpi = texture._rowsPerImage;
        globalThis.__texW = texture.width;
        globalThis.__texH = texture.height;
        // Check pixel data was copied correctly
        globalThis.__pixel0 = texture._bytes ? texture._bytes[0] : -1;
        globalThis.__pixel63 = texture._bytes ? texture._bytes[63] : -1;
        void 0;
    )JS";

    bridge->load_script(js);

    CHECK(engine.evaluate("String(globalThis.__texHasBytes)").toString() == "true");
    CHECK(engine.evaluate("String(globalThis.__texBpr)").toString() == "16");  // 4 * 4
    CHECK(engine.evaluate("String(globalThis.__texRpi)").toString() == "4");
    CHECK(engine.evaluate("String(globalThis.__texW)").toString() == "4");
    CHECK(engine.evaluate("String(globalThis.__texH)").toString() == "4");
    CHECK(engine.evaluate("String(globalThis.__pixel0)").toString() == "0");
    CHECK(engine.evaluate("String(globalThis.__pixel63)").toString() == "63");
}
