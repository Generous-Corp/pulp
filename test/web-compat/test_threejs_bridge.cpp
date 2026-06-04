#include <catch2/catch_test_macros.hpp>
#include "test_helpers.hpp"
#if defined(__APPLE__)
#include "../mac_window_harness.hpp"
#include <TargetConditionals.h>
#endif
#include "include/core/SkData.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include <pulp/canvas/skia_canvas.hpp>
#include <pulp/render/skia_surface.hpp>
#include <pulp/view/canvas_widget.hpp>
#include <pulp/view/js_engine.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/widget_bridge.hpp>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

using namespace pulp::view;

namespace {

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::optional<std::string> resolve_threejs_module(std::string_view path) {
    const auto root = std::filesystem::path(PULP_THREEJS_SOURCE_DIR);
    if (path == "three") {
        return read_text_file(root / "build" / "three.module.js");
    }
    if (path == "three/webgpu") {
        return read_text_file(root / "build" / "three.webgpu.js");
    }
    if (path == "./three.core.js" || path == "three.core.js") {
        return read_text_file(root / "build" / "three.core.js");
    }
    if (path == "three/addons/controls/OrbitControls.js") {
        return read_text_file(root / "examples" / "jsm" / "controls" / "OrbitControls.js");
    }
    if (path == "three/addons/loaders/GLTFLoader.js") {
        return read_text_file(root / "examples" / "jsm" / "loaders" / "GLTFLoader.js");
    }
    if (path == "../utils/BufferGeometryUtils.js" || path == "three/addons/utils/BufferGeometryUtils.js") {
        return read_text_file(root / "examples" / "jsm" / "utils" / "BufferGeometryUtils.js");
    }
    if (path == "../utils/SkeletonUtils.js" || path == "three/addons/utils/SkeletonUtils.js") {
        return read_text_file(root / "examples" / "jsm" / "utils" / "SkeletonUtils.js");
    }
    return std::nullopt;
}

struct NativeV8Environment {
    View root;
    ScriptEngine engine;
    pulp::state::StateStore store;
    std::unique_ptr<pulp::render::GpuSurface> gpu_surface;
    std::unique_ptr<WidgetBridge> bridge;

    NativeV8Environment(float width, float height)
        : engine(JsEngineType::v8) {
        root.set_bounds({0, 0, width, height});
        root.set_theme(Theme::dark());

        gpu_surface = pulp::render::GpuSurface::create_dawn();
        if (gpu_surface) {
            pulp::render::GpuSurface::Config config{};
            config.width = static_cast<uint32_t>(std::max(1.0f, width));
            config.height = static_cast<uint32_t>(std::max(1.0f, height));
            config.native_surface_handle = nullptr;
            if (!gpu_surface->initialize(config)) {
                gpu_surface.reset();
            }
        }

        bridge = std::make_unique<WidgetBridge>(engine, root, store, gpu_surface.get());
    }

    bool has_native_gpu() const { return gpu_surface != nullptr; }
};

std::string eval_string(ScriptEngine& engine, const std::string& code) {
    return std::string(engine.evaluate(code).getWithDefault<std::string_view>(""));
}

int32_t eval_i32(ScriptEngine& engine, const std::string& code) {
    return engine.evaluate(code).getWithDefault<int32_t>(0);
}

std::vector<uint8_t> make_minimal_glb() {
    std::string json = R"({"asset":{"version":"2.0","generator":"pulp-test"},"scene":0,"scenes":[{"nodes":[]}],"nodes":[]})";
    while ((json.size() % 4) != 0) {
        json.push_back(' ');
    }

    const uint32_t total_length = static_cast<uint32_t>(12 + 8 + json.size());
    std::vector<uint8_t> bytes;
    bytes.reserve(total_length);

    auto append_u32 = [&](uint32_t value) {
        bytes.push_back(static_cast<uint8_t>(value & 0xff));
        bytes.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
        bytes.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
        bytes.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
    };

    append_u32(0x46546c67); // glTF
    append_u32(2);
    append_u32(total_length);
    append_u32(static_cast<uint32_t>(json.size()));
    append_u32(0x4e4f534a); // JSON
    bytes.insert(bytes.end(), json.begin(), json.end());
    return bytes;
}

void write_binary_file(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    REQUIRE(out.good());
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(out.good());
}

struct PngColorStats {
    int width = 0;
    int height = 0;
    int red_pixels = 0;
    int bright_pixels = 0;
};

PngColorStats sample_png_colors(const std::vector<uint8_t>& png) {
    PngColorStats stats;
    auto data = SkData::MakeWithCopy(png.data(), png.size());
    auto image = SkImages::DeferredFromEncodedData(data);
    REQUIRE(image != nullptr);

    stats.width = image->width();
    stats.height = image->height();
    REQUIRE(stats.width > 0);
    REQUIRE(stats.height > 0);

    std::vector<uint8_t> pixels(static_cast<size_t>(stats.width) * static_cast<size_t>(stats.height) * 4u);
    const auto info = SkImageInfo::Make(stats.width,
                                        stats.height,
                                        kRGBA_8888_SkColorType,
                                        kPremul_SkAlphaType);
    REQUIRE(image->readPixels(info, pixels.data(), static_cast<size_t>(stats.width) * 4u, 0, 0));

    for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
        const auto r = static_cast<int>(pixels[i + 0]);
        const auto g = static_cast<int>(pixels[i + 1]);
        const auto b = static_cast<int>(pixels[i + 2]);
        const auto a = static_cast<int>(pixels[i + 3]);
        if (a > 128 && r > 140 && g < 120 && b < 120) {
            ++stats.red_pixels;
        }
        if (a > 128 && (r > 80 || g > 80 || b > 80)) {
            ++stats.bright_pixels;
        }
    }
    return stats;
}

} // namespace

// iOS-D.3c (#3217) regression: GPUQueue.writeBuffer's `dataOffset` and
// `size` are measured in ELEMENTS when `data` is a TypedArray, not bytes
// (only ArrayBuffer/DataView use byte units). The mock previously treated
// them as bytes, so Three.js's `writeBuffer(buf, dst, Float32Array(16), 0,
// 16)` — 16 floats = 64 bytes — copied only 16 BYTES = 4 floats, the first
// column of each mat4x4. The full-array form (size undefined) was already
// correct, which is why the [phase13] smoke stayed green; this pins the
// explicit element-count form that the smoke never exercises.
TEST_CASE("WebGPU mock writeBuffer: TypedArray dataOffset/size are in elements not bytes",
          "[webcompat][gpu][mock][issue-3217]") {
    if (!is_engine_available(JsEngineType::v8)) {
        SKIP("V8 is required for the WebGPU mock prelude harness");
    }
    // GPU is not required — the mock prelude (__createMockGPUDevice etc.)
    // is installed by WidgetBridge regardless of whether Dawn is available.
    NativeV8Environment env(64, 64);
    env.bridge->load_script("");

    // 4 floats = 16 bytes; size=4 (element count). All four floats must land.
    const auto full = eval_string(env.engine, R"JS(
        (() => {
            const dev = __createMockGPUDevice(__createMockGPUAdapter({}), {}, {});
            const buf = dev.createBuffer({ size: 16, usage: 0 });
            const f = new Float32Array([1, 2, 3, 4]);
            dev.queue.writeBuffer(buf, 0, f, 0, f.length);
            const v = new Float32Array(buf._bytes.buffer, buf._bytes.byteOffset, 4);
            return Array.from(v).join(',');
        })()
    )JS");
    INFO("writeBuffer full-array (element size) => " << full);
    REQUIRE(full == "1,2,3,4");  // bug truncates to "1,0,0,0"

    // dataOffset=2 elements (skip 10,20), size=2 elements (write 30,40).
    const auto sliced = eval_string(env.engine, R"JS(
        (() => {
            const dev = __createMockGPUDevice(__createMockGPUAdapter({}), {}, {});
            const buf = dev.createBuffer({ size: 8, usage: 0 });
            const f = new Float32Array([10, 20, 30, 40]);
            dev.queue.writeBuffer(buf, 0, f, 2, 2);
            const v = new Float32Array(buf._bytes.buffer, buf._bytes.byteOffset, 2);
            return Array.from(v).join(',');
        })()
    )JS");
    INFO("writeBuffer element dataOffset/size => " << sliced);
    REQUIRE(sliced == "30,40");  // bug reads bytes 2..4 -> garbage
}

TEST_CASE("Three.js bridge resolves the real GLTFLoader addon module", "[threejs][gpu][gltf][loader]") {
    if (!is_engine_available(JsEngineType::v8)) {
        SKIP("V8 is required for native Three.js module smoke");
    }

    NativeV8Environment env(64, 64);
    env.bridge->load_script("");

    bool resolved_loader = false;
    bool resolved_buffer_utils = false;
    bool resolved_skeleton_utils = false;
    bool module_completed = false;
    std::string module_error;

    env.engine.run_module(R"JS(
        import { GLTFLoader } from 'three/addons/loaders/GLTFLoader.js';
        const loader = new GLTFLoader();
        globalThis.__pulpGltfLoaderProbe = {
            loaderType: typeof GLTFLoader,
            loaderName: GLTFLoader.name,
            parseType: typeof GLTFLoader.prototype.parse,
            loadType: typeof GLTFLoader.prototype.load,
            setPathType: typeof loader.setPath,
            setResourcePathType: typeof loader.setResourcePath,
            hasLoadingManager: !!loader.manager
        };

        export default globalThis.__pulpGltfLoaderProbe;
    )JS",
    [&](std::string_view path) -> std::optional<std::string> {
        if (path == "three/addons/loaders/GLTFLoader.js") {
            resolved_loader = true;
        } else if (path == "../utils/BufferGeometryUtils.js" || path == "three/addons/utils/BufferGeometryUtils.js") {
            resolved_buffer_utils = true;
        } else if (path == "../utils/SkeletonUtils.js" || path == "three/addons/utils/SkeletonUtils.js") {
            resolved_skeleton_utils = true;
        }
        return resolve_threejs_module(path);
    },
    [&](const std::string& error, const choc::value::Value&) {
        module_completed = true;
        module_error = error;
    });

    for (int i = 0; i < 64; ++i) {
        env.engine.pump_message_loop();
        if (module_completed) {
            break;
        }
    }

    INFO(module_error);
    REQUIRE(module_completed);
    REQUIRE(module_error.empty());
    REQUIRE(resolved_loader);
    REQUIRE(resolved_buffer_utils);
    REQUIRE(resolved_skeleton_utils);

    const auto probe = eval_string(env.engine, "JSON.stringify(globalThis.__pulpGltfLoaderProbe || {})");
    INFO(probe);
    REQUIRE(probe.find("\"loaderType\":\"function\"") != std::string::npos);
    REQUIRE(probe.find("\"loaderName\":\"GLTFLoader\"") != std::string::npos);
    REQUIRE(probe.find("\"parseType\":\"function\"") != std::string::npos);
    REQUIRE(probe.find("\"loadType\":\"function\"") != std::string::npos);
    REQUIRE(probe.find("\"setPathType\":\"function\"") != std::string::npos);
    REQUIRE(probe.find("\"setResourcePathType\":\"function\"") != std::string::npos);
    REQUIRE(probe.find("\"hasLoadingManager\":true") != std::string::npos);
}

TEST_CASE("Three.js GLB fetch contract reads local bytes before GLTF parse", "[threejs][gpu][gltf][fetch]") {
    if (!is_engine_available(JsEngineType::v8)) {
        SKIP("V8 is required for native Three.js GLB fetch smoke");
    }

    const auto glb_path = std::filesystem::temp_directory_path()
        / "pulp-threejs-gltf-fetch" / "BoxTextured.glb";
    const auto glb_bytes = make_minimal_glb();
    write_binary_file(glb_path, glb_bytes);

    NativeV8Environment env(64, 64);
    env.bridge->load_script("");

    const auto generic_path = glb_path.generic_string();
    const std::string file_url = std::string("file://")
        + (generic_path.empty() || generic_path.front() == '/' ? "" : "/")
        + generic_path;

    bool module_completed = false;
    std::string module_error;
    std::string script = R"JS(
        import { GLTFLoader } from 'three/addons/loaders/GLTFLoader.js';

        globalThis.__pulpGltfFetchState = {
            status: 'start',
            url: '__PULP_GLB_URL__',
            expectedByteLength: __PULP_GLB_BYTE_LENGTH__
        };

        globalThis.__pulpGltfFetchTask = (async () => {
            const state = globalThis.__pulpGltfFetchState;
            state.status = 'fetch-start';
            if (typeof fetch !== 'function') {
                throw new Error('fetch unavailable');
            }
            if (typeof XMLHttpRequest === 'function') {
                state.xhrAvailable = true;
            }

            const response = await fetch(state.url);
            state.status = 'fetch-response';
            state.fetchOk = !!response.ok;
            state.statusCode = response.status;
            state.contentType = response.headers && response.headers.get
                ? String(response.headers.get('content-type') || '')
                : '';
            if (!response.ok) {
                throw new Error('fetch failed status=' + response.status);
            }

            const buffer = await response.arrayBuffer();
            state.status = 'bytes-ready';
            state.byteLength = buffer ? buffer.byteLength : 0;
            if (state.byteLength !== state.expectedByteLength) {
                throw new Error('GLB byte length mismatch via fetch: expected '
                    + state.expectedByteLength + ', got ' + state.byteLength);
            }

            await new Promise((resolve, reject) => {
                const loader = new GLTFLoader();
                loader.parse(buffer, '', (gltf) => {
                    state.status = 'parsed';
                    state.sceneCount = gltf && gltf.scenes ? gltf.scenes.length : -1;
                    state.defaultSceneType = gltf && gltf.scene ? gltf.scene.type : '';
                    resolve();
                }, (error) => {
                    state.status = 'parse-error';
                    reject(new Error('GLTFLoader.parse blocked after fetch: '
                        + (error && error.message ? error.message : String(error))));
                });
            });

            return state;
        })().catch((error) => {
            const state = globalThis.__pulpGltfFetchState || {};
            state.status = 'error';
            state.message = error && error.message ? String(error.message) : String(error);
            state.stack = error && error.stack ? String(error.stack) : '';
            globalThis.__pulpGltfFetchState = state;
        });

        export default globalThis.__pulpGltfFetchState;
    )JS";

    const auto url_pos = script.find("__PULP_GLB_URL__");
    REQUIRE(url_pos != std::string::npos);
    script.replace(url_pos, std::string("__PULP_GLB_URL__").size(), file_url);

    const auto length_pos = script.find("__PULP_GLB_BYTE_LENGTH__");
    REQUIRE(length_pos != std::string::npos);
    script.replace(length_pos,
                   std::string("__PULP_GLB_BYTE_LENGTH__").size(),
                   std::to_string(glb_bytes.size()));

    env.engine.run_module(script,
    resolve_threejs_module,
    [&](const std::string& error, const choc::value::Value&) {
        module_completed = true;
        module_error = error;
    });

    for (int i = 0; i < 256; ++i) {
        env.engine.pump_message_loop();
        const auto status = eval_string(env.engine, "globalThis.__pulpGltfFetchState && globalThis.__pulpGltfFetchState.status || ''");
        if (status == "parsed" || status == "error") {
            break;
        }
    }

    INFO(module_error);
    REQUIRE(module_completed);
    REQUIRE(module_error.empty());

    const auto state = eval_string(env.engine, "JSON.stringify(globalThis.__pulpGltfFetchState || {})");
    INFO(state);
    REQUIRE(state.find("\"status\":\"parsed\"") != std::string::npos);
    REQUIRE(state.find("\"url\":\"" + file_url + "\"") != std::string::npos);
    REQUIRE(state.find("\"fetchOk\":true") != std::string::npos);
    REQUIRE(state.find("\"contentType\":\"application/octet-stream\"") != std::string::npos);
    REQUIRE(state.find("\"byteLength\":" + std::to_string(glb_bytes.size())) != std::string::npos);
    REQUIRE(state.find("\"sceneCount\":1") != std::string::npos);
    REQUIRE(state.find("\"defaultSceneType\":\"Group\"") != std::string::npos);

    std::filesystem::remove(glb_path);
}

