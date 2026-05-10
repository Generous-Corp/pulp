// Web-compat prelude tests — validates the JS prelude layer
// Tests CSS parsing, named colors, document API, classList, querySelector,
// and DOM operations (appendChild, removeChild, getElementById, etc.).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "test_helpers.hpp"
#include <pulp/view/asset_manager.hpp>
#include <fstream>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/canvas_widget.hpp>

using namespace pulp::test;
using namespace pulp::view;

// ═══════════════════════════════════════════════════════════════════════════════
// Prelude loaded checks
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("WebCompat: named colors available", "[webcompat][prelude]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("__cssColors__['red']");
    REQUIRE(std::string(result.getWithDefault<std::string_view>("")) == "#ff0000");
}

TEST_CASE("WebCompat: named colors cornflowerblue", "[webcompat][prelude]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("__cssColors__['cornflowerblue']");
    REQUIRE(std::string(result.getWithDefault<std::string_view>("")) == "#6495ed");
}

TEST_CASE("WebCompat: parseCSSColor red", "[webcompat][prelude]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("parseCSSColor('red')");
    REQUIRE(std::string(result.getWithDefault<std::string_view>("")) == "#ff0000");
}

TEST_CASE("WebCompat: document object exists", "[webcompat][prelude]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("typeof document");
    REQUIRE(std::string(result.getWithDefault<std::string_view>("")) == "object");
}

TEST_CASE("WebCompat: document.body exists", "[webcompat][prelude]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("document.body !== null && document.body !== undefined");
    REQUIRE(result.getWithDefault<bool>(false) == true);
}

TEST_CASE("WebCompat: Element constructor exists", "[webcompat][prelude]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("typeof Element");
    REQUIRE(std::string(result.getWithDefault<std::string_view>("")) == "function");
}

TEST_CASE("WebCompat: StyleSheet constructor exists", "[webcompat][prelude]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("typeof StyleSheet");
    REQUIRE(std::string(result.getWithDefault<std::string_view>("")) == "function");
}

// ═══════════════════════════════════════════════════════════════════════════════
// CSS parser functions
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("WebCompat: parseCSSColor hex", "[webcompat][parser]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("parseCSSColor('#1e90ff')");
    REQUIRE(std::string(result.getWithDefault<std::string_view>("")) == "#1e90ff");
}

TEST_CASE("WebCompat: parseCSSColor hex short", "[webcompat][parser]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("parseCSSColor('#f00')");
    REQUIRE(std::string(result.getWithDefault<std::string_view>("")) == "#f00");
}

TEST_CASE("WebCompat: parseCSSColor rgb", "[webcompat][parser]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("parseCSSColor('rgb(255, 128, 0)')");
    REQUIRE(std::string(result.getWithDefault<std::string_view>("")) == "#ff8000");
}

TEST_CASE("WebCompat: parseCSSColor hsl red", "[webcompat][parser]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("parseCSSColor('hsl(0, 100%, 50%)')");
    REQUIRE(std::string(result.getWithDefault<std::string_view>("")) == "#ff0000");
}

TEST_CASE("WebCompat: parseCSSColor transparent", "[webcompat][parser]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("parseCSSColor('transparent')");
    REQUIRE(std::string(result.getWithDefault<std::string_view>("")) == "#00000000");
}

TEST_CASE("WebCompat: parseCSSLength px", "[webcompat][parser]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("parseCSSLength('20px').value");
    REQUIRE(result.getWithDefault<double>(0.0) == 20.0);
}

TEST_CASE("WebCompat: parseCSSLength unit", "[webcompat][parser]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("parseCSSLength('20px').unit");
    REQUIRE(std::string(result.getWithDefault<std::string_view>("")) == "px");
}

TEST_CASE("WebCompat: parseCSSLength percent", "[webcompat][parser]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("parseCSSLength('50%').unit");
    REQUIRE(std::string(result.getWithDefault<std::string_view>("")) == "%");
}

TEST_CASE("WebCompat: parseCSSLength auto", "[webcompat][parser]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("parseCSSLength('auto').unit");
    REQUIRE(std::string(result.getWithDefault<std::string_view>("")) == "auto");
}

TEST_CASE("WebCompat: parseCSSLength bare number", "[webcompat][parser]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("parseCSSLength('42').value");
    REQUIRE(result.getWithDefault<double>(0.0) == 42.0);
}

TEST_CASE("WebCompat: expandShorthand 1 value", "[webcompat][parser]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("JSON.stringify(expandShorthand('10px'))");
    REQUIRE(std::string(result.getWithDefault<std::string_view>("")) == "[10,10,10,10]");
}

TEST_CASE("WebCompat: expandShorthand 2 values", "[webcompat][parser]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("JSON.stringify(expandShorthand('10px 20px'))");
    REQUIRE(std::string(result.getWithDefault<std::string_view>("")) == "[10,20,10,20]");
}

TEST_CASE("WebCompat: expandShorthand 3 values", "[webcompat][parser]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("JSON.stringify(expandShorthand('10px 20px 30px'))");
    REQUIRE(std::string(result.getWithDefault<std::string_view>("")) == "[10,20,30,20]");
}

TEST_CASE("WebCompat: expandShorthand 4 values", "[webcompat][parser]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("JSON.stringify(expandShorthand('10px 20px 30px 40px'))");
    REQUIRE(std::string(result.getWithDefault<std::string_view>("")) == "[10,20,30,40]");
}

TEST_CASE("WebCompat: parseTransform scale", "[webcompat][parser]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("parseTransform('scale(1.5)')[0].fn");
    REQUIRE(std::string(result.getWithDefault<std::string_view>("")) == "scale");
}

TEST_CASE("WebCompat: parseTransform multiple", "[webcompat][parser]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("parseTransform('scale(1.5) rotate(45)').length");
    REQUIRE(result.getWithDefault<int>(0) == 2);
}

TEST_CASE("WebCompat: parseTransition", "[webcompat][parser]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("parseTransition('opacity 300ms ease').property");
    REQUIRE(std::string(result.getWithDefault<std::string_view>("")) == "opacity");
}

TEST_CASE("WebCompat: matchMedia min-width", "[webcompat][parser]") {
    TestEnvironment env(800, 600);
    auto result = env.engine.evaluate("typeof _matchMediaQuery === 'function' && _matchMediaQuery('(min-width: 600px)')");
    REQUIRE(result.getWithDefault<bool>(false) == true);
}

TEST_CASE("WebCompat: matchMedia orientation landscape", "[webcompat][parser]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("_matchMediaQuery('(orientation: landscape)')");
    REQUIRE(result.getWithDefault<bool>(false) == true);
}

// ═══════════════════════════════════════════════════════════════════════════════
// createElement (JS-only construction, no appendChild)
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("WebCompat: createElement returns Element", "[webcompat][element]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("document.createElement('div') instanceof Element");
    REQUIRE(result.getWithDefault<bool>(false) == true);
}

TEST_CASE("WebCompat: createElement tagName", "[webcompat][element]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("document.createElement('span').tagName");
    REQUIRE(std::string(result.getWithDefault<std::string_view>("")) == "SPAN");
}

TEST_CASE("WebCompat: canvas getContext returns 2d and mock webgpu contexts", "[webcompat][canvas]") {
    TestEnvironment env;
    env.eval(R"(
        var canvas = document.createElement('canvas');
        canvas.id = 'phase13-canvas';
        canvas.width = 320;
        canvas.height = 180;
        document.body.appendChild(canvas);
    )");

    auto has2d = env.engine.evaluate("document.getElementById('phase13-canvas').getContext('2d') !== null");
    auto webGpuType = env.engine.evaluate("document.getElementById('phase13-canvas').getContext('webgpu')._objectName");
    auto hasWebGpuConfigure = env.engine.evaluate("typeof document.getElementById('phase13-canvas').getContext('webgpu').configure");
    REQUIRE(has2d.getWithDefault<bool>(false) == true);
    REQUIRE(std::string(webGpuType.getWithDefault<std::string_view>("")) == "GPUCanvasContext");
    REQUIRE(std::string(hasWebGpuConfigure.getWithDefault<std::string_view>("")) == "function");

    env.root.layout_children();
    auto nativeIdValue = env.engine.evaluate("document.getElementById('phase13-canvas')._id");
    auto nativeId = std::string(nativeIdValue.getWithDefault<std::string_view>(""));
    auto* canvas = env.widget(nativeId);
    REQUIRE(canvas != nullptr);
    REQUIRE(bounds_match(canvas->bounds(), 0, 0, 320, 180));
}

