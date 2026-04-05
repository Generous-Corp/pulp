// Web-compat prelude tests — validates the JS prelude layer
// Tests CSS parsing, named colors, document API, classList, querySelector,
// and DOM operations (appendChild, removeChild, getElementById, etc.).

#include <catch2/catch_test_macros.hpp>
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