TEST_CASE("Three.js DataTexture material samples uploaded texture bytes on native WebGPU", "[threejs][gpu][texture][datatexture]") {
    if (!is_engine_available(JsEngineType::v8)) {
        SKIP("V8 is required for native Three.js texture smoke");
    }

    NativeV8Environment env(128, 128);
    if (!env.has_native_gpu()) {
        SKIP("Native Dawn adapter unavailable on this host/backend");
    }

    env.bridge->load_script("");

    bool module_completed = false;
    std::string module_error;
    env.engine.run_module(R"JS(
        import * as THREE from 'three/webgpu';

        const canvas = document.createElement('canvas');
        canvas.id = 'threejs-datatexture-canvas';
        canvas.width = 96;
        canvas.height = 96;
        document.body.appendChild(canvas);

        const context = canvas.getContext('webgpu');
        const renderer = new THREE.WebGPURenderer({ canvas, context, antialias: false });
        await renderer.init();
        renderer.setSize(96, 96, false);

        const scene = new THREE.Scene();
        scene.background = new THREE.Color(0x020617);
        const camera = new THREE.OrthographicCamera(-1, 1, 1, -1, 0.1, 10);
        camera.position.z = 2;

        const texture = new THREE.DataTexture(
            new Uint8Array([255, 32, 16, 255]),
            1,
            1,
            THREE.RGBAFormat
        );
        texture.colorSpace = THREE.NoColorSpace;
        texture.magFilter = THREE.NearestFilter;
        texture.minFilter = THREE.NearestFilter;
        texture.needsUpdate = true;

        const mesh = new THREE.Mesh(
            new THREE.PlaneGeometry(1.4, 1.4),
            new THREE.MeshBasicMaterial({ map: texture, color: 0xffffff, side: THREE.DoubleSide })
        );
        scene.add(mesh);

        renderer.render(scene, camera);
        if (typeof context.present === 'function') context.present();
        renderer.render(scene, camera);
        if (typeof context.present === 'function') context.present();

        globalThis.__pulpDataTextureState = {
            status: 'ready',
            backend: renderer.backend && renderer.backend.constructor ? renderer.backend.constructor.name : '',
            contextType: renderer.getContext() && renderer.getContext()._objectName ? renderer.getContext()._objectName : '',
            textureBytes: texture.image && texture.image.data ? texture.image.data.length : 0
        };

        export default true;
    )JS", resolve_threejs_module,
    [&](const std::string& error, const choc::value::Value&) {
        module_completed = true;
        module_error = error;
    });

    for (int i = 0; i < 256; ++i) {
        env.engine.pump_message_loop();
        if (eval_string(env.engine, "globalThis.__pulpDataTextureState.status") == "ready") {
            break;
        }
    }

    REQUIRE(module_completed);
    REQUIRE(module_error.empty());
    INFO(eval_string(env.engine, "JSON.stringify(globalThis.__pulpDataTextureState || {})"));
    REQUIRE(eval_string(env.engine, "globalThis.__pulpDataTextureState.status") == "ready");
    REQUIRE(eval_string(env.engine, "globalThis.__pulpDataTextureState.contextType") == "GPUCanvasContext");
    REQUIRE(eval_i32(env.engine, "globalThis.__pulpDataTextureState.textureBytes") == 4);

    env.root.layout_children();

    const auto native_id = eval_string(env.engine, "document.getElementById('threejs-datatexture-canvas')._id");
    auto* widget = dynamic_cast<CanvasWidget*>(env.bridge->widget(native_id));
    REQUIRE(widget != nullptr);

    auto skia = pulp::render::SkiaSurface::create(*env.gpu_surface, {.width = 96, .height = 96});
    REQUIRE(skia != nullptr);
    REQUIRE(skia->is_available());
    REQUIRE(env.gpu_surface->begin_frame());

    auto* canvas = skia->begin_frame();
    REQUIRE(canvas != nullptr);
    widget->paint(*canvas);
    REQUIRE(widget->last_native_gpu_texture_draw_succeeded());
    skia->end_frame();

    std::vector<uint8_t> pixels;
    uint32_t pixel_width = 0;
    uint32_t pixel_height = 0;
    REQUIRE(skia->read_current_rgba(pixels, pixel_width, pixel_height));
    env.gpu_surface->end_frame();

    REQUIRE(pixel_width == 96);
    REQUIRE(pixel_height == 96);
    const auto center = ((pixel_height / 2u) * pixel_width + (pixel_width / 2u)) * 4u;
    REQUIRE(center + 3 < pixels.size());
    const auto r = static_cast<int>(pixels[center + 0]);
    const auto g = static_cast<int>(pixels[center + 1]);
    const auto b = static_cast<int>(pixels[center + 2]);
    INFO("datatexture-center-r=" << r);
    INFO("datatexture-center-g=" << g);
    INFO("datatexture-center-b=" << b);
    REQUIRE(r > 160);
    REQUIRE(g < 100);
    REQUIRE(b < 80);
}

#if defined(__APPLE__) && TARGET_OS_OSX
TEST_CASE("Three.js bridge canvas is captured by hidden macOS WindowHost back buffer",
          "[threejs][gpu][host][mac][capture]") {
    if (!is_engine_available(JsEngineType::v8)) {
        SKIP("V8 is required for native Three.js host capture smoke");
    }

    View root;
    root.set_bounds({0, 0, 180, 140});
    root.set_theme(Theme::dark());

    WindowOptions opts;
    opts.title = "Three.js host capture smoke";
    opts.width = 180;
    opts.height = 140;
    opts.use_gpu = true;
    opts.initially_hidden = true;

    auto host = pulp::test::mac::make_test_window(root, opts);
    if (!host) {
        SKIP("Hidden macOS GPU WindowHost unavailable on this host/backend");
    }
    REQUIRE(host->gpu_surface() != nullptr);

    ScriptEngine engine(JsEngineType::v8);
    pulp::state::StateStore store;
    WidgetBridge bridge(engine, root, store, host->gpu_surface());
    bridge.set_repaint_callback([host_ptr = host.get()] {
        if (host_ptr) host_ptr->repaint();
    });
    bridge.load_script("");

    bool module_completed = false;
    std::string module_error;
    engine.run_module(R"JS(
        import * as THREE from 'three/webgpu';

        class PulpCanvas {
            constructor(canvas) {
                this._canvas = canvas;
                this.style = canvas.style || {};
            }

            get width() { return this._canvas.width; }
            set width(value) { this._canvas.width = value; }

            get height() { return this._canvas.height; }
            set height(value) { this._canvas.height = value; }

            get clientWidth() { return this._canvas.width; }
            get clientHeight() { return this._canvas.height; }

            addEventListener(type, fn, opts) { return this._canvas.addEventListener(type, fn, opts); }
            removeEventListener(type, fn, opts) { return this._canvas.removeEventListener(type, fn, opts); }
            dispatchEvent(event) { return this._canvas.dispatchEvent(event); }
            setPointerCapture(pointerId) { return this._canvas.setPointerCapture(pointerId); }
            releasePointerCapture(pointerId) { return this._canvas.releasePointerCapture(pointerId); }
        }

        document.body.style.backgroundColor = '#020617';
        document.body.style.width = '180px';
        document.body.style.height = '140px';
        document.body.style.padding = '12px';

        const canvas = document.createElement('canvas');
        canvas.id = 'threejs-host-capture-canvas';
        canvas.width = 96;
        canvas.height = 96;
        canvas.style.width = '96px';
        canvas.style.height = '96px';
        document.body.appendChild(canvas);

        const context = canvas.getContext('webgpu');
        const renderer = new THREE.WebGPURenderer({
            canvas: new PulpCanvas(canvas),
            context,
            antialias: false
        });
        await renderer.init();
        renderer.setSize(96, 96, false);

        const scene = new THREE.Scene();
        scene.background = new THREE.Color(1, 0, 0);
        const camera = new THREE.PerspectiveCamera(70, 1, 0.1, 10);
        camera.position.z = 2;

        const mesh = new THREE.Mesh(
            new THREE.BoxGeometry(0.75, 0.75, 0.75),
            new THREE.MeshBasicMaterial({ color: 0x00ff00 })
        );
        mesh.rotation.x = 0.4;
        mesh.rotation.y = 0.6;
        scene.add(mesh);

        renderer.render(scene, camera);
        if (typeof context.present === 'function') context.present();
        renderer.render(scene, camera);
        if (typeof context.present === 'function') context.present();

        globalThis.__pulpHostCaptureState = {
            status: 'ready',
            backend: renderer.backend && renderer.backend.constructor ? renderer.backend.constructor.name : '',
            contextType: renderer.getContext() && renderer.getContext()._objectName ? renderer.getContext()._objectName : '',
            bufferedSkipCount: (globalThis.__phase13BufferedSkips || []).length
        };

        export default true;
    )JS", resolve_threejs_module,
    [&](const std::string& error, const choc::value::Value&) {
        module_completed = true;
        module_error = error;
    });

    for (int i = 0; i < 256; ++i) {
        engine.pump_message_loop();
        if (eval_string(engine, "globalThis.__pulpHostCaptureState.status") == "ready") {
            break;
        }
    }

    REQUIRE(module_completed);
    REQUIRE(module_error.empty());
    INFO(eval_string(engine, "JSON.stringify(globalThis.__pulpHostCaptureState || {})"));
    REQUIRE(eval_string(engine, "globalThis.__pulpHostCaptureState.status") == "ready");
    REQUIRE(eval_string(engine, "globalThis.__pulpHostCaptureState.contextType") == "GPUCanvasContext");
    REQUIRE(eval_i32(engine, "globalThis.__pulpHostCaptureState.bufferedSkipCount") == 0);

    root.layout_children();
    const auto native_id = eval_string(engine, "document.getElementById('threejs-host-capture-canvas')._id");
    auto* widget = dynamic_cast<CanvasWidget*>(bridge.widget(native_id));
    REQUIRE(widget != nullptr);
    REQUIRE(widget->bounds().width > 0.0f);
    REQUIRE(widget->bounds().height > 0.0f);

    const auto png = pulp::test::mac::capture_back_buffer_png(*host);
    INFO("host capture png bytes=" << png.size());
    REQUIRE(png.size() > 8);
    REQUIRE(png[0] == 0x89);
    REQUIRE(png[1] == 0x50);
    REQUIRE(png[2] == 0x4e);
    REQUIRE(png[3] == 0x47);
    REQUIRE(widget->last_native_gpu_texture_draw_succeeded());

    const auto stats = sample_png_colors(png);
    INFO("host capture png " << stats.width << "x" << stats.height
         << " red_pixels=" << stats.red_pixels
         << " bright_pixels=" << stats.bright_pixels);
    REQUIRE(stats.width >= 180);
    REQUIRE(stats.height >= 140);
    REQUIRE(stats.red_pixels > 100);
    REQUIRE(stats.bright_pixels > 100);
}
#endif