TEST_CASE("WebCompat: window.pulp.gpu.getInfo exposes native GPU truth", "[webcompat][canvas][gpu]") {
    TestEnvironment env;
    auto backend = env.engine.evaluate("window.pulp.gpu.getInfo().backend");
    auto available = env.engine.evaluate("window.pulp.gpu.getInfo().available");
    REQUIRE(std::string(backend.getWithDefault<std::string_view>("")) == "Dawn/WebGPU");
    REQUIRE(available.getWithDefault<bool>(false) == true);
}

TEST_CASE("WebCompat: navigator.gpu exposes preferred format and adapter promise", "[webcompat][canvas][gpu]") {
    TestEnvironment env;
    auto backend = env.engine.evaluate("navigator.gpu.backend");
    auto format = env.engine.evaluate("navigator.gpu.getPreferredCanvasFormat()");
    auto promiseTag = env.engine.evaluate("Object.prototype.toString.call(navigator.gpu.requestAdapter())");
    auto thenType = env.engine.evaluate("typeof navigator.gpu.requestAdapter().then");

    REQUIRE(std::string(backend.getWithDefault<std::string_view>("")) == "Dawn/WebGPU");
    REQUIRE(std::string(format.getWithDefault<std::string_view>("")) == "bgra8unorm");
    REQUIRE(std::string(promiseTag.getWithDefault<std::string_view>("")) == "[object Promise]");
    REQUIRE(std::string(thenType.getWithDefault<std::string_view>("")) == "function");
}

TEST_CASE("WebCompat: mock adapter and device graph expose early WebGPU entry points", "[webcompat][canvas][gpu]") {
    TestEnvironment env;

    auto adapterName = env.engine.evaluate("window.pulp.gpu.createMockAdapter().name");
    auto adapterFeature = env.engine.evaluate("window.pulp.gpu.createMockAdapter().features.has('timestamp-query')");
    auto deviceQueueType = env.engine.evaluate("window.pulp.gpu.createMockDevice(window.pulp.gpu.createMockAdapter(), { requiredFeatures: ['timestamp-query'] }).queue._objectName");
    auto deviceFeature = env.engine.evaluate("window.pulp.gpu.createMockDevice(window.pulp.gpu.createMockAdapter(), { requiredFeatures: ['timestamp-query'] }).features.has('timestamp-query')");
    auto bufferSize = env.engine.evaluate("window.pulp.gpu.createMockDevice().createBuffer({ size: 64, usage: GPUBufferUsage.COPY_DST }).size");
    auto textureViewFormat = env.engine.evaluate("window.pulp.gpu.createMockDevice().createTexture({ size: { width: 16, height: 8 }, format: 'rgba8unorm', usage: GPUTextureUsage.TEXTURE_BINDING }).createView().format");
    auto commandBufferType = env.engine.evaluate("window.pulp.gpu.createMockDevice().createCommandEncoder().finish()._objectName");

    REQUIRE(std::string(adapterName.getWithDefault<std::string_view>("")) == "Mock Dawn Adapter");
    REQUIRE(adapterFeature.getWithDefault<bool>(false) == true);
    REQUIRE(std::string(deviceQueueType.getWithDefault<std::string_view>("")) == "GPUQueue");
    REQUIRE(deviceFeature.getWithDefault<bool>(false) == true);
    REQUIRE(bufferSize.getWithDefault<int32_t>(0) == 64);
    REQUIRE(std::string(textureViewFormat.getWithDefault<std::string_view>("")) == "rgba8unorm");
    REQUIRE(std::string(commandBufferType.getWithDefault<std::string_view>("")) == "GPUCommandBuffer");
}

TEST_CASE("WebCompat: mock GPUCanvasContext configures and returns current texture views", "[webcompat][canvas][gpu]") {
    TestEnvironment env;
    env.eval(R"(
        var canvas = document.createElement('canvas');
        canvas.id = 'phase13-webgpu-canvas';
        canvas.width = 256;
        canvas.height = 144;
        document.body.appendChild(canvas);
    )");

    auto configuredFormat = env.engine.evaluate(R"(
        (function() {
            var adapter = window.pulp.gpu.createMockAdapter();
            var device = window.pulp.gpu.createMockDevice(adapter, { requiredFeatures: ['timestamp-query'] });
            var context = document.getElementById('phase13-webgpu-canvas').getContext('webgpu');
            context.configure({
                device: device,
                format: navigator.gpu.getPreferredCanvasFormat(),
                alphaMode: 'premultiplied'
            });
            return context.getCurrentTexture().createView().format;
        })()
    )");
    auto currentTextureWidth = env.engine.evaluate(R"(
        (function() {
            var context = document.getElementById('phase13-webgpu-canvas').getContext('webgpu');
            return context.getCurrentTexture().width;
        })()
    )");

    REQUIRE(std::string(configuredFormat.getWithDefault<std::string_view>("")) == "bgra8unorm");
    REQUIRE(currentTextureWidth.getWithDefault<int32_t>(0) == 256);
}

TEST_CASE("WebCompat: WebGPU globals expose core usage and stage constants", "[webcompat][canvas][gpu]") {
    TestEnvironment env;
    auto shaderMask = env.engine.evaluate("GPUShaderStage.VERTEX | GPUShaderStage.FRAGMENT");
    auto bufferMask = env.engine.evaluate("GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ");
    auto textureMask = env.engine.evaluate("GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST");
    auto mapRead = env.engine.evaluate("GPUMapMode.READ");
    auto colorAll = env.engine.evaluate("GPUColorWrite.ALL");

    REQUIRE(shaderMask.getWithDefault<int32_t>(0) == 0x3);
    REQUIRE(bufferMask.getWithDefault<int32_t>(0) == 0x9);
    REQUIRE(textureMask.getWithDefault<int32_t>(0) == 0x6);
    REQUIRE(mapRead.getWithDefault<int32_t>(0) == 0x1);
    REQUIRE(colorAll.getWithDefault<int32_t>(0) == 0xF);
}

TEST_CASE("WebCompat: performance and storage shims are available", "[webcompat][browser]") {
    TestEnvironment env;
    auto perfNow = env.engine.evaluate("typeof performance.now === 'function' && performance.now() >= 0");
    env.eval("localStorage.setItem('phase13-key', 'phase13-value');");
    auto stored = env.engine.evaluate("localStorage.getItem('phase13-key')");

    REQUIRE(perfNow.getWithDefault<bool>(false) == true);
    REQUIRE(std::string(stored.getWithDefault<std::string_view>("")) == "phase13-value");
}

TEST_CASE("WebCompat: UTF-8 helpers round-trip non-ASCII text", "[webcompat][browser]") {
    TestEnvironment env;
    auto byteLength = env.engine.evaluate("new TextEncoder().encode('Pulp ✓').length");
    auto decoded = env.engine.evaluate("new TextDecoder().decode(new TextEncoder().encode('Pulp ✓'))");
    auto cloned = env.engine.evaluate("structuredClone({ label: 'Pulp ✓' }).label");

    REQUIRE(byteLength.getWithDefault<int32_t>(0) == 8);
    REQUIRE(std::string(decoded.getWithDefault<std::string_view>("")) == "Pulp ✓");
    REQUIRE(std::string(cloned.getWithDefault<std::string_view>("")) == "Pulp ✓");
}

TEST_CASE("WebCompat: browser utility shims cover base64, crypto, and Image", "[webcompat][browser]") {
    TestEnvironment env;
    auto roundTrip = env.engine.evaluate("atob(btoa('pulp'))");
    auto randomCount = env.engine.evaluate("crypto.getRandomValues(new Uint8Array(4)).length");
    auto imageComplete = env.engine.evaluate("var img = new Image(); img.src = 'mock.png'; img.complete");

    REQUIRE(std::string(roundTrip.getWithDefault<std::string_view>("")) == "pulp");
    REQUIRE(randomCount.getWithDefault<int32_t>(0) == 4);
    REQUIRE(imageComplete.getWithDefault<bool>(false) == true);
}