TEST_CASE("Three.js GLTF scene keeps resize, pointer drag, and wheel input live", "[threejs][gpu][gltf][input]") {
    if (!is_engine_available(JsEngineType::v8)) {
        SKIP("V8 is required for native Three.js GLTF input smoke");
    }

    NativeV8Environment env(180, 140);
    if (!env.has_native_gpu()) {
        SKIP("Native Dawn adapter unavailable on this host/backend");
    }

    const auto glb_path = std::filesystem::temp_directory_path()
        / "pulp-threejs-gltf-input" / "BoxTextured.glb";
    const auto glb_bytes = make_minimal_glb();
    write_binary_file(glb_path, glb_bytes);

    const auto generic_path = glb_path.generic_string();
    const std::string file_url = std::string("file://")
        + (generic_path.empty() || generic_path.front() == '/' ? "" : "/")
        + generic_path;

    env.bridge->load_script("");

    bool module_completed = false;
    std::string module_error;
    std::string script = R"JS(
        import * as THREE from 'three/webgpu';
        import { OrbitControls } from 'three/addons/controls/OrbitControls.js';
        import { GLTFLoader } from 'three/addons/loaders/GLTFLoader.js';

        class PulpCanvas {
            constructor(canvas) {
                this._canvas = canvas;
                this.style = canvas.style || {};
            }
            get width() { return this._canvas.width; }
            set width(value) { this._canvas.width = value; }
            get height() { return this._canvas.height; }
            set height(value) { this._canvas.height = value; }
            get clientWidth() { return this._canvas.width; }
            get clientHeight() { return this._canvas.height; }
            addEventListener(type, fn, opts) { return this._canvas.addEventListener(type, fn, opts); }
            removeEventListener(type, fn, opts) { return this._canvas.removeEventListener(type, fn, opts); }
            dispatchEvent(event) { return this._canvas.dispatchEvent(event); }
            setPointerCapture(pointerId) {
                if (typeof this._canvas.setPointerCapture === 'function') {
                    return this._canvas.setPointerCapture(pointerId);
                }
            }
            releasePointerCapture(pointerId) {
                if (typeof this._canvas.releasePointerCapture === 'function') {
                    return this._canvas.releasePointerCapture(pointerId);
                }
            }
        }

        function dispatch(target, event) {
            event.bubbles = event.bubbles !== false;
            event.cancelable = true;
            event.preventDefault = event.preventDefault || function() { this.defaultPrevented = true; };
            event.stopPropagation = event.stopPropagation || function() {};
            return target.dispatchEvent(event);
        }

        globalThis.__pulpGltfInputState = { status: 'start', url: '__PULP_GLB_URL__' };
        globalThis.__phase13BufferedSkips = [];

        globalThis.__pulpGltfInputTask = (async () => {
            const state = globalThis.__pulpGltfInputState;
            state.status = 'fetch-start';
            const response = await fetch(state.url);
            if (!response.ok) throw new Error('fetch failed status=' + response.status);
            const buffer = await response.arrayBuffer();
            state.byteLength = buffer.byteLength;

            const gltf = await new Promise((resolve, reject) => {
                new GLTFLoader().parse(buffer, '', resolve, (error) => {
                    reject(new Error('GLTFLoader.parse failed: '
                        + (error && error.message ? error.message : String(error))));
                });
            });
            state.status = 'gltf-ready';

            const canvas = document.createElement('canvas');
            canvas.id = 'pulp-gltf-input-canvas';
            canvas.width = 96;
            canvas.height = 96;
            document.body.appendChild(canvas);
            const wrappedCanvas = new PulpCanvas(canvas);
            const context = canvas.getContext('webgpu');
            const renderer = new THREE.WebGPURenderer({ canvas: wrappedCanvas, context, antialias: false });
            await renderer.init();

            const scene = new THREE.Scene();
            scene.background = new THREE.Color(0x111827);
            const camera = new THREE.PerspectiveCamera(70, 1, 0.1, 10);
            camera.position.z = 2.25;
            const controls = new OrbitControls(camera, canvas);
            controls.enableDamping = true;
            controls.enablePan = false;

            const object = gltf.scene || new THREE.Group();
            object.name = 'PulpGltfInputObject';
            object.rotation.x = 0.25;
            object.rotation.y = 0.4;
            scene.add(object);
            controls.target.set(0, 0, 0);
            controls.update();

            let dragging = false;
            let lastX = 0;
            let lastY = 0;
            let dragEvents = 0;
            let wheelEvents = 0;
            canvas.addEventListener('pointerdown', (event) => {
                dragging = true;
                lastX = event.clientX || event.offsetX || 0;
                lastY = event.clientY || event.offsetY || 0;
                if (typeof canvas.setPointerCapture === 'function') {
                    canvas.setPointerCapture(event.pointerId || 1);
                }
            });
            canvas.addEventListener('pointermove', (event) => {
                if (!dragging) return;
                const nextX = event.clientX || event.offsetX || 0;
                const nextY = event.clientY || event.offsetY || 0;
                object.rotation.y += (nextX - lastX) * 0.01;
                object.rotation.x += (nextY - lastY) * 0.01;
                lastX = nextX;
                lastY = nextY;
                dragEvents += 1;
            });
            canvas.addEventListener('pointerup', (event) => {
                dragging = false;
                if (typeof canvas.releasePointerCapture === 'function') {
                    canvas.releasePointerCapture(event.pointerId || 1);
                }
            });
            canvas.addEventListener('wheel', (event) => {
                camera.position.z = Math.max(1.1, Math.min(6.2, camera.position.z + (event.deltaY || 0) * 0.0025));
                wheelEvents += 1;
                if (typeof event.preventDefault === 'function') event.preventDefault();
            });

            renderer.setSize(128, 112, false);
            camera.aspect = 128 / 112;
            camera.updateProjectionMatrix();
            state.resizedWidth = renderer.domElement.width || 0;
            state.resizedHeight = renderer.domElement.height || 0;
            state.cameraAspectAfterResize = camera.aspect;

            const rotationBefore = object.rotation.y;
            const cameraZBefore = camera.position.z;
            dispatch(canvas, { type: 'pointerdown', pointerId: 1, clientX: 12, clientY: 18, offsetX: 12, offsetY: 18 });
            dispatch(canvas, { type: 'pointermove', pointerId: 1, clientX: 58, clientY: 34, offsetX: 58, offsetY: 34 });
            dispatch(canvas, { type: 'pointerup', pointerId: 1, clientX: 58, clientY: 34, offsetX: 58, offsetY: 34 });
            dispatch(canvas, { type: 'wheel', deltaY: -120 });

            controls.update();
            renderer.render(scene, camera);
            if (typeof context.present === 'function') context.present();

            state.status = 'ready';
            state.dragEvents = dragEvents;
            state.wheelEvents = wheelEvents;
            state.rotationBefore = rotationBefore;
            state.rotationAfter = object.rotation.y;
            state.cameraZBefore = cameraZBefore;
            state.cameraZAfter = camera.position.z;
            state.bufferedSkipCount = (globalThis.__phase13BufferedSkips || []).length;
            state.rendererWidth = renderer.domElement.width || 0;
            state.rendererHeight = renderer.domElement.height || 0;
            return state;
        })().catch((error) => {
            const state = globalThis.__pulpGltfInputState || {};
            state.status = 'error';
            state.message = error && error.message ? String(error.message) : String(error);
            state.stack = error && error.stack ? String(error.stack) : '';
            globalThis.__pulpGltfInputState = state;
        });

        export default globalThis.__pulpGltfInputState;
    )JS";

    const auto url_pos = script.find("__PULP_GLB_URL__");
    REQUIRE(url_pos != std::string::npos);
    script.replace(url_pos, std::string("__PULP_GLB_URL__").size(), file_url);

    env.engine.run_module(script,
    resolve_threejs_module,
    [&](const std::string& error, const choc::value::Value&) {
        module_completed = true;
        module_error = error;
    });

    for (int i = 0; i < 512; ++i) {
        env.engine.pump_message_loop();
        env.bridge->service_frame_callbacks();
        const auto status = eval_string(env.engine, "globalThis.__pulpGltfInputState && globalThis.__pulpGltfInputState.status || ''");
        if (status == "ready" || status == "error") {
            break;
        }
    }

    INFO(module_error);
    REQUIRE(module_completed);
    REQUIRE(module_error.empty());

    const auto state = eval_string(env.engine, "JSON.stringify(globalThis.__pulpGltfInputState || {})");
    INFO(state);
    REQUIRE(state.find("\"status\":\"ready\"") != std::string::npos);
    REQUIRE(state.find("\"byteLength\":" + std::to_string(glb_bytes.size())) != std::string::npos);
    REQUIRE(state.find("\"resizedWidth\":128") != std::string::npos);
    REQUIRE(state.find("\"resizedHeight\":112") != std::string::npos);
    REQUIRE(state.find("\"rendererWidth\":128") != std::string::npos);
    REQUIRE(state.find("\"rendererHeight\":112") != std::string::npos);
    REQUIRE(state.find("\"dragEvents\":1") != std::string::npos);
    REQUIRE(state.find("\"wheelEvents\":1") != std::string::npos);
    REQUIRE(state.find("\"bufferedSkipCount\":0") != std::string::npos);
    REQUIRE(eval_i32(env.engine, "globalThis.__pulpGltfInputState.rotationAfter !== globalThis.__pulpGltfInputState.rotationBefore ? 1 : 0") == 1);
    REQUIRE(eval_i32(env.engine, "globalThis.__pulpGltfInputState.cameraZAfter !== globalThis.__pulpGltfInputState.cameraZBefore ? 1 : 0") == 1);
    REQUIRE(eval_i32(env.engine, "Math.abs(globalThis.__pulpGltfInputState.cameraAspectAfterResize - (128/112)) < 0.0001 ? 1 : 0") == 1);

    std::filesystem::remove(glb_path);
}

TEST_CASE("Three.js native smoke initializes and renders through the Dawn-backed bridge", "[threejs][gpu][phase13]") {
    if (!is_engine_available(JsEngineType::v8)) {
        SKIP("V8 is required for native Three.js smoke");
    }

    NativeV8Environment env(320, 220);
    if (!env.has_native_gpu()) {
        SKIP("Native Dawn adapter unavailable on this host/backend");
    }

    env.bridge->load_script("");

    const auto append_probe = eval_string(env.engine, R"JS(
        (() => {
            const probe = document.createElement('canvas');
            probe.id = 'phase13-append-probe';
            document.body.appendChild(probe);
            return JSON.stringify({
                status: 'ok',
                childCount: document.body.children.length,
                probeId: probe._id || ''
            });
        })()
    )JS");
    INFO(append_probe);
    REQUIRE(append_probe.find("\"status\":\"ok\"") != std::string::npos);
    env.engine.evaluate(R"JS(
        (() => {
            const probe = document.getElementById('phase13-append-probe');
            if (probe && probe.parentElement) {
                probe.parentElement.removeChild(probe);
            }
            return true;
        })()
    )JS");

    env.engine.evaluate(R"JS(
        (() => {
            globalThis.__phase13NestedPromise = 'start';
            new Promise(async (resolve, reject) => {
                globalThis.__phase13NestedPromise = 'before-await';
                await Promise.resolve('nested-ok');
                globalThis.__phase13NestedPromise = 'after-await';
                resolve('done');
            }).then(() => {
                globalThis.__phase13NestedPromise = 'resolved';
            }).catch((error) => {
                globalThis.__phase13NestedPromise = 'error:' + String(error && error.message ? error.message : error);
            });
            return true;
        })()
    )JS");
    for (int i = 0; i < 64; ++i) {
        env.engine.pump_message_loop();
    }
    const auto nested_promise_probe = eval_string(env.engine, "String(globalThis.__phase13NestedPromise || '')");
    INFO(nested_promise_probe);
    REQUIRE(nested_promise_probe == "resolved");

    env.engine.evaluate(R"JS(
        (() => {
            globalThis.__phase13BufferedDrawPayloads = [];
            globalThis.__phase13DirectDrawPayloads = [];
            globalThis.__phase13SubmitSummaries = [];
            globalThis.__phase13RenderPassDraws = [];
            globalThis.__phase13EncoderStages = [];
            globalThis.__phase13BundleStages = [];
            globalThis.__phase13CopyStages = [];
            globalThis.__phase13BindGroupStages = [];
            globalThis.__phase13BufferedSkips = [];
            globalThis.__phase13CreateBufferStages = [];
            globalThis.__phase13WriteBufferStages = [];

            if (typeof __gpuQueueDrawBufferedImpl === 'function') {
                const originalBuffered = __gpuQueueDrawBufferedImpl;
                __gpuQueueDrawBufferedImpl = function(payload) {
                    try {
                        globalThis.__phase13BufferedDrawPayloads.push(String(payload));
                    } catch (_) {}
                    return originalBuffered(payload);
                };
            }

            if (typeof __gpuQueueDrawImpl === 'function') {
                const originalDirect = __gpuQueueDrawImpl;
                __gpuQueueDrawImpl = function() {
                    try {
                        globalThis.__phase13DirectDrawPayloads.push(JSON.stringify(Array.from(arguments)));
                    } catch (_) {}
                    return originalDirect.apply(this, arguments);
                };
            }

            if (typeof __createMockGPURenderPassEncoder === 'function') {
                const originalCreateMockGPURenderPassEncoder = __createMockGPURenderPassEncoder;
                __createMockGPURenderPassEncoder = function(init) {
                    const descriptor = init && init.descriptor ? init.descriptor : {};
                    const attachments = descriptor && descriptor.colorAttachments ? descriptor.colorAttachments : [];
                    const attachment = attachments.length > 0 ? attachments[0] : null;
                    const attachmentView = attachment && attachment.view ? attachment.view : null;
                    const encoder = originalCreateMockGPURenderPassEncoder(init);
                    const originalSetPipeline = encoder.setPipeline ? encoder.setPipeline.bind(encoder) : null;
                    const originalSetVertexBuffer = encoder.setVertexBuffer ? encoder.setVertexBuffer.bind(encoder) : null;
                    const originalDraw = encoder.draw ? encoder.draw.bind(encoder) : null;
                    const originalDrawIndexed = encoder.drawIndexed ? encoder.drawIndexed.bind(encoder) : null;
                    let pipelineSet = false;
                    let vertexBufferSetCount = 0;

                    if (originalSetPipeline) {
                        encoder.setPipeline = function(pipeline) {
                            pipelineSet = !!pipeline;
                            return originalSetPipeline(pipeline);
                        };
                    }

                    if (originalSetVertexBuffer) {
                        encoder.setVertexBuffer = function() {
                            vertexBufferSetCount += 1;
                            return originalSetVertexBuffer.apply(this, arguments);
                        };
                    }

                    function recordDraw(kind) {
                        try {
                            globalThis.__phase13RenderPassDraws.push(JSON.stringify({
                                kind,
                                nativeBridge: !!(attachmentView && attachmentView._nativeBridge),
                                canvasId: attachmentView && attachmentView._nativeCanvasId ? attachmentView._nativeCanvasId : '',
                                textureId: attachmentView && attachmentView._nativeTextureId ? attachmentView._nativeTextureId : '',
                                format: attachmentView && attachmentView.format ? attachmentView.format : '',
                                pipelineSet,
                                pipelineNativeBridge: !!(currentPipeline && currentPipeline._nativeBridge),
                                vertexBufferSetCount
                            }));
                        } catch (_) {}
                    }

                    if (originalDraw) {
                        encoder.draw = function() {
                            recordDraw('draw');
                            return originalDraw.apply(this, arguments);
                        };
                    }

                    if (originalDrawIndexed) {
                        encoder.drawIndexed = function() {
                            recordDraw('drawIndexed');
                            return originalDrawIndexed.apply(this, arguments);
                        };
                    }

                    return encoder;
                };
            }

            return true;
        })()
    )JS");

    bool module_completed = false;
    std::string module_error;

    env.engine.run_module(R"JS(
        import * as THREE from 'three/webgpu';
        import { OrbitControls } from 'three/addons/controls/OrbitControls.js';

        class PulpCanvas {
            constructor(canvas) {
                this._canvas = canvas;
                this.style = canvas.style || {};
            }

            get width() { return this._canvas.width; }
            set width(value) { this._canvas.width = value; }

            get height() { return this._canvas.height; }
            set height(value) { this._canvas.height = value; }

            get clientWidth() { return this._canvas.width; }
            set clientWidth(value) { this._canvas.width = value; }

            get clientHeight() { return this._canvas.height; }
            set clientHeight(value) { this._canvas.height = value; }

            addEventListener() {}
            removeEventListener() {}
            dispatchEvent() { return false; }
            setPointerCapture() {}
            releasePointerCapture() {}
        }

        globalThis.__phase13ThreeState = { status: 'starting', step: 'module-start', message: '' };
        globalThis.__phase13PromiseSanity = 'pending';
        Promise.resolve('ok').then((value) => {
            globalThis.__phase13PromiseSanity = value;
        });

        document.body.style.backgroundColor = '#050816';
        document.body.style.padding = '12px';

        const shell = document.createElement('div');
        shell.id = 'phase13-threejs-shell';
        shell.style.flexDirection = 'row';
        shell.style.gap = '12px';
        shell.style.width = '296px';
        shell.style.height = '196px';
        shell.style.backgroundColor = '#101626';
        shell.style.borderRadius = '16px';
        shell.style.padding = '12px';
        document.body.appendChild(shell);

        const sceneColumn = document.createElement('div');
        sceneColumn.id = 'phase13-threejs-scene-column';
        sceneColumn.style.flexGrow = '1';
        sceneColumn.style.gap = '8px';
        shell.appendChild(sceneColumn);

        const title = document.createElement('h2');
        title.id = 'phase13-threejs-title';
        title.textContent = 'Pulp Native Three.js';
        title.style.color = '#f8fafc';
        title.style.fontSize = '18px';
        sceneColumn.appendChild(title);

        const subtitle = document.createElement('p');
        subtitle.id = 'phase13-threejs-subtitle';
        subtitle.textContent = 'Hybrid 2D+3D smoke';
        subtitle.style.color = '#cbd5e1';
        subtitle.style.fontSize = '12px';
        sceneColumn.appendChild(subtitle);

        const canvasCard = document.createElement('div');
        canvasCard.id = 'phase13-threejs-canvas-card';
        canvasCard.style.backgroundColor = '#0b1020';
        canvasCard.style.borderRadius = '12px';
        canvasCard.style.padding = '6px';
        sceneColumn.appendChild(canvasCard);

        globalThis.__phase13ThreeState.step = 'create-canvas';
        const canvas = document.createElement('canvas');
        canvas.id = 'phase13-threejs-native-canvas';
        canvas.width = 96;
        canvas.height = 96;
        globalThis.__phase13ThreeState.step = 'append-canvas';
        canvasCard.appendChild(canvas);

        const hud = document.createElement('div');
        hud.id = 'phase13-threejs-hud';
        hud.style.width = '88px';
        hud.style.flexShrink = '0';
        hud.style.backgroundColor = '#0b1220';
        hud.style.borderRadius = '12px';
        hud.style.padding = '8px';
        hud.style.gap = '6px';
        shell.appendChild(hud);

        const hudTitle = document.createElement('h3');
        hudTitle.id = 'phase13-threejs-hud-title';
        hudTitle.textContent = 'Runtime';
        hudTitle.style.color = '#f8fafc';
        hudTitle.style.fontSize = '14px';
        hud.appendChild(hudTitle);

        const hudValue = document.createElement('span');
        hudValue.id = 'phase13-threejs-hud-backend';
        hudValue.textContent = 'booting';
        hudValue.style.color = '#7dd3fc';
        hudValue.style.fontSize = '12px';
        hud.appendChild(hudValue);

        globalThis.__phase13ThreeState.step = 'get-context';
        const context = canvas.getContext('webgpu');
        globalThis.__phase13ThreeState.step = 'wrap-canvas';
        const wrappedCanvas = new PulpCanvas(canvas);
        globalThis.__phase13ThreeState.step = 'schedule-task';

        const originalRequestAdapter = navigator.gpu.requestAdapter.bind(navigator.gpu);
        navigator.gpu.requestAdapter = async function(options) {
            globalThis.__phase13ThreeState.step = 'request-adapter';
            const adapter = await originalRequestAdapter(options);
            globalThis.__phase13ThreeState.step = 'request-adapter-done';
            if (adapter && typeof adapter.requestDevice === 'function') {
                const originalRequestDevice = adapter.requestDevice.bind(adapter);
                adapter.requestDevice = async function(descriptor) {
                    globalThis.__phase13ThreeState.step = 'request-device';
                    const device = await originalRequestDevice(descriptor);
                    globalThis.__phase13ThreeState.step = 'request-device-done';
                    if (device && typeof device.createCommandEncoder === 'function') {
                        const originalCreateCommandEncoder = device.createCommandEncoder.bind(device);
                        device.createCommandEncoder = function(commandDescriptor) {
                            try {
                                globalThis.__phase13EncoderStages.push(JSON.stringify({
                                    stage: 'createCommandEncoder',
                                    label: commandDescriptor && commandDescriptor.label ? commandDescriptor.label : '',
                                }));
                            } catch (_) {}
                            const encoder = originalCreateCommandEncoder(commandDescriptor);
                            if (!encoder) {
                                return encoder;
                            }

                            const originalBeginRenderPass = encoder.beginRenderPass ? encoder.beginRenderPass.bind(encoder) : null;
                            if (originalBeginRenderPass) {
                                encoder.beginRenderPass = function(passDescriptor) {
                                    try {
                                        const attachments = passDescriptor && passDescriptor.colorAttachments ? passDescriptor.colorAttachments : [];
                                        const attachment = attachments.length > 0 ? attachments[0] : null;
                                        const view = attachment && attachment.view ? attachment.view : null;
                                        globalThis.__phase13EncoderStages.push(JSON.stringify({
                                            stage: 'beginRenderPass',
                                            nativeBridge: !!(view && view._nativeBridge),
                                            canvasId: view && view._nativeCanvasId ? view._nativeCanvasId : '',
                                            textureId: view && view._nativeTextureId ? view._nativeTextureId : '',
                                            format: view && view.format ? view.format : '',
                                            loadOp: attachment && attachment.loadOp ? attachment.loadOp : '',
                                            storeOp: attachment && attachment.storeOp ? attachment.storeOp : ''
                                        }));
                                    } catch (_) {}
                                    const pass = originalBeginRenderPass(passDescriptor);
                                    let pipelineSet = false;
                                    const boundGroups = [];
                                    const originalSetPipeline = pass && pass.setPipeline ? pass.setPipeline.bind(pass) : null;
                                    if (originalSetPipeline) {
                                        pass.setPipeline = function(pipeline) {
                                            pipelineSet = !!pipeline;
                                            return originalSetPipeline.apply(this, arguments);
                                        };
                                    }
                                    let bindGroupSetCount = 0;
                                    const originalSetBindGroup = pass && pass.setBindGroup ? pass.setBindGroup.bind(pass) : null;
                                    if (originalSetBindGroup) {
                                        pass.setBindGroup = function(index, bindGroup) {
                                            bindGroupSetCount += 1;
                                            try {
                                                boundGroups.push({
                                                    index: index == null ? 0 : index,
                                                    summary: bindGroup && bindGroup._phase13Summary ? bindGroup._phase13Summary : null
                                                });
                                            } catch (_) {}
                                            return originalSetBindGroup.apply(this, arguments);
                                        };
                                    }
                                    const originalEnd = pass && pass.end ? pass.end.bind(pass) : null;
                                    if (originalEnd) {
                                        pass.end = function() {
                                            try {
                                                globalThis.__phase13EncoderStages.push(JSON.stringify({
                                                    stage: 'endRenderPass',
                                                    bindGroupSetCount,
                                                    pipelineSet,
                                                    boundGroups
                                                }));
                                            } catch (_) {}
                                            return originalEnd();
                                        };
                                    }
                                    return pass;
                                };
                            }

                            const originalCopyTextureToTexture = encoder.copyTextureToTexture ? encoder.copyTextureToTexture.bind(encoder) : null;
                            if (originalCopyTextureToTexture) {
                                encoder.copyTextureToTexture = function(source, destination, copySize) {
                                    try {
                                        globalThis.__phase13CopyStages.push(JSON.stringify({
                                            stage: 'copyTextureToTexture',
                                            sourceFormat: source && source.texture && source.texture.format ? source.texture.format : '',
                                            destinationFormat: destination && destination.texture && destination.texture.format ? destination.texture.format : '',
                                            destinationNativeBridge: !!(destination && destination.texture && destination.texture._nativeBridge),
                                            destinationCanvasId: destination && destination.texture && destination.texture._nativeCanvasId ? destination.texture._nativeCanvasId : '',
                                            width: copySize && copySize.width ? copySize.width : 0,
                                            height: copySize && copySize.height ? copySize.height : 0
                                        }));
                                    } catch (_) {}
                                    return originalCopyTextureToTexture(source, destination, copySize);
                                };
                            }

                            const originalFinish = encoder.finish ? encoder.finish.bind(encoder) : null;
                            if (originalFinish) {
                                encoder.finish = function(finishDescriptor) {
                                    const commandBuffer = originalFinish(finishDescriptor);
                                    try {
                                        const commands = commandBuffer && commandBuffer._commands ? commandBuffer._commands : [];
                                        globalThis.__phase13EncoderStages.push(JSON.stringify({
                                            stage: 'finishCommandEncoder',
                                            label: finishDescriptor && finishDescriptor.label ? finishDescriptor.label : '',
                                            commandCount: commands.length,
                                            commandTypes: commands.map((command) => command && command.type ? command.type : typeof command)
                                        }));
                                    } catch (_) {}
                                    return commandBuffer;
                                };
                            }

                            return encoder;
                        };
                    }
                    if (device && typeof device.createRenderBundleEncoder === 'function') {
                        const originalCreateRenderBundleEncoder = device.createRenderBundleEncoder.bind(device);
                        device.createRenderBundleEncoder = function(bundleDescriptor) {
                            try {
                                globalThis.__phase13BundleStages.push(JSON.stringify({
                                    stage: 'createRenderBundleEncoder',
                                    label: bundleDescriptor && bundleDescriptor.label ? bundleDescriptor.label : ''
                                }));
                            } catch (_) {}
                            const bundleEncoder = originalCreateRenderBundleEncoder(bundleDescriptor);
                            if (!bundleEncoder) {
                                return bundleEncoder;
                            }
                            const originalFinish = bundleEncoder.finish ? bundleEncoder.finish.bind(bundleEncoder) : null;
                            if (originalFinish) {
                                bundleEncoder.finish = function(finishDescriptor) {
                                    const bundle = originalFinish(finishDescriptor);
                                    try {
                                        const commands = bundle && bundle._commands ? bundle._commands : [];
                                        globalThis.__phase13BundleStages.push(JSON.stringify({
                                            stage: 'finishRenderBundle',
                                            label: finishDescriptor && finishDescriptor.label ? finishDescriptor.label : '',
                                            commandCount: commands.length,
                                            commandTypes: commands.map((command) => command && command.type ? command.type : typeof command)
                                        }));
                                    } catch (_) {}
                                    return bundle;
                                };
                            }
                            return bundleEncoder;
                        };
                    }
                    if (device && typeof device.createBindGroup === 'function') {
                        const originalCreateBindGroup = device.createBindGroup.bind(device);
                        device.createBindGroup = function(bindGroupDescriptor) {
                            const bindGroup = originalCreateBindGroup(bindGroupDescriptor);
                            try {
                                const entries = bindGroupDescriptor && bindGroupDescriptor.entries ? bindGroupDescriptor.entries : [];
                                bindGroup._phase13Summary = entries.map((entry) => {
                                    const resource = entry ? entry.resource : null;
                                    if (resource && resource.buffer) {
                                        return {
                                            binding: entry.binding,
                                            type: 'buffer',
                                            size: resource.buffer.size || 0
                                        };
                                    }
                                    if (resource && resource.texture) {
                                                return {
                                                    binding: entry.binding,
                                                    type: 'textureView',
                                                    format: resource.format || (resource.texture && resource.texture.format) || '',
                                                    nativeBridge: !!(resource.texture && resource.texture._nativeBridge),
                                                    nativeCanvasId: resource.texture && resource.texture._nativeCanvasId ? resource.texture._nativeCanvasId : '',
                                                    nativeTextureId: resource.texture && resource.texture._nativeTextureId ? resource.texture._nativeTextureId : ''
                                                };
                                            }
                                    return {
                                        binding: entry && entry.binding != null ? entry.binding : 0,
                                        type: resource && resource._objectName ? resource._objectName : typeof resource
                                    };
                                });
                                globalThis.__phase13BindGroupStages.push(JSON.stringify(bindGroup._phase13Summary));
                            } catch (_) {}
                            return bindGroup;
                        };
                    }
                    if (device && typeof device.createBuffer === 'function') {
                        const originalCreateBuffer = device.createBuffer.bind(device);
                        device.createBuffer = function(bufferDescriptor) {
                            const buffer = originalCreateBuffer(bufferDescriptor);
                            try {
                                globalThis.__phase13CreateBufferStages.push(JSON.stringify({
                                    label: bufferDescriptor && bufferDescriptor.label ? bufferDescriptor.label : '',
                                    size: bufferDescriptor && bufferDescriptor.size != null ? bufferDescriptor.size : 0,
                                    usage: bufferDescriptor && bufferDescriptor.usage != null ? bufferDescriptor.usage : 0,
                                    mappedAtCreation: !!(bufferDescriptor && bufferDescriptor.mappedAtCreation),
                                    objectName: buffer && buffer._objectName ? buffer._objectName : ''
                                }));
                            } catch (_) {}
                            return buffer;
                        };
                    }
                    if (device && device.queue && typeof device.queue.writeBuffer === 'function') {
                        const originalWriteBuffer = device.queue.writeBuffer.bind(device.queue);
                        device.queue.writeBuffer = function(buffer, bufferOffset, data, dataOffset, size) {
                            try {
                                const bytes = ArrayBuffer.isView(data)
                                    ? Array.from(new Uint8Array(data.buffer, data.byteOffset, Math.min(data.byteLength, 32)))
                                    : (data instanceof ArrayBuffer
                                        ? Array.from(new Uint8Array(data, 0, Math.min(data.byteLength, 32)))
                                        : []);
                                globalThis.__phase13WriteBufferStages.push(JSON.stringify({
                                    label: buffer && buffer.label ? buffer.label : '',
                                    size: buffer && buffer.size != null ? buffer.size : 0,
                                    bufferOffset: bufferOffset == null ? 0 : bufferOffset,
                                    dataOffset: dataOffset == null ? 0 : dataOffset,
                                    copySize: size == null ? null : size,
                                    byteLength: ArrayBuffer.isView(data) ? data.byteLength : (data instanceof ArrayBuffer ? data.byteLength : 0),
                                    preview: bytes
                                }));
                            } catch (_) {}
                            return originalWriteBuffer(buffer, bufferOffset, data, dataOffset, size);
                        };
                    }
                    if (device && device.queue && typeof device.queue.submit === 'function') {
                        const originalSubmit = device.queue.submit.bind(device.queue);
                        device.queue.submit = function(commandBuffers) {
                            try {
                                const summaries = [];
                                if (commandBuffers && typeof commandBuffers.length === 'number') {
                                    for (let i = 0; i < commandBuffers.length; ++i) {
                                        const commandBuffer = commandBuffers[i];
                                        const commands = commandBuffer && commandBuffer._commands ? commandBuffer._commands : [];
                                        summaries.push({
                                            objectName: commandBuffer && commandBuffer._objectName ? commandBuffer._objectName : '',
                                            keys: commandBuffer ? Object.keys(commandBuffer) : [],
                                            commandTypes: commands.map((command) => command && command.type ? command.type : typeof command)
                                        });
                                    }
                                }
                                globalThis.__phase13SubmitSummaries.push(JSON.stringify(summaries));
                            } catch (_) {}
                            return originalSubmit(commandBuffers);
                        };
                    }
                    return device;
                };
            }
            return adapter;
        };

        const originalConfigure = context.configure ? context.configure.bind(context) : null;
        if (originalConfigure) {
            context.configure = function(descriptor) {
                globalThis.__phase13ThreeState.step = 'context-configure';
                const result = originalConfigure(descriptor);
                globalThis.__phase13ThreeState.step = 'context-configure-done';
                return result;
            };
        }

        const originalRAF = window.requestAnimationFrame ? window.requestAnimationFrame.bind(window) : null;
        if (originalRAF) {
            window.requestAnimationFrame = function(callback) {
                globalThis.__phase13ThreeState.step = 'request-animation-frame';
                return originalRAF(callback);
            };
        }

        globalThis.__phase13ThreeTask = (async () => {
            globalThis.__phase13ThreeState.step = 'renderer-construct';
            const renderer = new THREE.WebGPURenderer({
                canvas: wrappedCanvas,
                context,
                antialias: false
            });

            globalThis.__phase13ThreeRenderer = renderer;
            if (renderer.backend && typeof renderer.backend.init === 'function') {
                const originalBackendInit = renderer.backend.init.bind(renderer.backend);
                renderer.backend.init = async function(rendererArg) {
                    globalThis.__phase13ThreeState.step = 'backend-init-enter';
                    const result = await originalBackendInit(rendererArg);
                    globalThis.__phase13ThreeState.step = 'backend-init-done';
                    return result;
                };
            }
            if (renderer._inspector && typeof renderer._inspector.init === 'function') {
                const originalInspectorInit = renderer._inspector.init.bind(renderer._inspector);
                renderer._inspector.init = function() {
                    globalThis.__phase13ThreeState.step = 'inspector-init';
                    return originalInspectorInit();
                };
            }
            if (typeof renderer.init === 'function') {
                const originalRendererInit = renderer.init.bind(renderer);
                renderer.init = async function() {
                    globalThis.__phase13ThreeState.step = 'renderer-init-enter';
                    const result = await originalRendererInit();
                    globalThis.__phase13ThreeState.step = 'renderer-init-resolved';
                    return result;
                };
            }
            globalThis.__phase13ThreeState.step = 'renderer-init-await';
            await renderer.init();
            globalThis.__phase13ThreeState.step = 'renderer-init-done';

            const scene = new THREE.Scene();
            scene.background = new THREE.Color(1, 0, 0);

            const camera = new THREE.PerspectiveCamera(70, 1, 0.1, 10);
            camera.position.z = 2;

            const geometry = new THREE.BoxGeometry(0.75, 0.75, 0.75);
            const material = new THREE.MeshBasicMaterial({ color: 0x00ff00 });
            const mesh = new THREE.Mesh(geometry, material);
            mesh.rotation.x = 0.4;
            mesh.rotation.y = 0.6;
            scene.add(mesh);

            globalThis.__phase13ThreeState.step = 'controls-init';
            const controls = new OrbitControls(camera, canvas);
            controls.enableDamping = true;
            controls.enablePan = false;
            controls.target.set(0, 0, 0);
            controls.update();

            globalThis.__phase13ThreeState.step = 'renderer-render';
            renderer.render(scene, camera);
            if (typeof context.present === 'function') {
                globalThis.__phase13ThreeState.step = 'context-present';
                context.present();
            }
            globalThis.__phase13ThreeState.step = 'renderer-render-2';
            renderer.render(scene, camera);
            if (typeof context.present === 'function') {
                globalThis.__phase13ThreeState.step = 'context-present-2';
                context.present();
            }

            globalThis.__phase13ThreeState = {
                status: 'ready',
                step: 'done',
                layout: 'hybrid-2d-3d',
                backend: renderer.backend && renderer.backend.constructor ? renderer.backend.constructor.name : '',
                contextType: renderer.getContext() && renderer.getContext()._objectName ? renderer.getContext()._objectName : '',
                width: renderer.domElement.width || 0,
                height: renderer.domElement.height || 0,
                titleText: title.textContent
            };
            hudValue.textContent = globalThis.__phase13ThreeState.backend || 'unknown';
        })().catch((error) => {
            globalThis.__phase13ThreeState = {
                status: 'error',
                step: globalThis.__phase13ThreeState && globalThis.__phase13ThreeState.step ? globalThis.__phase13ThreeState.step : '',
                message: error && error.message ? String(error.message) : String(error),
                stack: error && error.stack ? String(error.stack) : ''
            };
        });

        export default true;
    )JS",
    resolve_threejs_module,
    [&](const std::string& error, const choc::value::Value&) {
        module_completed = true;
        module_error = error;
    });

    for (int i = 0; i < 512; ++i) {
        env.engine.pump_message_loop();
        const auto status = eval_string(env.engine, "globalThis.__phase13ThreeState && globalThis.__phase13ThreeState.status || ''");
        if (status == "ready" || status == "error") {
            break;
        }
    }

    for (int i = 0; i < 64; ++i) {
        env.engine.pump_message_loop();
    }

    REQUIRE(module_completed);
    REQUIRE(module_error.empty());

    const auto status = eval_string(env.engine, "globalThis.__phase13ThreeState.status");
    INFO(eval_string(env.engine, "JSON.stringify(globalThis.__phase13ThreeState)"));
    INFO(eval_string(env.engine, "String(globalThis.__phase13PromiseSanity)"));
    INFO(eval_string(env.engine, "String((globalThis.__phase13SubmitSummaries || []).length)"));
    INFO(eval_string(env.engine, "JSON.stringify(globalThis.__phase13SubmitSummaries || [])"));
    INFO(eval_string(env.engine, "String((globalThis.__phase13RenderPassDraws || []).length)"));
    INFO(eval_string(env.engine, "JSON.stringify(globalThis.__phase13RenderPassDraws || [])"));
    INFO(eval_string(env.engine, "String((globalThis.__phase13EncoderStages || []).length)"));
    INFO(eval_string(env.engine, "JSON.stringify(globalThis.__phase13EncoderStages || [])"));
    INFO(eval_string(env.engine, "String((globalThis.__phase13BundleStages || []).length)"));
    INFO(eval_string(env.engine, "JSON.stringify(globalThis.__phase13BundleStages || [])"));
    INFO(eval_string(env.engine, "String((globalThis.__phase13CopyStages || []).length)"));
    INFO(eval_string(env.engine, "JSON.stringify(globalThis.__phase13CopyStages || [])"));
    INFO(eval_string(env.engine, "String((globalThis.__phase13BindGroupStages || []).length)"));
    INFO(eval_string(env.engine, "JSON.stringify(globalThis.__phase13BindGroupStages || [])"));
    INFO(eval_string(env.engine, "String((globalThis.__phase13BufferedSkips || []).length)"));
    INFO(eval_string(env.engine, "JSON.stringify(globalThis.__phase13BufferedSkips || [])"));
    INFO(eval_string(env.engine, "String((globalThis.__phase13CreateBufferStages || []).length)"));
    INFO(eval_string(env.engine, "JSON.stringify(globalThis.__phase13CreateBufferStages || [])"));
    INFO(eval_string(env.engine, "String((globalThis.__phase13WriteBufferStages || []).length)"));
    INFO(eval_string(env.engine, "JSON.stringify(globalThis.__phase13WriteBufferStages || [])"));
    INFO(eval_string(env.engine, "String((globalThis.__phase13BufferedDrawPayloads || []).length)"));
    INFO(eval_string(env.engine, "String((globalThis.__phase13DirectDrawPayloads || []).length)"));
    INFO(eval_string(env.engine, "JSON.stringify(globalThis.__phase13BufferedDrawPayloads || [])"));
    INFO(eval_string(env.engine, "(globalThis.__phase13DirectDrawPayloads && globalThis.__phase13DirectDrawPayloads[0]) || ''"));
    REQUIRE(status == "ready");
    REQUIRE(eval_string(env.engine, "globalThis.__phase13ThreeState.layout") == "hybrid-2d-3d");
    REQUIRE(eval_string(env.engine, "globalThis.__phase13ThreeState.titleText") == "Pulp Native Three.js");
    REQUIRE(eval_string(env.engine, "globalThis.__phase13ThreeState.contextType") == "GPUCanvasContext");
    REQUIRE(eval_i32(env.engine, "globalThis.__phase13ThreeState.width") == 96);
    REQUIRE(eval_i32(env.engine, "globalThis.__phase13ThreeState.height") == 96);

    env.root.layout_children();

    const auto title_id = eval_string(env.engine, "document.getElementById('phase13-threejs-title')._id");
    const auto hud_id = eval_string(env.engine, "document.getElementById('phase13-threejs-hud')._id");
    auto* title_widget = dynamic_cast<Label*>(env.bridge->widget(title_id));
    REQUIRE(title_widget != nullptr);
    REQUIRE(title_widget->text() == "Pulp Native Three.js");
    REQUIRE(env.bridge->widget(hud_id) != nullptr);

    const auto native_id = eval_string(env.engine, "document.getElementById('phase13-threejs-native-canvas')._id");
    const auto source_texture_id = eval_string(
        env.engine,
        "(() => {"
        "  try {"
        "    const payloads = globalThis.__phase13BufferedDrawPayloads || [];"
        "    return payloads.length ? (JSON.parse(payloads[0]).targetTextureId || '') : '';"
        "  } catch (_) {"
        "    return '';"
        "  }"
        "})()");
    auto* widget = dynamic_cast<CanvasWidget*>(env.bridge->widget(native_id));
    REQUIRE(widget != nullptr);
    INFO(source_texture_id);

    auto source_frame = env.bridge->describe_native_texture_frame(source_texture_id);
    INFO(source_frame.available);
    if (source_frame.available && source_frame.texture_handle != nullptr) {
        auto source_skia = pulp::render::SkiaSurface::create(*env.gpu_surface, {.width = 96, .height = 96});
        REQUIRE(source_skia != nullptr);
        REQUIRE(source_skia->is_available());
        REQUIRE(env.gpu_surface->begin_frame());

        auto* source_canvas = source_skia->begin_frame();
        REQUIRE(source_canvas != nullptr);
        auto* source_skia_canvas = dynamic_cast<pulp::canvas::SkiaCanvas*>(source_canvas);
        REQUIRE(source_skia_canvas != nullptr);
        REQUIRE(source_skia_canvas->draw_native_dawn_texture(source_frame.texture_handle,
                                                             source_frame.width,
                                                             source_frame.height,
                                                             source_frame.format,
                                                             0.0f,
                                                             0.0f,
                                                             96.0f,
                                                             96.0f));
        source_skia->end_frame();

        std::vector<uint8_t> source_pixels;
        uint32_t source_pixel_width = 0;
        uint32_t source_pixel_height = 0;
        REQUIRE(source_skia->read_current_rgba(source_pixels, source_pixel_width, source_pixel_height));
        env.gpu_surface->end_frame();
        REQUIRE(source_pixel_width == 96);
        REQUIRE(source_pixel_height == 96);
        const auto source_center = ((source_pixel_height / 2u) * source_pixel_width + (source_pixel_width / 2u)) * 4u;
        REQUIRE(source_center + 3 < source_pixels.size());
        const auto source_r = static_cast<int>(source_pixels[source_center + 0]);
        const auto source_g = static_cast<int>(source_pixels[source_center + 1]);
        const auto source_b = static_cast<int>(source_pixels[source_center + 2]);
        const auto source_a = static_cast<int>(source_pixels[source_center + 3]);
        INFO("source-center-r=" << source_r);
        INFO("source-center-g=" << source_g);
        INFO("source-center-b=" << source_b);
        INFO("source-center-a=" << source_a);
        REQUIRE(source_g > 100);
        REQUIRE(source_g > source_r + 32);
        REQUIRE(source_g > source_b + 32);
    }

    INFO(eval_string(env.engine,
                     std::string("(() => {")
                     + "  try {"
                     + "    const payloads = globalThis.__phase13BufferedDrawPayloads || [];"
                     + "    if (!payloads.length || typeof __gpuQueuePresentTextureImpl !== 'function') return 'no-direct-present-probe';"
                     + "    const sourceTextureId = JSON.parse(payloads[0]).targetTextureId || '';"
                     + "    if (!sourceTextureId) return 'no-source-texture';"
                     + "    return String(__gpuQueuePresentTextureImpl(JSON.stringify({ canvasId: '"
                     + native_id
                     + "', sourceTextureId })));"
                     + "  } catch (error) {"
                     + "    return 'direct-present-error:' + String(error && error.message ? error.message : error);"
                     + "  }"
                     + "})()"));

    auto canvas_frame = env.bridge->describe_native_canvas_frame(native_id);
    INFO(canvas_frame.available);
    if (canvas_frame.available && canvas_frame.texture_handle != nullptr) {
        auto canvas_probe = pulp::render::SkiaSurface::create(*env.gpu_surface, {.width = 96, .height = 96});
        REQUIRE(canvas_probe != nullptr);
        REQUIRE(canvas_probe->is_available());
        REQUIRE(env.gpu_surface->begin_frame());

        auto* canvas_probe_canvas = canvas_probe->begin_frame();
        REQUIRE(canvas_probe_canvas != nullptr);
        auto* canvas_probe_skia = dynamic_cast<pulp::canvas::SkiaCanvas*>(canvas_probe_canvas);
        REQUIRE(canvas_probe_skia != nullptr);
        REQUIRE(canvas_probe_skia->draw_native_dawn_texture(canvas_frame.texture_handle,
                                                            canvas_frame.width,
                                                            canvas_frame.height,
                                                            canvas_frame.format,
                                                            0.0f,
                                                            0.0f,
                                                            96.0f,
                                                            96.0f));
        canvas_probe->end_frame();

        std::vector<uint8_t> canvas_probe_pixels;
        uint32_t canvas_probe_width = 0;
        uint32_t canvas_probe_height = 0;
        REQUIRE(canvas_probe->read_current_rgba(canvas_probe_pixels, canvas_probe_width, canvas_probe_height));
        env.gpu_surface->end_frame();
        REQUIRE(canvas_probe_width == 96);
        REQUIRE(canvas_probe_height == 96);
        const auto canvas_probe_center =
            ((canvas_probe_height / 2u) * canvas_probe_width + (canvas_probe_width / 2u)) * 4u;
        REQUIRE(canvas_probe_center + 3 < canvas_probe_pixels.size());
        const auto canvas_probe_r = static_cast<int>(canvas_probe_pixels[canvas_probe_center + 0]);
        const auto canvas_probe_g = static_cast<int>(canvas_probe_pixels[canvas_probe_center + 1]);
        const auto canvas_probe_b = static_cast<int>(canvas_probe_pixels[canvas_probe_center + 2]);
        const auto canvas_probe_a = static_cast<int>(canvas_probe_pixels[canvas_probe_center + 3]);
        INFO("canvas-center-r=" << canvas_probe_r);
        INFO("canvas-center-g=" << canvas_probe_g);
        INFO("canvas-center-b=" << canvas_probe_b);
        INFO("canvas-center-a=" << canvas_probe_a);
        REQUIRE(canvas_probe_g > 100);
        REQUIRE(canvas_probe_g > canvas_probe_r + 32);
        REQUIRE(canvas_probe_g > canvas_probe_b + 32);
    }

    auto skia = pulp::render::SkiaSurface::create(*env.gpu_surface, {.width = 96, .height = 96});
    REQUIRE(skia != nullptr);
    REQUIRE(skia->is_available());
    REQUIRE(env.gpu_surface->begin_frame());

    auto* canvas = skia->begin_frame();
    REQUIRE(canvas != nullptr);
    widget->paint(*canvas);
    REQUIRE(widget->last_native_gpu_texture_draw_succeeded());
    skia->end_frame();

    std::vector<uint8_t> pixels;
    uint32_t pixel_width = 0;
    uint32_t pixel_height = 0;
    REQUIRE(skia->read_current_rgba(pixels, pixel_width, pixel_height));
    env.gpu_surface->end_frame();

    REQUIRE(pixel_width == 96);
    REQUIRE(pixel_height == 96);
    const auto center = ((pixel_height / 2u) * pixel_width + (pixel_width / 2u)) * 4u;
    REQUIRE(center + 3 < pixels.size());
    const auto final_r = static_cast<int>(pixels[center + 0]);
    const auto final_g = static_cast<int>(pixels[center + 1]);
    const auto final_b = static_cast<int>(pixels[center + 2]);
    const auto final_a = static_cast<int>(pixels[center + 3]);
    INFO("final-center-r=" << final_r);
    INFO("final-center-g=" << final_g);
    INFO("final-center-b=" << final_b);
    INFO("final-center-a=" << final_a);
    REQUIRE(final_g > 100);
    REQUIRE(final_g > final_r + 32);
    REQUIRE(final_g > final_b + 32);
}