TEST_CASE("WebCompat: fetch bridge preserves UTF-8 JSON asset payloads", "[webcompat][browser][fetch]") {
    TestEnvironment env;
    auto& assets = AssetManager::instance();

    static const char kJson[] = "{\"label\":\"Pulp ✓\"}";
    assets.register_embedded("phase13/config.json",
                             reinterpret_cast<const uint8_t*>(kJson),
                             sizeof(kJson) - 1);

    auto promiseTag = env.engine.evaluate(
        "Object.prototype.toString.call(fetch('pulp://phase13/config.json'))");
    auto contentType = env.engine.evaluate(R"(
        (function() {
            var response = __responseFromAssetRecord(__loadAssetSync__('pulp://phase13/config.json'));
            return response.headers.get('content-type');
        })()
    )");
    auto text = env.engine.evaluate(R"(
        (function() {
            var response = __responseFromAssetRecord(__loadAssetSync__('pulp://phase13/config.json'));
            return response.text();
        })()
    )");
    auto label = env.engine.evaluate(R"(
        (function() {
            var response = __responseFromAssetRecord(__loadAssetSync__('pulp://phase13/config.json'));
            return response.json().label;
        })()
    )");
    auto byteLength = env.engine.evaluate(R"(
        (function() {
            var response = __responseFromAssetRecord(__loadAssetSync__('pulp://phase13/config.json'));
            return new Uint8Array(response.arrayBuffer()).length;
        })()
    )");

    REQUIRE(std::string(promiseTag.getWithDefault<std::string_view>("")) == "[object Promise]");
    REQUIRE(std::string(contentType.getWithDefault<std::string_view>("")) == "application/json;charset=utf-8");
    REQUIRE(std::string(text.getWithDefault<std::string_view>("")) == kJson);
    REQUIRE(std::string(label.getWithDefault<std::string_view>("")) == "Pulp ✓");
    REQUIRE(byteLength.getWithDefault<int32_t>(0) == static_cast<int32_t>(sizeof(kJson) - 1));
}

TEST_CASE("WebCompat: file and blob URLs stay readable through the browser helpers", "[webcompat][browser][fetch]") {
    TestEnvironment env;

    const std::string fileText = "Pulp ✓ file";
    const auto tempPath = std::filesystem::temp_directory_path() / "pulp-phase13-fetch.txt";
    {
        std::ofstream out(tempPath, std::ios::binary);
        out << fileText;
    }

    const auto genericPath = tempPath.generic_string();
    const std::string fileUrl = std::string("file://")
        + (genericPath.empty() || genericPath.front() == '/' ? "" : "/")
        + genericPath;
    const std::string fileExpr = "'" + fileUrl + "'";

    auto fetchedText = env.engine.evaluate(
        "(function() {"
        "  var response = __responseFromAssetRecord(__loadAssetSync__(" + fileExpr + "));"
        "  return response.text();"
        "})()");

    env.eval(R"(
        var phase13Blob = new Blob(
            [new TextEncoder().encode('{"label":"Pulp ✓"}')],
            { type: 'application/json' }
        );
        var phase13BlobUrl = URL.createObjectURL(phase13Blob);
    )");
    auto blobUrl = env.engine.evaluate("phase13BlobUrl");
    auto blobLabel = env.engine.evaluate("__responseFromDataUri(phase13BlobUrl, phase13BlobUrl).json().label");

    REQUIRE(std::string(fetchedText.getWithDefault<std::string_view>("")) == fileText);
    const auto blobUrlString = std::string(blobUrl.getWithDefault<std::string_view>(""));
    REQUIRE(blobUrlString.find("data:application/json;charset=utf-8;base64,") == 0);
    REQUIRE(std::string(blobLabel.getWithDefault<std::string_view>("")) == "Pulp ✓");

    std::filesystem::remove(tempPath);
}

TEST_CASE("WebCompat: element.id assignment", "[webcompat][element]") {
    TestEnvironment env;
    env.eval("var el = document.createElement('div'); el.id = 'test123';");
    auto result = env.engine.evaluate("el.id");
    REQUIRE(std::string(result.getWithDefault<std::string_view>("")) == "test123");
}

TEST_CASE("WebCompat: element.textContent", "[webcompat][element]") {
    TestEnvironment env;
    env.eval("var el = document.createElement('span'); el.textContent = 'Hello World';");
    auto result = env.engine.evaluate("el.textContent");
    REQUIRE(std::string(result.getWithDefault<std::string_view>("")) == "Hello World");
}

TEST_CASE("WebCompat: element.hidden default false", "[webcompat][element]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("document.createElement('div').hidden");
    REQUIRE(result.getWithDefault<bool>(true) == false);
}

TEST_CASE("WebCompat: element.disabled default false", "[webcompat][element]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("document.createElement('div').disabled");
    REQUIRE(result.getWithDefault<bool>(true) == false);
}

// ═══════════════════════════════════════════════════════════════════════════════
// classList
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("WebCompat: classList.add", "[webcompat][classList]") {
    TestEnvironment env;
    env.eval("var el = document.createElement('div'); el.classList.add('active');");
    auto result = env.engine.evaluate("el.classList.contains('active')");
    REQUIRE(result.getWithDefault<bool>(false) == true);
}

TEST_CASE("WebCompat: classList.remove", "[webcompat][classList]") {
    TestEnvironment env;
    env.eval("var el = document.createElement('div'); el.classList.add('active'); el.classList.remove('active');");
    auto result = env.engine.evaluate("el.classList.contains('active')");
    REQUIRE(result.getWithDefault<bool>(true) == false);
}

TEST_CASE("WebCompat: classList.toggle on", "[webcompat][classList]") {
    TestEnvironment env;
    env.eval("var el = document.createElement('div'); el.classList.toggle('on');");
    auto result = env.engine.evaluate("el.classList.contains('on')");
    REQUIRE(result.getWithDefault<bool>(false) == true);
}

TEST_CASE("WebCompat: classList.toggle off", "[webcompat][classList]") {
    TestEnvironment env;
    env.eval("var el = document.createElement('div'); el.classList.add('on'); el.classList.toggle('on');");
    auto result = env.engine.evaluate("el.classList.contains('on')");
    REQUIRE(result.getWithDefault<bool>(true) == false);
}

TEST_CASE("WebCompat: className setter", "[webcompat][classList]") {
    TestEnvironment env;
    env.eval("var el = document.createElement('div'); el.className = 'foo bar';");
    auto r1 = env.engine.evaluate("el.classList.contains('foo')");
    auto r2 = env.engine.evaluate("el.classList.contains('bar')");
    REQUIRE(r1.getWithDefault<bool>(false) == true);
    REQUIRE(r2.getWithDefault<bool>(false) == true);
}