TEST_CASE("Three.js spectrum analyzer mode renders an audio-reactive peak bar through the native bridge", "[threejs][gpu][phase13][spectrum]") {
    if (!is_engine_available(JsEngineType::v8)) {
        SKIP("V8 is required for native Three.js smoke");
    }

    NativeV8Environment env(320, 220);
    if (!env.has_native_gpu()) {
        SKIP("Native Dawn adapter unavailable on this host/backend");
    }

    env.bridge->load_script("");
    env.engine.register_function("__readSpectrumFrame__", [](choc::javascript::ArgumentList) {
        auto result = choc::value::createObject("");
        result.addMember("bars", choc::value::createArray(24, [](uint32_t index) {
            const auto center = 11.5;
            const auto distance = std::abs(static_cast<double>(index) - center) / center;
            return choc::value::createFloat64(std::max(0.15, 1.0 - distance));
        }));
        result.addMember("peak", choc::value::createFloat64(1.0));
        result.addMember("time", choc::value::createFloat64(0.25));
        return result;
    });

    bool module_completed = false;
    std::string module_error;
    env.engine.run_module(R"JS(
        import * as THREE from 'three/webgpu';

        class PulpCanvas {
            constructor(canvas) {
                this._canvas = canvas;
                this.style = canvas.style || {};
            }
            get width() { return this._canvas.width; }
            set width(value) { this._canvas.width = value; }
            get height() { return this._canvas.height; }
            set height(value) { this._canvas.height = value; }
            get clientWidth() { return this._canvas.width; }
            get clientHeight() { return this._canvas.height; }
            addEventListener(type, fn, opts) { return this._canvas.addEventListener(type, fn, opts); }
            removeEventListener(type, fn, opts) { return this._canvas.removeEventListener(type, fn, opts); }
            dispatchEvent(event) { return this._canvas.dispatchEvent(event); }
        }

        globalThis.__phase13SpectrumState = { status: 'starting', peak: 0 };

        const shell = document.createElement('div');
        shell.id = 'phase13-spectrum-shell';
        document.body.appendChild(shell);

        const title = document.createElement('h2');
        title.id = 'phase13-spectrum-title';
        title.textContent = 'Pulp Native Spectrum Analyzer';
        shell.appendChild(title);

        const canvas = document.createElement('canvas');
        canvas.id = 'phase13-spectrum-canvas';
        canvas.width = 160;
        canvas.height = 160;
        shell.appendChild(canvas);

        const context = canvas.getContext('webgpu');
        const renderer = new THREE.WebGPURenderer({
            canvas: new PulpCanvas(canvas),
            context,
            antialias: false
        });
        await renderer.init();

        const scene = new THREE.Scene();
        scene.background = new THREE.Color(0x08111f);
        const camera = new THREE.PerspectiveCamera(70, 1, 0.1, 10);
        camera.position.z = 4.8;
        camera.position.y = 0.6;

        const barsGroup = new THREE.Group();
        barsGroup.rotation.x = -0.18;
        const barGeometry = new THREE.BoxGeometry(0.2, 1.0, 0.28);
        const frame = typeof __readSpectrumFrame__ === 'function' ? __readSpectrumFrame__() : { bars: [], peak: 0 };
        const bars = Array.isArray(frame.bars) ? frame.bars : [];
        for (let i = 0; i < 24; ++i) {
            const magnitude = Math.max(0.05, Math.min(1.0, Number(bars[i] || 0)));
            const bar = new THREE.Mesh(barGeometry, new THREE.MeshBasicMaterial({ color: 0x38bdf8 }));
            const height = 0.2 + magnitude * 2.4;
            bar.position.x = (i - 11.5) * 0.22;
            bar.position.y = height * 0.5;
            bar.scale.y = height;
            barsGroup.add(bar);
        }
        scene.add(barsGroup);

        const peakBar = new THREE.Mesh(
            new THREE.BoxGeometry(0.6, 1.0, 0.6),
            new THREE.MeshBasicMaterial({ color: 0xa3e635 })
        );
        const peakHeight = 0.35 + Number(frame.peak || 0) * 3.0;
        peakBar.scale.y = peakHeight;
        peakBar.position.y = peakHeight * 0.5;
        peakBar.position.z = -0.7;
        scene.add(peakBar);

        renderer.render(scene, camera);
        if (typeof context.present === 'function') context.present();
        renderer.render(scene, camera);
        if (typeof context.present === 'function') context.present();

        globalThis.__phase13SpectrumState = {
            status: 'ready',
            peak: Number(frame.peak || 0),
            titleText: title.textContent
        };

        export default true;
    )JS", resolve_threejs_module,
    [&](const std::string& error, const choc::value::Value&) {
        module_completed = true;
        module_error = error;
    });

    for (int i = 0; i < 256; ++i) {
        env.engine.pump_message_loop();
        if (eval_string(env.engine, "globalThis.__phase13SpectrumState.status") == "ready") {
            break;
        }
    }

    REQUIRE(module_completed);
    REQUIRE(module_error.empty());
    REQUIRE(eval_string(env.engine, "globalThis.__phase13SpectrumState.status") == "ready");
    REQUIRE(eval_string(env.engine, "globalThis.__phase13SpectrumState.titleText") == "Pulp Native Spectrum Analyzer");

    env.root.layout_children();

    const auto native_id = eval_string(env.engine, "document.getElementById('phase13-spectrum-canvas')._id");
    auto* widget = dynamic_cast<CanvasWidget*>(env.bridge->widget(native_id));
    REQUIRE(widget != nullptr);

    auto skia = pulp::render::SkiaSurface::create(*env.gpu_surface, {.width = 160, .height = 160});
    REQUIRE(skia != nullptr);
    REQUIRE(skia->is_available());
    REQUIRE(env.gpu_surface->begin_frame());

    auto* canvas = skia->begin_frame();
    REQUIRE(canvas != nullptr);
    widget->paint(*canvas);
    REQUIRE(widget->last_native_gpu_texture_draw_succeeded());
    skia->end_frame();

    std::vector<uint8_t> pixels;
    uint32_t pixel_width = 0;
    uint32_t pixel_height = 0;
    REQUIRE(skia->read_current_rgba(pixels, pixel_width, pixel_height));
    env.gpu_surface->end_frame();

    const auto center = ((pixel_height / 2u) * pixel_width + (pixel_width / 2u)) * 4u;
    REQUIRE(center + 3 < pixels.size());
    const auto r = static_cast<int>(pixels[center + 0]);
    const auto g = static_cast<int>(pixels[center + 1]);
    const auto b = static_cast<int>(pixels[center + 2]);
    INFO("spectrum-center-r=" << r);
    INFO("spectrum-center-g=" << g);
    INFO("spectrum-center-b=" << b);
    REQUIRE(g > 110);
    REQUIRE(g > r);
    REQUIRE(g > b);
}


TEST_CASE("Three.js particle visualizer mode renders an audio-reactive particle core through the native bridge", "[threejs][gpu][phase13][particles]") {
    if (!is_engine_available(JsEngineType::v8)) {
        SKIP("V8 is required for native Three.js smoke");
    }

    NativeV8Environment env(320, 220);
    if (!env.has_native_gpu()) {
        SKIP("Native Dawn adapter unavailable on this host/backend");
    }

    env.bridge->load_script("");
    env.engine.register_function("__readSpectrumFrame__", [](choc::javascript::ArgumentList) {
        auto result = choc::value::createObject("");
        result.addMember("bars", choc::value::createArray(24, [](uint32_t index) {
            const auto angle = static_cast<double>(index) / 23.0;
            return choc::value::createFloat64(0.25 + 0.75 * std::sin(angle * 3.14159265358979323846));
        }));
        result.addMember("peak", choc::value::createFloat64(0.92));
        result.addMember("rms", choc::value::createFloat64(0.78));
        result.addMember("beat", choc::value::createFloat64(0.96));
        result.addMember("time", choc::value::createFloat64(0.35));
        return result;
    });

    bool module_completed = false;
    std::string module_error;
    env.engine.run_module(R"JS(
        import * as THREE from 'three/webgpu';

        class PulpCanvas {
            constructor(canvas) {
                this._canvas = canvas;
                this.style = canvas.style || {};
            }
            get width() { return this._canvas.width; }
            set width(value) { this._canvas.width = value; }
            get height() { return this._canvas.height; }
            set height(value) { this._canvas.height = value; }
            get clientWidth() { return this._canvas.width; }
            get clientHeight() { return this._canvas.height; }
            addEventListener(type, fn, opts) { return this._canvas.addEventListener(type, fn, opts); }
            removeEventListener(type, fn, opts) { return this._canvas.removeEventListener(type, fn, opts); }
            dispatchEvent(event) { return this._canvas.dispatchEvent(event); }
        }

        globalThis.__phase13ParticleState = { status: 'starting', beat: 0, rms: 0 };

        const shell = document.createElement('div');
        shell.id = 'phase13-particle-shell';
        document.body.appendChild(shell);

        const title = document.createElement('h2');
        title.id = 'phase13-particle-title';
        title.textContent = 'Pulp Native Particle Visualizer';
        shell.appendChild(title);

        const canvas = document.createElement('canvas');
        canvas.id = 'phase13-particle-canvas';
        canvas.width = 160;
        canvas.height = 160;
        shell.appendChild(canvas);

        const context = canvas.getContext('webgpu');
        const renderer = new THREE.WebGPURenderer({
            canvas: new PulpCanvas(canvas),
            context,
            antialias: false
        });
        await renderer.init();

        const frame = typeof __readSpectrumFrame__ === 'function' ? __readSpectrumFrame__() : { bars: [], rms: 0, beat: 0, time: 0 };
        const scene = new THREE.Scene();
        scene.background = new THREE.Color(0x050816);
        const camera = new THREE.PerspectiveCamera(70, 1, 0.1, 10);
        camera.position.z = 5.6;
        camera.position.y = 0.7;

        const particleCount = 320;
        const positions = new Float32Array(particleCount * 3);
        const colors = new Float32Array(particleCount * 3);
        const bars = Array.isArray(frame.bars) ? frame.bars : [];
        const color = new THREE.Color();
        for (let i = 0; i < particleCount; ++i) {
            const ring = 0.45 + (i % 20) * 0.06;
            const angle = (i / particleCount) * Math.PI * 12.0;
            const band = Number(bars[i % 24] || 0.2);
            const idx = i * 3;
            positions[idx + 0] = Math.cos(angle) * ring;
            positions[idx + 1] = ((i % 18) - 9) * 0.09 + Number(frame.beat || 0) * 0.15;
            positions[idx + 2] = Math.sin(angle) * ring * 0.72;
            color.setHSL(0.56 - band * 0.28, 0.9, 0.5 + Number(frame.rms || 0) * 0.12);
            colors[idx + 0] = color.r;
            colors[idx + 1] = color.g;
            colors[idx + 2] = color.b;
        }

        const geometry = new THREE.BufferGeometry();
        geometry.setAttribute('position', new THREE.BufferAttribute(positions, 3));
        geometry.setAttribute('color', new THREE.BufferAttribute(colors, 3));
        const points = new THREE.Points(
            geometry,
            new THREE.PointsMaterial({ size: 0.13, vertexColors: true, sizeAttenuation: true })
        );
        points.rotation.x = -0.12;
        points.rotation.y = 0.35;
        scene.add(points);

        const pulseCore = new THREE.Mesh(
            new THREE.SphereGeometry(0.38, 20, 16),
            new THREE.MeshBasicMaterial({ color: 0x7dd3fc })
        );
        pulseCore.position.z = -0.3;
        pulseCore.scale.setScalar(1.0 + Number(frame.beat || 0) * 1.1);
        scene.add(pulseCore);

        renderer.render(scene, camera);
        if (typeof context.present === 'function') context.present();
        renderer.render(scene, camera);
        if (typeof context.present === 'function') context.present();

        globalThis.__phase13ParticleState = {
            status: 'ready',
            beat: Number(frame.beat || 0),
            rms: Number(frame.rms || 0),
            titleText: title.textContent
        };

        export default true;
    )JS", resolve_threejs_module,
    [&](const std::string& error, const choc::value::Value&) {
        module_completed = true;
        module_error = error;
    });

    for (int i = 0; i < 256; ++i) {
        env.engine.pump_message_loop();
        if (eval_string(env.engine, "globalThis.__phase13ParticleState.status") == "ready") {
            break;
        }
    }

    REQUIRE(module_completed);
    REQUIRE(module_error.empty());
    REQUIRE(eval_string(env.engine, "globalThis.__phase13ParticleState.status") == "ready");
    REQUIRE(eval_string(env.engine, "globalThis.__phase13ParticleState.titleText") == "Pulp Native Particle Visualizer");

    env.root.layout_children();

    const auto native_id = eval_string(env.engine, "document.getElementById('phase13-particle-canvas')._id");
    auto* widget = dynamic_cast<CanvasWidget*>(env.bridge->widget(native_id));
    REQUIRE(widget != nullptr);

    auto skia = pulp::render::SkiaSurface::create(*env.gpu_surface, {.width = 160, .height = 160});
    REQUIRE(skia != nullptr);
    REQUIRE(skia->is_available());
    REQUIRE(env.gpu_surface->begin_frame());

    auto* canvas = skia->begin_frame();
    REQUIRE(canvas != nullptr);
    widget->paint(*canvas);
    REQUIRE(widget->last_native_gpu_texture_draw_succeeded());
    skia->end_frame();

    std::vector<uint8_t> pixels;
    uint32_t pixel_width = 0;
    uint32_t pixel_height = 0;
    REQUIRE(skia->read_current_rgba(pixels, pixel_width, pixel_height));
    env.gpu_surface->end_frame();

    const auto center = ((pixel_height / 2u) * pixel_width + (pixel_width / 2u)) * 4u;
    REQUIRE(center + 3 < pixels.size());
    const auto r = static_cast<int>(pixels[center + 0]);
    const auto g = static_cast<int>(pixels[center + 1]);
    const auto b = static_cast<int>(pixels[center + 2]);
    INFO("particle-center-r=" << r);
    INFO("particle-center-g=" << g);
    INFO("particle-center-b=" << b);