// ═══════════════════════════════════════════════════════════════════════════════
// StyleSheet construction
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("WebCompat: StyleSheet construction", "[webcompat][stylesheet]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("new StyleSheet({'.box': {width: '100px'}}) instanceof StyleSheet");
    REQUIRE(result.getWithDefault<bool>(false) == true);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Style pending, setAttribute, dataset
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("WebCompat: style property stores pending value", "[webcompat][style]") {
    TestEnvironment env;
    env.eval("var el = document.createElement('div'); el.style.width = '200px';");
    auto result = env.engine.evaluate("el.style._props['width']");
    REQUIRE(std::string(result.getWithDefault<std::string_view>("")) == "200px");
}

TEST_CASE("WebCompat: setAttribute/getAttribute", "[webcompat][element]") {
    TestEnvironment env;
    env.eval("var el = document.createElement('div'); el.setAttribute('data-name', 'test');");
    auto result = env.engine.evaluate("el.getAttribute('data-name')");
    REQUIRE(std::string(result.getWithDefault<std::string_view>("")) == "test");
}

TEST_CASE("WebCompat: dataset from data attribute", "[webcompat][element]") {
    TestEnvironment env;
    env.eval("var el = document.createElement('div'); el.setAttribute('data-user-id', '42');");
    auto result = env.engine.evaluate("el.dataset.userId");
    REQUIRE(std::string(result.getWithDefault<std::string_view>("")) == "42");
}

// ═══════════════════════════════════════════════════════════════════════════════
// DOM operations (appendChild, removeChild, getElementById, querySelector, etc.)
// These tests require the web-compat-dom-ops.js prelude (loaded by WidgetBridge).
// They call native C++ bridge functions from within JS prototype methods, which
// requires sufficient C stack for the QuickJS↔C++ interleaving.
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("WebCompat: appendChild creates native widget", "[webcompat][dom]") {
    TestEnvironment env;
    env.eval("var __el = document.createElement('div');");
    env.eval("document.body.appendChild(__el);");
    auto r = env.engine.evaluate("__el._nativeCreated");
    REQUIRE(r.getWithDefault<bool>(false) == true);
}

TEST_CASE("WebCompat: appendChild span with text", "[webcompat][dom]") {
    TestEnvironment env;
    env.eval("var __sp = document.createElement('span');");
    env.eval("__sp.textContent = 'Hello World';");
    env.eval("document.body.appendChild(__sp);");
    env.eval("var __spId = __sp._id;");
    auto id = std::string(env.engine.evaluate("__spId").getWithDefault<std::string_view>(""));
    auto* w = env.widget(id);
    REQUIRE(w != nullptr);
    auto* label = dynamic_cast<Label*>(w);
    REQUIRE(label != nullptr);
}

TEST_CASE("WebCompat: appendChild nested divs", "[webcompat][dom]") {
    TestEnvironment env;
    env.eval("var __outer = document.createElement('div');");
    env.eval("var __inner = document.createElement('div');");
    env.eval("__outer.appendChild(__inner);");
    env.eval("document.body.appendChild(__outer);");
    env.eval("var __outerId = __outer._id;");
    env.eval("var __innerId = __inner._id;");
    auto outerId = std::string(env.engine.evaluate("__outerId").getWithDefault<std::string_view>(""));
    auto innerId = std::string(env.engine.evaluate("__innerId").getWithDefault<std::string_view>(""));
    REQUIRE(env.widget(outerId) != nullptr);
    REQUIRE(env.widget(innerId) != nullptr);
    REQUIRE(env.widget(outerId)->child_count() == 1);
}

TEST_CASE("WebCompat: appendChild with style", "[webcompat][dom]") {
    TestEnvironment env;
    env.eval("var __box = document.createElement('div');");
    env.eval("__box.style.width = '200px';");
    env.eval("__box.style.height = '100px';");
    env.eval("__box.style.backgroundColor = '#ff0000';");
    env.eval("document.body.appendChild(__box);");
    env.eval("var __boxId = __box._id;");
    auto id = std::string(env.engine.evaluate("__boxId").getWithDefault<std::string_view>(""));
    auto* w = env.widget(id);
    REQUIRE(w != nullptr);
    REQUIRE(w->flex().preferred_width == 200.0f);
    REQUIRE(w->flex().preferred_height == 100.0f);
    REQUIRE(w->has_background_color());
}

TEST_CASE("WebCompat: removeChild", "[webcompat][dom]") {
    TestEnvironment env;
    env.eval("var __p = document.createElement('div');");
    env.eval("var __c = document.createElement('div');");
    env.eval("__p.appendChild(__c);");
    env.eval("document.body.appendChild(__p);");
    env.eval("var __pId = __p._id;");
    env.eval("__p.removeChild(__c);");
    auto pid = std::string(env.engine.evaluate("__pId").getWithDefault<std::string_view>(""));
    auto* p = env.widget(pid);
    REQUIRE(p != nullptr);
    REQUIRE(p->child_count() == 0);
}

TEST_CASE("WebCompat: getElementById after appendChild", "[webcompat][dom]") {
    TestEnvironment env;
    env.eval("var __findEl = document.createElement('div');");
    env.eval("__findEl.id = 'findme123';");
    env.eval("document.body.appendChild(__findEl);");
    env.eval("var __found = document.getElementById('findme123');");
    auto r = env.engine.evaluate("__found !== null && __found.id === 'findme123'");
    REQUIRE(r.getWithDefault<bool>(false) == true);
}

TEST_CASE("WebCompat: querySelector after appendChild", "[webcompat][dom]") {
    TestEnvironment env;
    env.eval("var __qel = document.createElement('div');");
    env.eval("__qel.className = 'test-panel';");
    env.eval("document.body.appendChild(__qel);");
    env.eval("var __qfound = document.querySelector('.test-panel');");
    auto r = env.engine.evaluate("__qfound !== null");
    REQUIRE(r.getWithDefault<bool>(false) == true);
}

TEST_CASE("WebCompat: addEventListener click", "[webcompat][events]") {
    TestEnvironment env;
    env.eval("var __evtEl = document.createElement('div');");
    env.eval("var __clicked = false;");
    env.eval("__evtEl.addEventListener('click', function() { __clicked = true; });");
    env.eval("document.body.appendChild(__evtEl);");
    env.eval("__evtEl.dispatchEvent({type: 'click', bubbles: true});");
    auto r = env.engine.evaluate("__clicked");
    REQUIRE(r.getWithDefault<bool>(false) == true);
}

TEST_CASE("WebCompat: StyleSheet applies to appended element", "[webcompat][dom]") {
    TestEnvironment env;
    env.eval("var __sheet = new StyleSheet({'.styled-box': { width: '150px', height: '75px' }});");
    env.eval("__sheet.attach();");
    env.eval("var __sEl = document.createElement('div');");
    env.eval("__sEl.className = 'styled-box';");
    env.eval("document.body.appendChild(__sEl);");
    env.eval("var __sElId = __sEl._id;");
    auto id = std::string(env.engine.evaluate("__sElId").getWithDefault<std::string_view>(""));
    auto* w = env.widget(id);
    REQUIRE(w != nullptr);
    REQUIRE(w->flex().preferred_width == 150.0f);
    REQUIRE(w->flex().preferred_height == 75.0f);
}

// ═══════════════════════════════════════════════════════════════════════════════
// display: flex row-default (pulp #1147)
// CSS web-platform default for `display: flex` is `flex-direction: row`.
// Pulp's underlying widgets default to FlexDirection::column (RN convention),
// so web-compat-style-decl.js must override on `display: flex` unless the
// consumer also declared flexDirection. These tests pin that behavior.
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("WebCompat: display:flex defaults to flex-direction:row", "[webcompat][style][issue-1147]") {
    TestEnvironment env;
    env.eval("var __box = document.createElement('div');");
    env.eval("document.body.appendChild(__box);");
    env.eval("__box.style.display = 'flex';");
    env.eval("__box.appendChild(document.createElement('div'));");
    env.eval("__box.appendChild(document.createElement('div'));");
    env.eval("var __id = __box._id;");
    auto id = std::string(env.engine.evaluate("__id").getWithDefault<std::string_view>(""));
    auto* w = env.widget(id);
    REQUIRE(w != nullptr);
    REQUIRE(w->flex().direction == FlexDirection::row);
}

TEST_CASE("WebCompat: display:flex with explicit flexDirection:column overrides default", "[webcompat][style][issue-1147]") {
    TestEnvironment env;
    env.eval("var __box = document.createElement('div');");
    env.eval("document.body.appendChild(__box);");
    env.eval("__box.style.display = 'flex';");
    env.eval("__box.style.flexDirection = 'column';");
    auto id = std::string(env.engine.evaluate("__box._id").getWithDefault<std::string_view>(""));
    auto* w = env.widget(id);
    REQUIRE(w != nullptr);
    REQUIRE(w->flex().direction == FlexDirection::column);
}

TEST_CASE("WebCompat: display:flex with explicit flexDirection:row stays row", "[webcompat][style][issue-1147]") {
    TestEnvironment env;
    env.eval("var __box = document.createElement('div');");
    env.eval("document.body.appendChild(__box);");
    env.eval("__box.style.display = 'flex';");
    env.eval("__box.style.flexDirection = 'row';");
    auto id = std::string(env.engine.evaluate("__box._id").getWithDefault<std::string_view>(""));
    auto* w = env.widget(id);
    REQUIRE(w != nullptr);
    REQUIRE(w->flex().direction == FlexDirection::row);
}

TEST_CASE("WebCompat: flexDirection set BEFORE display:flex still wins", "[webcompat][style][issue-1147]") {
    // The user-declared direction must always win, regardless of which
    // assignment happens first. Setting flexDirection before display:flex
    // means _props.flexDirection is already populated when the display
    // handler checks for an explicit override.
    TestEnvironment env;
    env.eval("var __box = document.createElement('div');");
    env.eval("document.body.appendChild(__box);");
    env.eval("__box.style.flexDirection = 'column';");
    env.eval("__box.style.display = 'flex';");
    auto id = std::string(env.engine.evaluate("__box._id").getWithDefault<std::string_view>(""));
    auto* w = env.widget(id);
    REQUIRE(w != nullptr);
    REQUIRE(w->flex().direction == FlexDirection::column);
}

TEST_CASE("WebCompat: display:flex children lay out horizontally by default", "[webcompat][layout][issue-1147]") {
    // End-to-end: <div style={{display:'flex'}}><a/><b/><c/></div> with each
    // child sized 50px wide should produce x = 0, 50, 100 (row layout) — not
    // y = 0, 50, 100 (column).
    TestEnvironment env(400, 200);
    env.eval("var __row = document.createElement('div');");
    env.eval("__row.style.width = '300px';");
    env.eval("__row.style.height = '100px';");
    env.eval("document.body.appendChild(__row);");
    env.eval("__row.style.display = 'flex';");

    for (int i = 0; i < 3; ++i) {
        env.eval("(function(){ var __c = document.createElement('div');"
                 " __c.style.width = '50px'; __c.style.height = '50px';"
                 " __row.appendChild(__c); })();");
    }
    env.eval("var __rowId = __row._id;");

    env.root.layout_children();

    auto id = std::string(env.engine.evaluate("__rowId").getWithDefault<std::string_view>(""));
    auto* w = env.widget(id);
    REQUIRE(w != nullptr);
    REQUIRE(w->child_count() == 3);
    // Row layout: each child at x=0,50,100 with same y
    REQUIRE(w->child_at(0)->bounds().x == 0.0f);
    REQUIRE(w->child_at(1)->bounds().x == 50.0f);
    REQUIRE(w->child_at(2)->bounds().x == 100.0f);
    REQUIRE(w->child_at(0)->bounds().y == w->child_at(1)->bounds().y);
    REQUIRE(w->child_at(1)->bounds().y == w->child_at(2)->bounds().y);
}

TEST_CASE("WebCompat: display:none does not change flex direction", "[webcompat][style][issue-1147]") {
    // The row-default override must trigger only on `display: flex`, not on
    // `display: none` or other display values.
    TestEnvironment env;
    env.eval("var __box = document.createElement('div');");
    env.eval("document.body.appendChild(__box);");
    env.eval("__box.style.display = 'none';");
    auto id = std::string(env.engine.evaluate("__box._id").getWithDefault<std::string_view>(""));
    auto* w = env.widget(id);
    REQUIRE(w != nullptr);
    // __domAppend defaulted divs to column; display:none should not flip it.
    REQUIRE(w->flex().direction == FlexDirection::column);
}

TEST_CASE("WebCompat: flexFlow:wrap before display:flex still defaults to row", "[webcompat][style][issue-1147]") {
    // Codex review flag: `flex-flow: wrap` does NOT specify a direction —
    // CSS shorthand semantics keep the default direction (row). Without the
    // content-aware check, the display handler would treat any flexFlow as
    // explicit and skip the row default, leaving the widget at column.
    TestEnvironment env;
    env.eval("var __box = document.createElement('div');");
    env.eval("document.body.appendChild(__box);");
    env.eval("__box.style.flexFlow = 'wrap';");
    env.eval("__box.style.display = 'flex';");
    auto id = std::string(env.engine.evaluate("__box._id").getWithDefault<std::string_view>(""));
    auto* w = env.widget(id);
    REQUIRE(w != nullptr);
    REQUIRE(w->flex().direction == FlexDirection::row);
}

TEST_CASE("WebCompat: flexFlow:column before display:flex stays column", "[webcompat][style][issue-1147]") {
    // Sanity check: flexFlow with an explicit direction token still wins.
    TestEnvironment env;
    env.eval("var __box = document.createElement('div');");
    env.eval("document.body.appendChild(__box);");
    env.eval("__box.style.flexFlow = 'column wrap';");
    env.eval("__box.style.display = 'flex';");
    auto id = std::string(env.engine.evaluate("__box._id").getWithDefault<std::string_view>(""));
    auto* w = env.widget(id);
    REQUIRE(w != nullptr);
    REQUIRE(w->flex().direction == FlexDirection::column);
}

TEST_CASE("WebCompat: StyleSheet display:flex defaults to row", "[webcompat][style][issue-1147]") {
    // Class-based path: StyleSheet rules walk _props by iteration order via
    // _flushAll, not the per-property setter trap. The display:flex row
    // default must still kick in.
    TestEnvironment env;
    env.eval("var __sheet = new StyleSheet({'.row-flex': { display: 'flex' }});");
    env.eval("__sheet.attach();");
    env.eval("var __el = document.createElement('div');");
    env.eval("__el.className = 'row-flex';");
    env.eval("document.body.appendChild(__el);");
    auto id = std::string(env.engine.evaluate("__el._id").getWithDefault<std::string_view>(""));
    auto* w = env.widget(id);
    REQUIRE(w != nullptr);
    REQUIRE(w->flex().direction == FlexDirection::row);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Modern CSS color spaces (pulp #1434 Triage #8)
// ═══════════════════════════════════════════════════════════════════════════════
//
// Spike-quality oklch / oklab / lch / lab / color() → sRGB hex.
// Tolerances are intentionally generous (±2 hex levels per channel)
// because the conversion math involves Bradford adaptation + matrix
// products + gamma encode; the goal is "reasonably close round-trip"
// not "bit-exact reference". Reference values cross-checked against
// the CSS Color 4 reference implementation
// (https://www.w3.org/TR/css-color-4/) for sanity but allowing for
// the slight divergence of double-precision arithmetic.

namespace {
    // Helper: extract decimal channel values from a "#rrggbb[aa]" hex
    // string. Returns {r, g, b, a} in [0, 255] (a defaults to 255).
    struct Rgba { int r, g, b, a; };
    Rgba parseHex(const std::string& hex) {
        Rgba out{0, 0, 0, 255};
        if (hex.size() < 7 || hex[0] != '#') return out;
        auto h2 = [&](size_t i) -> int {
            return std::stoi(hex.substr(i, 2), nullptr, 16);
        };
        out.r = h2(1);
        out.g = h2(3);
        out.b = h2(5);
        if (hex.size() >= 9) out.a = h2(7);
        return out;
    }
}

TEST_CASE("WebCompat: parseCSSColor oklch returns hex", "[webcompat][parser][issue-1434-color]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("parseCSSColor('oklch(0.7 0.18 240)')");
    auto hex = std::string(result.getWithDefault<std::string_view>(""));
    REQUIRE(hex.size() >= 7);
    REQUIRE(hex[0] == '#');
    auto rgba = parseHex(hex);
    // oklch(0.7 0.18 240) is a saturated blue — blue channel dominates
    // and red is moderately damped.
    REQUIRE(rgba.b > rgba.r);
    REQUIRE(rgba.b > rgba.g);
}

TEST_CASE("WebCompat: parseCSSColor oklab black returns near-black", "[webcompat][parser][issue-1434-color]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("parseCSSColor('oklab(0 0 0)')");
    auto rgba = parseHex(std::string(result.getWithDefault<std::string_view>("")));
    REQUIRE(rgba.r <= 4);
    REQUIRE(rgba.g <= 4);
    REQUIRE(rgba.b <= 4);
}

TEST_CASE("WebCompat: parseCSSColor oklab white returns near-white", "[webcompat][parser][issue-1434-color]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("parseCSSColor('oklab(1 0 0)')");
    auto rgba = parseHex(std::string(result.getWithDefault<std::string_view>("")));
    REQUIRE(rgba.r >= 250);
    REQUIRE(rgba.g >= 250);
    REQUIRE(rgba.b >= 250);
}

TEST_CASE("WebCompat: parseCSSColor oklch with percent L", "[webcompat][parser][issue-1434-color]") {
    TestEnvironment env;
    // 70% L should round-trip to the same color as 0.7 L.
    auto a = env.engine.evaluate("parseCSSColor('oklch(70% 0.18 240)')");
    auto b = env.engine.evaluate("parseCSSColor('oklch(0.7 0.18 240)')");
    REQUIRE(std::string(a.getWithDefault<std::string_view>("")) ==
            std::string(b.getWithDefault<std::string_view>("")));
}

TEST_CASE("WebCompat: parseCSSColor oklch with alpha", "[webcompat][parser][issue-1434-color]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("parseCSSColor('oklch(0.7 0.18 240 / 50%)')");
    auto rgba = parseHex(std::string(result.getWithDefault<std::string_view>("")));
    // 50% alpha should be ~127 ± 1.
    REQUIRE(rgba.a >= 126);
    REQUIRE(rgba.a <= 128);
}

TEST_CASE("WebCompat: parseCSSColor lab returns hex", "[webcompat][parser][issue-1434-color]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("parseCSSColor('lab(50 -40 60)')");
    auto hex = std::string(result.getWithDefault<std::string_view>(""));
    REQUIRE(hex.size() >= 7);
    REQUIRE(hex[0] == '#');
    auto rgba = parseHex(hex);
    // lab(50 -40 60) — green-shifted, yellow-shifted: green > red, green > blue.
    REQUIRE(rgba.g > rgba.b);
}

TEST_CASE("WebCompat: parseCSSColor lch white", "[webcompat][parser][issue-1434-color]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("parseCSSColor('lch(100 0 0)')");
    auto rgba = parseHex(std::string(result.getWithDefault<std::string_view>("")));
    // L=100 with zero chroma should be near-white.
    REQUIRE(rgba.r >= 250);
    REQUIRE(rgba.g >= 250);
    REQUIRE(rgba.b >= 250);
}

TEST_CASE("WebCompat: parseCSSColor color(srgb) passthrough", "[webcompat][parser][issue-1434-color]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("parseCSSColor('color(srgb 1 0 0)')");
    auto rgba = parseHex(std::string(result.getWithDefault<std::string_view>("")));
    REQUIRE(rgba.r == 255);
    REQUIRE(rgba.g == 0);
    REQUIRE(rgba.b == 0);
}

TEST_CASE("WebCompat: parseCSSColor color(srgb-linear) gamma encodes", "[webcompat][parser][issue-1434-color]") {
    TestEnvironment env;
    // Linear 0.5 → sRGB gamma-encoded ≈ 188.
    auto result = env.engine.evaluate("parseCSSColor('color(srgb-linear 0.5 0.5 0.5)')");
    auto rgba = parseHex(std::string(result.getWithDefault<std::string_view>("")));
    REQUIRE(rgba.r >= 186);
    REQUIRE(rgba.r <= 190);
    REQUIRE(rgba.g == rgba.r);
    REQUIRE(rgba.b == rgba.r);
}

TEST_CASE("WebCompat: parseCSSColor color(display-p3) maps to sRGB", "[webcompat][parser][issue-1434-color]") {
    TestEnvironment env;
    // P3 red is wider than sRGB red; clamping should yield max-red sRGB.
    auto result = env.engine.evaluate("parseCSSColor('color(display-p3 1 0 0)')");
    auto rgba = parseHex(std::string(result.getWithDefault<std::string_view>("")));
    REQUIRE(rgba.r == 255);
    REQUIRE(rgba.g <= 5);
    REQUIRE(rgba.b <= 5);
}

TEST_CASE("WebCompat: parseCSSColor color(display-p3) gray passes through", "[webcompat][parser][issue-1434-color]") {
    TestEnvironment env;
    // P3 (0.5, 0.5, 0.5) gamma-encoded = sRGB (0.5, 0.5, 0.5) — gray
    // is invariant across same-white-point spaces (D65 == D65).
    auto result = env.engine.evaluate("parseCSSColor('color(display-p3 0.5 0.5 0.5)')");
    auto rgba = parseHex(std::string(result.getWithDefault<std::string_view>("")));
    REQUIRE(rgba.r >= 125);
    REQUIRE(rgba.r <= 130);
    REQUIRE(std::abs(rgba.r - rgba.g) <= 1);
    REQUIRE(std::abs(rgba.g - rgba.b) <= 1);
}

TEST_CASE("WebCompat: parseCSSColor oklch radians", "[webcompat][parser][issue-1434-color]") {
    TestEnvironment env;
    // 4.18879rad ≈ 240deg — should match the 240deg path.
    auto a = env.engine.evaluate("parseCSSColor('oklch(0.7 0.18 4.18879rad)')");
    auto b = env.engine.evaluate("parseCSSColor('oklch(0.7 0.18 240)')");
    auto ra = parseHex(std::string(a.getWithDefault<std::string_view>("")));
    auto rb = parseHex(std::string(b.getWithDefault<std::string_view>("")));
    REQUIRE(std::abs(ra.r - rb.r) <= 2);
    REQUIRE(std::abs(ra.g - rb.g) <= 2);
    REQUIRE(std::abs(ra.b - rb.b) <= 2);
}

TEST_CASE("WebCompat: parseCSSColor unknown color() space returns null", "[webcompat][parser][issue-1434-color]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("parseCSSColor('color(rec2020 0.5 0.5 0.5)') === null");
    REQUIRE(result.getWithDefault<bool>(false) == true);
}

TEST_CASE("WebCompat: parseCSSColor malformed oklch returns null", "[webcompat][parser][issue-1434-color]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("parseCSSColor('oklch(banana)') === null");
    REQUIRE(result.getWithDefault<bool>(false) == true);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Modern transform fan-out (pulp #1434 Triage #9)
// ═══════════════════════════════════════════════════════════════════════════════
//
// CSS transform string: scaleX/Y, skewX/Y, rotateX/Y/Z, matrix, plus
// rad/turn/grad angle units. Bridge: setSkew newly registered;
// rotateX/Y + matrix3d + perspective silently drop (pulp's 2D View
// has no 3D model); scaleX/Y last-write-wins (uniform setScale only).

TEST_CASE("WebCompat: parseTransform handles rad units", "[webcompat][parser][issue-1434-tx]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("parseTransform('rotate(1rad)')[0].args[0]");
    REQUIRE(result.getWithDefault<double>(0) > 57.0);  // 1 rad ≈ 57.3°
    REQUIRE(result.getWithDefault<double>(0) < 58.0);
}

TEST_CASE("WebCompat: parseTransform handles turn units", "[webcompat][parser][issue-1434-tx]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("parseTransform('rotate(0.5turn)')[0].args[0]");
    REQUIRE(result.getWithDefault<double>(0) == 180.0);
}

TEST_CASE("WebCompat: parseTransform handles grad units", "[webcompat][parser][issue-1434-tx]") {
    TestEnvironment env;
    auto result = env.engine.evaluate("parseTransform('rotate(100grad)')[0].args[0]");
    REQUIRE(result.getWithDefault<double>(0) == 90.0);  // 100 grad = 90°
}

TEST_CASE("WebCompat: parseTransform recognizes scaleX", "[webcompat][parser][issue-1434-tx]") {
    TestEnvironment env;
    auto fn = env.engine.evaluate("parseTransform('scaleX(2)')[0].fn");
    REQUIRE(std::string(fn.getWithDefault<std::string_view>("")) == "scaleX");
    auto val = env.engine.evaluate("parseTransform('scaleX(2)')[0].args[0]");
    REQUIRE(val.getWithDefault<double>(0) == 2.0);
}

TEST_CASE("WebCompat: parseTransform recognizes skewX/skewY", "[webcompat][parser][issue-1434-tx]") {
    TestEnvironment env;
    auto fn1 = env.engine.evaluate("parseTransform('skewX(10deg)')[0].fn");
    REQUIRE(std::string(fn1.getWithDefault<std::string_view>("")) == "skewX");
    auto fn2 = env.engine.evaluate("parseTransform('skewY(5deg)')[0].fn");
    REQUIRE(std::string(fn2.getWithDefault<std::string_view>("")) == "skewY");
}

TEST_CASE("WebCompat: parseTransform recognizes rotateZ", "[webcompat][parser][issue-1434-tx]") {
    TestEnvironment env;
    auto fn = env.engine.evaluate("parseTransform('rotateZ(45deg)')[0].fn");
    REQUIRE(std::string(fn.getWithDefault<std::string_view>("")) == "rotateZ");
}

TEST_CASE("WebCompat: parseTransform recognizes matrix(a b c d tx ty)", "[webcompat][parser][issue-1434-tx]") {
    TestEnvironment env;
    auto fn = env.engine.evaluate("parseTransform('matrix(1, 0, 0, 1, 50, 30)')[0].fn");
    REQUIRE(std::string(fn.getWithDefault<std::string_view>("")) == "matrix");
    auto len = env.engine.evaluate("parseTransform('matrix(1, 0, 0, 1, 50, 30)')[0].args.length");
    REQUIRE(len.getWithDefault<double>(0) == 6.0);
}

TEST_CASE("WebCompat: parseTransform multi-op walk", "[webcompat][parser][issue-1434-tx]") {
    TestEnvironment env;
    auto len = env.engine.evaluate("parseTransform('translateX(10px) rotate(45deg) scaleX(2) skewX(5deg)').length");
    REQUIRE(len.getWithDefault<double>(0) == 4.0);
}

TEST_CASE("WebCompat: setSkew bridge fn registered", "[webcompat][bridge][issue-1434-tx]") {
    // pulp #1434 Triage #9 — setSkew is now a registered bridge fn.
    // Earlier, View::set_skew existed in C++ but was unreachable from
    // JS; the parser stored skewX/skewY on the snapshot but couldn't
    // dispatch.
    TestEnvironment env;
    auto result = env.engine.evaluate("typeof setSkew");
    REQUIRE(std::string(result.getWithDefault<std::string_view>("")) == "function");
}

// pulp #1434 Triage #9 P1 fix (Codex post-merge audit) —
// matrix(a,b,c,d,tx,ty) must dispatch to setTransform with all 6
// components verbatim, NOT decompose to translate+uniform-scale+rotate
// (which silently dropped c/d skew components on rotation matrices and
// could mask zero-scale collapses).

TEST_CASE("WebCompat: matrix() preserves all 6 components verbatim",
          "[webcompat][bridge][issue-1434-tx][issue-1434-tx-p1]") {
    TestEnvironment env;
    // Install a recorder for setTransform so we can inspect args.
    // The element must be attached (`_nativeCreated`) before style
    // assignments forward to the bridge — match the pattern from
    // existing CSSStyleDeclaration tests.
    env.engine.evaluate(R"JS(
        var __setTransformCalls = [];
        setTransform = function(id, a, b, c, d, e, f) {
            __setTransformCalls.push([id, a, b, c, d, e, f]);
        };
        var el = document.createElement('div');
        document.body.appendChild(el);
        el.style.transform = 'matrix(0.866, 0.5, -0.5, 0.866, 100, 50)';
    )JS");

    auto numCalls = env.engine.evaluate("__setTransformCalls.length");
    REQUIRE(numCalls.getWithDefault<double>(0) == 1.0);

    // Verify all 6 components round-trip.
    auto a = env.engine.evaluate("__setTransformCalls[0][1]").getWithDefault<double>(0);
    auto b = env.engine.evaluate("__setTransformCalls[0][2]").getWithDefault<double>(0);
    auto c = env.engine.evaluate("__setTransformCalls[0][3]").getWithDefault<double>(0);
    auto d = env.engine.evaluate("__setTransformCalls[0][4]").getWithDefault<double>(0);
    auto e = env.engine.evaluate("__setTransformCalls[0][5]").getWithDefault<double>(0);
    auto f = env.engine.evaluate("__setTransformCalls[0][6]").getWithDefault<double>(0);
    REQUIRE_THAT(a, Catch::Matchers::WithinAbs(0.866, 0.001));
    REQUIRE_THAT(b, Catch::Matchers::WithinAbs(0.5,   0.001));
    REQUIRE_THAT(c, Catch::Matchers::WithinAbs(-0.5,  0.001));
    REQUIRE_THAT(d, Catch::Matchers::WithinAbs(0.866, 0.001));
    REQUIRE_THAT(e, Catch::Matchers::WithinAbs(100.0, 0.001));
    REQUIRE_THAT(f, Catch::Matchers::WithinAbs(50.0,  0.001));
}

TEST_CASE("WebCompat: matrix() with zero scale (a=b=0) preserves the collapse",
          "[webcompat][bridge][issue-1434-tx][issue-1434-tx-p2]") {
    // P2 fix: an intentional zero-scale collapse (a=b=0) used to be
    // masked by the decomposition computing sx=1 fallback. With direct
    // setTransform passthrough, a=b=0 reaches the bridge unchanged.
    TestEnvironment env;
    env.engine.evaluate(R"JS(
        var __setTransformCalls = [];
        setTransform = function(id, a, b, c, d, e, f) {
            __setTransformCalls.push([id, a, b, c, d, e, f]);
        };
        var el = document.createElement('div');
        document.body.appendChild(el);
        el.style.transform = 'matrix(0, 0, 0, 0, 100, 50)';
    )JS");
    auto a = env.engine.evaluate("__setTransformCalls[0][1]").getWithDefault<double>(-1.0);
    auto b = env.engine.evaluate("__setTransformCalls[0][2]").getWithDefault<double>(-1.0);
    auto c = env.engine.evaluate("__setTransformCalls[0][3]").getWithDefault<double>(-1.0);
    auto d = env.engine.evaluate("__setTransformCalls[0][4]").getWithDefault<double>(-1.0);
    REQUIRE_THAT(a, Catch::Matchers::WithinAbs(0.0, 0.001));
    REQUIRE_THAT(b, Catch::Matchers::WithinAbs(0.0, 0.001));
    REQUIRE_THAT(c, Catch::Matchers::WithinAbs(0.0, 0.001));
    REQUIRE_THAT(d, Catch::Matchers::WithinAbs(0.0, 0.001));
}

// ═══════════════════════════════════════════════════════════════════════════════
// pulp #1551 — CSS catalog Bundle 3: 13 already-implemented features
// ═══════════════════════════════════════════════════════════════════════════════
//
// One Catch2 case per catalog entry. Each one drives the existing
// implementation through the same path a consumer (Spectr's editor.js
// or @pulp/react JSX) would hit, so the assertions double as a
// regression net for the catalog entries.
//
// Scope deliberately narrow: smoke-level "does the existing
// implementation actually do what the catalog claims". Edge-case
// behaviour (multi-rule layering, focus/blur snapshot restore, var()
// nested fallbacks, etc.) is covered by neighbouring tests
// (test_css_hover_translation.cpp, test_selector_matching.cpp) — see
// the `tests` field on each compat.json entry.

TEST_CASE("WebCompat: :hover pseudo wires mouseenter/mouseleave (issue-1551)",
          "[webcompat][css-pseudo][issue-1551]") {
    TestEnvironment env;
    env.eval(R"JS(
        var __sheet = new StyleSheet({ '.h': { backgroundColor: 'red' } });
        // _setupPseudoHover only matches when parsed.pseudo === 'hover'.
        __sheet._parsedRules[0].parsed.pseudo = 'hover';
        __sheet.attach();
        var __hEl = document.createElement('div');
        __hEl.className = 'h';
        document.body.appendChild(__hEl);
    )JS");
    auto wired = env.engine.evaluate(
        "!!__hEl._hoverState && __hEl._hoverState.propsList.length === 1");
    REQUIRE(wired.getWithDefault<bool>(false) == true);
}

TEST_CASE("WebCompat: :focus pseudo wires focus/blur listeners (issue-1551)",
          "[webcompat][css-pseudo][issue-1551]") {
    TestEnvironment env;
    env.eval(R"JS(
        var __fEl = document.createElement('input');
        __fEl.className = 'f';
        document.body.appendChild(__fEl);
        _setupPseudoFocus(__fEl, { backgroundColor: 'red' });
    )JS");
    auto setup = env.engine.evaluate("__fEl._focusSetup === true");
    REQUIRE(setup.getWithDefault<bool>(false) == true);
}

TEST_CASE("WebCompat: :active pseudo wires mousedown/mouseup listeners (issue-1551)",
          "[webcompat][css-pseudo][issue-1551]") {
    TestEnvironment env;
    env.eval(R"JS(
        var __aEl = document.createElement('button');
        __aEl.className = 'a';
        document.body.appendChild(__aEl);
        _setupPseudoActive(__aEl, { backgroundColor: 'red' });
    )JS");
    auto setup = env.engine.evaluate("__aEl._activeSetup === true");
    REQUIRE(setup.getWithDefault<bool>(false) == true);
}

TEST_CASE("WebCompat: :disabled pseudo applies styles when el.disabled is set (issue-1551)",
          "[webcompat][css-pseudo][issue-1551]") {
    TestEnvironment env;
    env.eval(R"JS(
        var __dEl = document.createElement('button');
        __dEl.className = 'd';
        __dEl.disabled = true;
        var __dSheet = new StyleSheet({ '.d': { width: '42px' } });
        // Force the parsed pseudo to 'disabled' so _applyTo routes
        // through the :disabled branch.
        __dSheet._parsedRules[0].parsed.pseudo = 'disabled';
        __dSheet.attach();
        document.body.appendChild(__dEl);
    )JS");
    auto applied = env.engine.evaluate("__dEl.style._props.width");
    REQUIRE(std::string(applied.getWithDefault<std::string_view>("")) == "42px");
}

TEST_CASE("WebCompat: tag selector matches by lowercase tagName (issue-1551)",
          "[webcompat][css-selector][issue-1551]") {
    TestEnvironment env;
    env.eval(R"JS(
        var __tEl = document.createElement('button');
    )JS");
    auto matched = env.engine.evaluate(
        "_matchesSelector(__tEl, _parseSelector('button'))");
    auto rejected = env.engine.evaluate(
        "_matchesSelector(__tEl, _parseSelector('input'))");
    REQUIRE(matched.getWithDefault<bool>(false) == true);
    REQUIRE(rejected.getWithDefault<bool>(true) == false);
}

TEST_CASE("WebCompat: id selector matches via getAttribute('id') (issue-1551)",
          "[webcompat][css-selector][issue-1551]") {
    TestEnvironment env;
    env.eval(R"JS(
        var __iEl = document.createElement('div');
        __iEl.id = 'unique';
    )JS");
    auto matched = env.engine.evaluate(
        "_matchesSelector(__iEl, _parseSelector('#unique'))");
    auto rejected = env.engine.evaluate(
        "_matchesSelector(__iEl, _parseSelector('#other'))");
    REQUIRE(matched.getWithDefault<bool>(false) == true);
    REQUIRE(rejected.getWithDefault<bool>(true) == false);
}

TEST_CASE("WebCompat: class selector requires every class in classList (issue-1551)",
          "[webcompat][css-selector][issue-1551]") {
    TestEnvironment env;
    env.eval(R"JS(
        var __cEl = document.createElement('div');
        __cEl.className = 'foo bar';
    )JS");
    auto onlyFoo = env.engine.evaluate(
        "_matchesSelector(__cEl, _parseSelector('.foo'))");
    auto compound = env.engine.evaluate(
        "_matchesSelector(__cEl, _parseSelector('.foo.bar'))");
    auto missing = env.engine.evaluate(
        "_matchesSelector(__cEl, _parseSelector('.foo.baz'))");
    REQUIRE(onlyFoo.getWithDefault<bool>(false) == true);
    REQUIRE(compound.getWithDefault<bool>(false) == true);
    REQUIRE(missing.getWithDefault<bool>(true) == false);
}

TEST_CASE("WebCompat: descendant combinator walks ancestor chain (issue-1551)",
          "[webcompat][css-selector][issue-1551]") {
    TestEnvironment env;
    env.eval(R"JS(
        var __gp = document.createElement('section');
        var __p = document.createElement('div');
        var __c = document.createElement('span');
        __gp._children.push(__p);
        __p._parentElement = __gp;
        __p._children.push(__c);
        __c._parentElement = __p;
    )JS");
    auto descendant = env.engine.evaluate(
        "_matchesSelector(__c, _parseSelector('section span'))");
    REQUIRE(descendant.getWithDefault<bool>(false) == true);
}

TEST_CASE("WebCompat: child combinator requires immediate parent (issue-1551)",
          "[webcompat][css-selector][issue-1551]") {
    TestEnvironment env;
    env.eval(R"JS(
        var __gp2 = document.createElement('section');
        var __p2 = document.createElement('div');
        var __c2 = document.createElement('span');
        __gp2._children.push(__p2);
        __p2._parentElement = __gp2;
        __p2._children.push(__c2);
        __c2._parentElement = __p2;
    )JS");
    auto direct = env.engine.evaluate(
        "_matchesSelector(__c2, _parseSelector('div > span'))");
    auto skip = env.engine.evaluate(
        "_matchesSelector(__c2, _parseSelector('section > span'))");
    REQUIRE(direct.getWithDefault<bool>(false) == true);
    REQUIRE(skip.getWithDefault<bool>(true) == false);
}

TEST_CASE("WebCompat: _matchMediaQuery resolves @media-style queries (issue-1551)",
          "[webcompat][matchmedia][issue-1551]") {
    // The catalog tracks `_matchMediaQuery()` (loaded with css-parser prelude),
    // not `window.matchMedia()` — the latter lives in core/view/js/web-compat.js
    // which is not part of the WidgetBridge prelude chain. Stylesheets and
    // matched-media @rules go through `_matchMediaQuery` directly. The shape
    // tested here is what the engine actually exposes today.
    TestEnvironment env(800, 600);
    auto match_min = env.engine.evaluate(
        "_matchMediaQuery('(min-width: 600px)')");
    auto miss_min = env.engine.evaluate(
        "_matchMediaQuery('(min-width: 1200px)')");
    auto orient = env.engine.evaluate(
        "_matchMediaQuery('(orientation: landscape)')");
    REQUIRE(match_min.getWithDefault<bool>(false) == true);
    REQUIRE(miss_min.getWithDefault<bool>(true) == false);
    REQUIRE(orient.getWithDefault<bool>(false) == true);
}

TEST_CASE("WebCompat: var(--name) resolves through motion-token bridge (issue-1551)",
          "[webcompat][css-var][issue-1551]") {
    TestEnvironment env;
    env.eval("setMotionToken('v1551-w', 64);");
    auto resolved = env.engine.evaluate("_resolveVar('var(--v1551-w)')");
    auto withFallback = env.engine.evaluate("_resolveVar('var(--v1551-missing, 12)')");
    REQUIRE(std::string(resolved.getWithDefault<std::string_view>("")) == "64");
    REQUIRE(std::string(withFallback.getWithDefault<std::string_view>("")) == "12");
}

TEST_CASE("WebCompat: setProperty('--name') routes to motion-token bridge (issue-1551)",
          "[webcompat][css-var][issue-1551]") {
    TestEnvironment env;
    env.eval(R"JS(
        var __spEl = document.createElement('div');
        __spEl.style.setProperty('--v1551-h', '128px');
    )JS");
    auto roundTrip = env.engine.evaluate("getMotionToken('v1551-h')");
    REQUIRE(roundTrip.getWithDefault<double>(-1.0) == 128.0);
}

TEST_CASE("WebCompat: StyleSheet attach applies rules to existing elements (issue-1551)",
          "[webcompat][stylesheet][issue-1551]") {
    TestEnvironment env;
    env.eval(R"JS(
        var __ssEl = document.createElement('div');
        __ssEl.className = 'ss-1551';
        document.body.appendChild(__ssEl);
        var __ssSheet = new StyleSheet({ '.ss-1551': { width: '99px' } });
        __ssSheet.attach();
    )JS");
    auto applied = env.engine.evaluate("__ssEl.style._props.width");
    auto attached = env.engine.evaluate("__ssSheet._attached");
    REQUIRE(std::string(applied.getWithDefault<std::string_view>("")) == "99px");
    REQUIRE(attached.getWithDefault<bool>(false) == true);

    // Detach is the second half of the catalog claim.
    env.eval("__ssSheet.detach();");
    auto detached = env.engine.evaluate("__ssSheet._attached");
    REQUIRE(detached.getWithDefault<bool>(true) == false);
}

// pulp #1737 (Codex P2 followup #2 on #1773): :root pseudo-class
// must remain wired in _matchesPseudoClass for stylesheet matching.
// Codex caught a regression where my earlier removal (intended only
// for the querySelector path, where :root is unreachable due to
// _findMatch starting from root._children) silently broke
// `:root { ... }` style application via StyleSheet._applyTo. CSS
// token / theme patterns regressed.
TEST_CASE("WebCompat: :root pseudo applies stylesheet rules to body element",
          "[webcompat][css-pseudo][issue-1737][issue-1773-followup]") {
    TestEnvironment env;
    env.eval(R"JS(
        var __rSheet = new StyleSheet({ ':root': { width: '7px' } });
        // Emulate the parser's pseudo extraction — _parseSelector strips
        // the leading colon, so the stored pseudo is "root".
        __rSheet._parsedRules[0].parsed.pseudo = 'root';
        __rSheet.attach();
    )JS");
    // The body element has no parent in the DOM-lite tree, so the
    // :root branch in _matchesPseudoClass returns true for it. After
    // attach() walks all elements, the rule should be applied.
    auto applied = env.engine.evaluate("__bodyElement__.style._props.width");
    REQUIRE(std::string(applied.getWithDefault<std::string_view>("")) == "7px");
}