REQUIRE(g > 85);
REQUIRE(b > 85);
REQUIRE(g + b > r + 70);
}


TEST_CASE("Three.js waveform ribbon mode renders a streaming audio-reactive ribbon through the native bridge", "[threejs][gpu][phase13][ribbon]") {
    if (!is_engine_available(JsEngineType::v8)) {
        SKIP("V8 is required for native Three.js smoke");
    }

    NativeV8Environment env(320, 220);
    if (!env.has_native_gpu()) {
        SKIP("Native Dawn adapter unavailable on this host/backend");
    }

    env.bridge->load_script("");
    env.engine.register_function("__readSpectrumFrame__", [](choc::javascript::ArgumentList) {
        auto result = choc::value::createObject("");
        result.addMember("bars", choc::value::createArray(24, [](uint32_t index) {
            const auto phase = static_cast<double>(index) / 23.0;
            return choc::value::createFloat64(0.18 + 0.82 * std::sin(phase * 3.14159265358979323846));
        }));
        result.addMember("peak", choc::value::createFloat64(0.88));
        result.addMember("rms", choc::value::createFloat64(0.74));
        result.addMember("beat", choc::value::createFloat64(0.91));
        result.addMember("time", choc::value::createFloat64(0.42));
        return result;
    });

    bool module_completed = false;
    std::string module_error;
    env.engine.run_module(R"JS(
        import * as THREE from 'three/webgpu';

        class PulpCanvas {
            constructor(canvas) {
                this._canvas = canvas;
                this.style = canvas.style || {};
            }
            get width() { return this._canvas.width; }
            set width(value) { this._canvas.width = value; }
            get height() { return this._canvas.height; }
            set height(value) { this._canvas.height = value; }
            get clientWidth() { return this._canvas.width; }
            get clientHeight() { return this._canvas.height; }
            addEventListener(type, fn, opts) { return this._canvas.addEventListener(type, fn, opts); }
            removeEventListener(type, fn, opts) { return this._canvas.removeEventListener(type, fn, opts); }
            dispatchEvent(event) { return this._canvas.dispatchEvent(event); }
        }

        globalThis.__phase13RibbonState = { status: 'starting', beat: 0, rms: 0 };

        const shell = document.createElement('div');
        shell.id = 'phase13-ribbon-shell';
        document.body.appendChild(shell);

        const title = document.createElement('h2');
        title.id = 'phase13-ribbon-title';
        title.textContent = 'Pulp Native Waveform Ribbon';
        shell.appendChild(title);

        const canvas = document.createElement('canvas');
        canvas.id = 'phase13-ribbon-canvas';
        canvas.width = 180;
        canvas.height = 140;
        shell.appendChild(canvas);

        const context = canvas.getContext('webgpu');
        const renderer = new THREE.WebGPURenderer({
            canvas: new PulpCanvas(canvas),
            context,
            antialias: false
        });
        await renderer.init();

        const frame = typeof __readSpectrumFrame__ === 'function' ? __readSpectrumFrame__() : { bars: [], rms: 0, beat: 0, time: 0 };
        const scene = new THREE.Scene();
        scene.background = new THREE.Color(0x040814);
        const camera = new THREE.PerspectiveCamera(70, 180 / 140, 0.1, 10);
        camera.position.z = 6.2;
        camera.position.y = 0.45;

        const ribbonSegments = 96;
        const ribbonVertexCount = (ribbonSegments + 1) * 2;
        const positions = new Float32Array(ribbonVertexCount * 3);
        const colors = new Float32Array(ribbonVertexCount * 3);
        const indices = new Uint16Array(ribbonSegments * 6);
        const bars = Array.isArray(frame.bars) ? frame.bars : [];
        const color = new THREE.Color();
        for (let i = 0; i < ribbonSegments; ++i) {
            const vertex = i * 2;
            const idx = i * 6;
            indices[idx + 0] = vertex;
            indices[idx + 1] = vertex + 1;
            indices[idx + 2] = vertex + 2;
            indices[idx + 3] = vertex + 1;
            indices[idx + 4] = vertex + 3;
            indices[idx + 5] = vertex + 2;
        }

        for (let i = 0; i <= ribbonSegments; ++i) {
            const t = i / ribbonSegments;
            const band = Number(bars[Math.min(23, Math.floor(t * 24))] || 0.0);
            const x = (t - 0.5) * 6.8;
            const center = Math.sin(t * 3.14159265358979323846 * 2.0 + Number(frame.time || 0) * 2.1) * (0.12 + band * 0.3)
                + (band - 0.5) * 0.34
                + Number(frame.beat || 0) * 0.18;
            const half = 0.18 + Number(frame.rms || 0) * 0.2 + band * 0.07;
            const wave = Math.cos(t * 3.14159265358979323846 * 4.0 + Number(frame.time || 0) * 1.8) * (0.08 + band * 0.13);
            const lift = Math.sin(t * 3.14159265358979323846 * 1.5) * 0.08;
            const vertex = i * 2;
            const topIdx = vertex * 3;
            const bottomIdx = (vertex + 1) * 3;
            positions[topIdx + 0] = x;
            positions[topIdx + 1] = center + half;
            positions[topIdx + 2] = wave + lift;
            positions[bottomIdx + 0] = x;
            positions[bottomIdx + 1] = center - half;
            positions[bottomIdx + 2] = wave - lift;
            color.setHSL(0.58 - band * 0.28, 0.92, 0.56 + Number(frame.rms || 0) * 0.12);
            colors[topIdx + 0] = color.r;
            colors[topIdx + 1] = color.g;
            colors[topIdx + 2] = color.b;
            colors[bottomIdx + 0] = color.r * 0.42;
            colors[bottomIdx + 1] = color.g * 0.45;
            colors[bottomIdx + 2] = color.b * 0.72;
        }

        const geometry = new THREE.BufferGeometry();
        geometry.setAttribute('position', new THREE.BufferAttribute(positions, 3));
        geometry.setAttribute('color', new THREE.BufferAttribute(colors, 3));
        geometry.setIndex(new THREE.BufferAttribute(indices, 1));
        const ribbon = new THREE.Mesh(
            geometry,
            new THREE.MeshBasicMaterial({ vertexColors: true, side: THREE.DoubleSide })
        );
        ribbon.rotation.x = -0.2;
        ribbon.rotation.y = 0.18;
        scene.add(ribbon);

        const glow = new THREE.Mesh(
            new THREE.SphereGeometry(0.22, 20, 16),
            new THREE.MeshBasicMaterial({ color: 0x7dd3fc })
        );
        glow.position.z = -0.15;
        glow.scale.setScalar(1.0 + Number(frame.beat || 0) * 0.8);
        scene.add(glow);

        renderer.render(scene, camera);
        if (typeof context.present === 'function') context.present();
        renderer.render(scene, camera);
        if (typeof context.present === 'function') context.present();

        globalThis.__phase13RibbonState = {
            status: 'ready',
            beat: Number(frame.beat || 0),
            rms: Number(frame.rms || 0),
            titleText: title.textContent
        };

        export default true;
    )JS", resolve_threejs_module,
    [&](const std::string& error, const choc::value::Value&) {
        module_completed = true;
        module_error = error;
    });

    for (int i = 0; i < 256; ++i) {
        env.engine.pump_message_loop();
        if (eval_string(env.engine, "globalThis.__phase13RibbonState.status") == "ready") {
            break;
        }
    }

    REQUIRE(module_completed);
    REQUIRE(module_error.empty());
    REQUIRE(eval_string(env.engine, "globalThis.__phase13RibbonState.status") == "ready");
    REQUIRE(eval_string(env.engine, "globalThis.__phase13RibbonState.titleText") == "Pulp Native Waveform Ribbon");

    env.root.layout_children();

    const auto native_id = eval_string(env.engine, "document.getElementById('phase13-ribbon-canvas')._id");
    auto* widget = dynamic_cast<CanvasWidget*>(env.bridge->widget(native_id));
    REQUIRE(widget != nullptr);

    auto skia = pulp::render::SkiaSurface::create(*env.gpu_surface, {.width = 180, .height = 140});
    REQUIRE(skia != nullptr);
    REQUIRE(skia->is_available());
    REQUIRE(env.gpu_surface->begin_frame());

    auto* canvas = skia->begin_frame();
    REQUIRE(canvas != nullptr);
    widget->paint(*canvas);
    REQUIRE(widget->last_native_gpu_texture_draw_succeeded());
    skia->end_frame();

    std::vector<uint8_t> pixels;
    uint32_t pixel_width = 0;
    uint32_t pixel_height = 0;
    REQUIRE(skia->read_current_rgba(pixels, pixel_width, pixel_height));
    env.gpu_surface->end_frame();

    const auto center = ((pixel_height / 2u) * pixel_width + (pixel_width / 2u)) * 4u;
    REQUIRE(center + 3 < pixels.size());
    const auto r = static_cast<int>(pixels[center + 0]);
    const auto g = static_cast<int>(pixels[center + 1]);
    const auto b = static_cast<int>(pixels[center + 2]);
    INFO("ribbon-center-r=" << r);
    INFO("ribbon-center-g=" << g);
    INFO("ribbon-center-b=" << b);
    size_t bright_pixels = 0;
    for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
        const auto pr = static_cast<int>(pixels[i + 0]);
        const auto pg = static_cast<int>(pixels[i + 1]);
        const auto pb = static_cast<int>(pixels[i + 2]);
        if (pg > 70 && pb > 80 && pg + pb > pr + 50) {
            ++bright_pixels;
        }
    }
    INFO("ribbon-bright-pixels=" << bright_pixels);
    REQUIRE(bright_pixels > 20);
}

TEST_CASE("Three.js room reverb mode renders parameter-driven room geometry through the native bridge", "[threejs][gpu][phase13][reverb]") {
    if (!is_engine_available(JsEngineType::v8)) {
        SKIP("V8 is required for native Three.js smoke");
    }

    NativeV8Environment env(320, 220);
    if (!env.has_native_gpu()) {
        SKIP("Native Dawn adapter unavailable on this host/backend");
    }

    auto make_reverb_frame = [](double roomWidth,
                                double roomDepth,
                                double roomHeight,
                                double absorption,
                                double sourceX,
                                double sourceY,
                                double sourceZ,
                                double listenerX,
                                double listenerY,
                                double listenerZ,
                                double waveformPhase,
                                double time,
                                double beat,
                                double rms) {
        auto result = choc::value::createObject("");
        result.addMember("waveform", choc::value::createArray(24, [=](uint32_t index) {
            const auto t = static_cast<double>(index) / 23.0;
            const auto wave = std::sin(t * 6.283185307179586 + waveformPhase) * 0.58
                + std::cos(t * 3.14159265358979323846 * 2.0 + waveformPhase * 0.5) * 0.24;
            return choc::value::createFloat64(wave);
        }));
        result.addMember("rms", choc::value::createFloat64(rms));
        result.addMember("beat", choc::value::createFloat64(beat));
        result.addMember("time", choc::value::createFloat64(time));
        result.addMember("roomWidth", choc::value::createFloat64(roomWidth));
        result.addMember("roomDepth", choc::value::createFloat64(roomDepth));
        result.addMember("roomHeight", choc::value::createFloat64(roomHeight));
        result.addMember("absorption", choc::value::createFloat64(absorption));
        result.addMember("sourceX", choc::value::createFloat64(sourceX));
        result.addMember("sourceY", choc::value::createFloat64(sourceY));
        result.addMember("sourceZ", choc::value::createFloat64(sourceZ));
        result.addMember("listenerX", choc::value::createFloat64(listenerX));
        result.addMember("listenerY", choc::value::createFloat64(listenerY));
        result.addMember("listenerZ", choc::value::createFloat64(listenerZ));
        return result;
    };

    env.bridge->load_script("");
    env.engine.register_function("__readSpectrumFrame__", [make_reverb_frame](choc::javascript::ArgumentList) {
        return make_reverb_frame(6.8, 5.4, 3.4, 0.22, 0.85, 0.72, -1.0, -0.58, 0.44, 1.16, 0.95, 0.18, 0.24, 0.82);
    });
    env.engine.register_function("__readSpectrumFrameB__", [make_reverb_frame](choc::javascript::ArgumentList) {
        return make_reverb_frame(8.4, 6.2, 4.0, 0.64, -1.08, 0.82, -1.18, 0.62, 0.50, 1.36, 1.12, 0.77, 0.56, 0.48);
    });

    bool module_completed = false;
    std::string module_error;
    env.engine.run_module(R"JS(
        import * as THREE from 'three/webgpu';

        class PulpCanvas {
            constructor(canvas) {
                this._canvas = canvas;
                this.style = canvas.style || {};
            }
            get width() { return this._canvas.width; }
            set width(value) { this._canvas.width = value; }
            get height() { return this._canvas.height; }
            set height(value) { this._canvas.height = value; }
            get clientWidth() { return this._canvas.width; }
            get clientHeight() { return this._canvas.height; }
            addEventListener(type, fn, opts) { return this._canvas.addEventListener(type, fn, opts); }
            removeEventListener(type, fn, opts) { return this._canvas.removeEventListener(type, fn, opts); }
            dispatchEvent(event) { return this._canvas.dispatchEvent(event); }
        }

        globalThis.__phase13ReverbState = { status: 'starting', roomWidth: 0, updatedRoomWidth: 0 };

        const shell = document.createElement('div');
        shell.id = 'phase13-reverb-shell';
        shell.style.flexDirection = 'row';
        shell.style.gap = '12px';
        shell.style.width = '296px';
        shell.style.height = '196px';
        shell.style.backgroundColor = '#0c1220';
        shell.style.borderRadius = '16px';
        shell.style.padding = '12px';
        document.body.appendChild(shell);

        const sceneColumn = document.createElement('div');
        sceneColumn.id = 'phase13-reverb-scene-column';
        sceneColumn.style.flexGrow = '1';
        sceneColumn.style.gap = '8px';
        shell.appendChild(sceneColumn);

        const title = document.createElement('h2');
        title.id = 'phase13-reverb-title';
        title.textContent = 'Pulp Native Room Reverb';
        sceneColumn.appendChild(title);

        const subtitle = document.createElement('p');
        subtitle.id = 'phase13-reverb-subtitle';
        subtitle.textContent = 'Room shell, reflection lines, and diffuse late-reverb cloud';
        sceneColumn.appendChild(subtitle);

        const canvas = document.createElement('canvas');
        canvas.id = 'phase13-reverb-canvas';
        canvas.width = 180;
        canvas.height = 140;
        sceneColumn.appendChild(canvas);

        const context = canvas.getContext('webgpu');
        const renderer = new THREE.WebGPURenderer({
            canvas: new PulpCanvas(canvas),
            context,
            antialias: false
        });
        await renderer.init();

        const scene = new THREE.Scene();
        scene.background = new THREE.Color(0x040816);
        const camera = new THREE.PerspectiveCamera(68, 180 / 140, 0.1, 20);
        camera.position.set(0.55, 1.05, 7.1);
        camera.lookAt(0.0, 1.05, 0.0);

        const roomGroup = new THREE.Group();
        roomGroup.position.y = 0.03;
        scene.add(roomGroup);

        const roomShell = new THREE.Mesh(
            new THREE.BoxGeometry(1, 1, 1),
            new THREE.MeshBasicMaterial({ color: 0x1e293b, wireframe: true, transparent: true, opacity: 0.82 })
        );
        roomGroup.add(roomShell);

        const source = new THREE.Mesh(
            new THREE.SphereGeometry(0.1, 14, 10),
            new THREE.MeshBasicMaterial({ color: 0xfb7185 })
        );
        roomGroup.add(source);

        const listener = new THREE.Mesh(
            new THREE.SphereGeometry(0.12, 14, 10),
            new THREE.MeshBasicMaterial({ color: 0x7dd3fc })
        );
        roomGroup.add(listener);

        const reflectionLines = [];
        const reflectionGeometry = [];
        const reflectionPalette = [0xfef08a, 0x7dd3fc, 0xa3e635, 0xf472b6, 0x60a5fa, 0xf8fafc];
        for (let i = 0; i < 6; ++i) {
            const geometry = new THREE.BufferGeometry();
            geometry.setAttribute('position', new THREE.BufferAttribute(new Float32Array(9), 3));
            reflectionGeometry.push(geometry);
            const line = new THREE.Line(
                geometry,
                new THREE.LineBasicMaterial({
                    color: reflectionPalette[i],
                    transparent: true,
                    opacity: 0.92
                })
            );
            roomGroup.add(line);
            reflectionLines.push(line);
        }

        const cloudCount = 120;
        const cloudPositions = new Float32Array(cloudCount * 3);
        const cloudColors = new Float32Array(cloudCount * 3);
        const cloudMeta = [];
        for (let i = 0; i < cloudCount; ++i) {
            cloudMeta.push({
                radius: 0.28 + (i % 11) * 0.055,
                angle: (i / cloudCount) * Math.PI * 14.0,
                phase: (i % 17) * 0.11
            });
            const idx = i * 3;
            cloudPositions[idx + 0] = Math.cos(i * 0.27) * 0.35;
            cloudPositions[idx + 1] = 0.32 + Math.sin(i * 0.19) * 0.18;
            cloudPositions[idx + 2] = Math.sin(i * 0.23) * 0.35;
            cloudColors[idx + 0] = 0.48;
            cloudColors[idx + 1] = 0.74;
            cloudColors[idx + 2] = 1.0;
        }

        const cloudGeometry = new THREE.BufferGeometry();
        cloudGeometry.setAttribute('position', new THREE.BufferAttribute(cloudPositions, 3));
        cloudGeometry.setAttribute('color', new THREE.BufferAttribute(cloudColors, 3));
        const cloud = new THREE.Points(
            cloudGeometry,
            new THREE.PointsMaterial({
                size: 0.08,
                vertexColors: true,
                transparent: true,
                opacity: 0.68,
                sizeAttenuation: true
            })
        );
        roomGroup.add(cloud);

        const tmpColor = new THREE.Color();
        let readSpectrumFrame = () => (typeof __readSpectrumFrame__ === 'function' ? __readSpectrumFrame__() : {});
        function updateReverbScene() {
            const frame = readSpectrumFrame();
            const waveform = Array.isArray(frame.waveform) ? frame.waveform : [];
            const roomWidth = Math.max(4.2, Number(frame.roomWidth || 6.8));
            const roomDepth = Math.max(3.8, Number(frame.roomDepth || 5.4));
            const roomHeight = Math.max(2.4, Number(frame.roomHeight || 3.4));
            const absorption = Math.max(0.0, Math.min(1.0, Number(frame.absorption || 0.0)));
            const sourcePos = new THREE.Vector3(
                Number(frame.sourceX || 0.0),
                Math.max(0.2, Number(frame.sourceY || 0.6)),
                Number(frame.sourceZ || -1.0)
            );
            const listenerPos = new THREE.Vector3(
                Number(frame.listenerX || 0.0),
                Math.max(0.2, Number(frame.listenerY || 0.45)),
                Number(frame.listenerZ || 1.0)
            );
            const waveformPeak = waveform.reduce((peak, value) => Math.max(peak, Math.abs(Number(value || 0.0))), 0.0);

            roomShell.scale.set(roomWidth, roomHeight, roomDepth);
            roomShell.position.y = roomHeight * 0.5;
            roomShell.material.opacity = Math.max(0.3, 0.86 - absorption * 0.5);
            source.position.copy(sourcePos);
            listener.position.copy(listenerPos);

            let reflectionSampleX = 0.0;
            for (let i = 0; i < reflectionLines.length; ++i) {
                const line = reflectionLines[i];
                const positions = line.geometry.getAttribute('position');
                const wallAxis = i % 3;
                const midpoint = new THREE.Vector3(
                    (sourcePos.x + listenerPos.x) * 0.5,
                    (sourcePos.y + listenerPos.y) * 0.5,
                    (sourcePos.z + listenerPos.z) * 0.5
                );
                if (wallAxis === 0) {
                    midpoint.x = i < 3 ? -roomWidth * 0.5 : roomWidth * 0.5;
                    midpoint.y += Math.sin(i * 0.9) * (0.06 + waveformPeak * 0.12);
                    midpoint.z += Math.cos(i * 1.1) * (0.05 + waveformPeak * 0.09);
                } else if (wallAxis === 1) {
                    midpoint.y = i < 3 ? 0.0 : roomHeight;
                    midpoint.x += Math.sin(i * 1.2) * (0.08 + waveformPeak * 0.08);
                    midpoint.z += Math.cos(i * 0.8) * (0.04 + waveformPeak * 0.08);
                } else {
                    midpoint.z = i < 3 ? -roomDepth * 0.5 : roomDepth * 0.5;
                    midpoint.x += Math.sin(i * 1.3) * (0.06 + waveformPeak * 0.1);
                    midpoint.y += Math.cos(i * 1.0) * (0.05 + waveformPeak * 0.09);
                }
                positions.setXYZ(0, sourcePos.x, sourcePos.y, sourcePos.z);
                positions.setXYZ(1, midpoint.x, midpoint.y, midpoint.z);
                positions.setXYZ(2, listenerPos.x, listenerPos.y, listenerPos.z);
                positions.needsUpdate = true;
                line.material.color.setHSL(0.14 + i * 0.06, 1.0, 0.72 + waveformPeak * 0.06);
                line.material.opacity = Math.max(0.45, 0.9 - absorption * 0.3);
                if (i === 0) {
                    reflectionSampleX = midpoint.x;
                }
            }

            let cloudSampleX = 0.0;
            for (let i = 0; i < cloudMeta.length; ++i) {
                const meta = cloudMeta[i];
                const idx = i * 3;
                const sample = waveform.length > 0 ? Number(waveform[i % waveform.length] || 0.0) : 0.0;
                const energy = Math.min(1.0, Math.abs(sample) * 1.7 + absorption * 0.4);
                const swirl = meta.angle + Number(frame.time || 0.0) * (0.45 + meta.phase * 0.03) + sample * 0.7;
                const radius = 0.34 + waveformPeak * 0.5 + meta.radius * (0.5 + energy * 0.65);
                cloudPositions[idx + 0] = listenerPos.x + Math.cos(swirl) * radius;
                cloudPositions[idx + 1] = listenerPos.y + Math.sin(Number(frame.time || 0.0) * 2.0 + meta.phase) * (0.09 + energy * 0.2) + sample * 0.08;
                cloudPositions[idx + 2] = listenerPos.z + Math.sin(swirl) * radius * 0.56;
                tmpColor.setHSL(0.57 - energy * 0.18, 0.9, 0.54 + energy * 0.2);
                cloudColors[idx + 0] = tmpColor.r;
                cloudColors[idx + 1] = tmpColor.g;
                cloudColors[idx + 2] = tmpColor.b;
                if (i === 0) {
                    cloudSampleX = cloudPositions[idx + 0];
                }
            }
            cloudGeometry.getAttribute('position').needsUpdate = true;
            cloudGeometry.getAttribute('color').needsUpdate = true;
            cloud.rotation.y += 0.002;
            roomGroup.rotation.y += 0.0015;

            return {
                roomWidth,
                roomDepth,
                roomHeight,
                absorption,
                waveformPeak,
                reflectionSampleX,
                cloudSampleX
            };
        }

        const firstFrame = updateReverbScene();
        renderer.render(scene, camera);
        if (typeof context.present === 'function') context.present();

        readSpectrumFrame = () => (typeof __readSpectrumFrameB__ === 'function' ? __readSpectrumFrameB__() : {});
        const secondFrame = updateReverbScene();
        renderer.render(scene, camera);
        if (typeof context.present === 'function') context.present();

        globalThis.__phase13ReverbState = {
            status: 'ready',
            step: 'done',
            titleText: title.textContent,
            firstRoomWidth: firstFrame.roomWidth,
            secondRoomWidth: secondFrame.roomWidth,
            firstRoomHeight: firstFrame.roomHeight,
            secondRoomHeight: secondFrame.roomHeight,
            firstAbsorption: firstFrame.absorption,
            secondAbsorption: secondFrame.absorption,
            firstReflectionSampleX: firstFrame.reflectionSampleX,
            secondReflectionSampleX: secondFrame.reflectionSampleX,
            firstCloudSampleX: firstFrame.cloudSampleX,
            secondCloudSampleX: secondFrame.cloudSampleX,
            waveformPeak: secondFrame.waveformPeak
        };

        export default true;
    )JS", resolve_threejs_module,
    [&](const std::string& error, const choc::value::Value&) {
        module_completed = true;
        module_error = error;
    });

    for (int i = 0; i < 256; ++i) {
        env.engine.pump_message_loop();
        if (eval_string(env.engine, "globalThis.__phase13ReverbState.status") == "ready") {
            break;
        }
    }

    REQUIRE(module_completed);
    REQUIRE(module_error.empty());
    REQUIRE(eval_string(env.engine, "globalThis.__phase13ReverbState.status") == "ready");
    REQUIRE(eval_string(env.engine, "globalThis.__phase13ReverbState.titleText") == "Pulp Native Room Reverb");
    REQUIRE(eval_i32(env.engine, "Math.round(globalThis.__phase13ReverbState.firstRoomWidth * 10)") != eval_i32(env.engine, "Math.round(globalThis.__phase13ReverbState.secondRoomWidth * 10)"));
    REQUIRE(eval_i32(env.engine, "Math.round(globalThis.__phase13ReverbState.firstRoomHeight * 10)") != eval_i32(env.engine, "Math.round(globalThis.__phase13ReverbState.secondRoomHeight * 10)"));
    REQUIRE(eval_i32(env.engine, "Math.round(globalThis.__phase13ReverbState.firstAbsorption * 100)") != eval_i32(env.engine, "Math.round(globalThis.__phase13ReverbState.secondAbsorption * 100)"));
    REQUIRE(eval_i32(env.engine, "Math.round(globalThis.__phase13ReverbState.firstReflectionSampleX * 100)") != eval_i32(env.engine, "Math.round(globalThis.__phase13ReverbState.secondReflectionSampleX * 100)"));
    REQUIRE(eval_i32(env.engine, "Math.round(globalThis.__phase13ReverbState.firstCloudSampleX * 100)") != eval_i32(env.engine, "Math.round(globalThis.__phase13ReverbState.secondCloudSampleX * 100)"));

    env.root.layout_children();

    const auto native_id = eval_string(env.engine, "document.getElementById('phase13-reverb-canvas')._id");
    auto* widget = dynamic_cast<CanvasWidget*>(env.bridge->widget(native_id));
    REQUIRE(widget != nullptr);

    auto skia = pulp::render::SkiaSurface::create(*env.gpu_surface, {.width = 180, .height = 140});
    REQUIRE(skia != nullptr);
    REQUIRE(skia->is_available());
    REQUIRE(env.gpu_surface->begin_frame());

    auto* canvas = skia->begin_frame();
    REQUIRE(canvas != nullptr);
    widget->paint(*canvas);
    REQUIRE(widget->last_native_gpu_texture_draw_succeeded());
    skia->end_frame();

    std::vector<uint8_t> pixels;
    uint32_t pixel_width = 0;
    uint32_t pixel_height = 0;
    REQUIRE(skia->read_current_rgba(pixels, pixel_width, pixel_height));
    env.gpu_surface->end_frame();

    REQUIRE(pixel_width == 180);
    REQUIRE(pixel_height == 140);
    size_t bright_pixels = 0;
    for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
        const auto pr = static_cast<int>(pixels[i + 0]);
        const auto pg = static_cast<int>(pixels[i + 1]);
        const auto pb = static_cast<int>(pixels[i + 2]);
        if (pg > 70 && pb > 60 && pg + pb > pr + 30) {
            ++bright_pixels;
        }
    }
    INFO("reverb-bright-pixels=" << bright_pixels);
    REQUIRE(bright_pixels > 12);
}
