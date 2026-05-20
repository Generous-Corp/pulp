#include <pulp/render/gpu_surface.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/js_engine.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/visualization_bridge.hpp>
#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/window_host.hpp>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#if defined(_WIN32)
#include <winsock2.h>
#else
#include <unistd.h>
#endif

#ifdef PULP_BENCHMARK
#include <pulp/render/bench/perf_counters.hpp>
#include <choc/text/choc_JSON.h>
#endif

using namespace pulp::view;

namespace {

enum class DemoMode {
    cube,
    spectrum,
    particles,
    ribbon,
    reverb
};

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.good()) {
        throw std::runtime_error("Cannot read file: " + path.string());
    }
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
    return std::nullopt;
}

std::string eval_string(ScriptEngine& engine, const std::string& code) {
    return std::string(engine.evaluate(code).getWithDefault<std::string_view>(""));
}

void write_binary_file(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary);
    if (!out.good()) {
        throw std::runtime_error("Cannot write file: " + path.string());
    }
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    if (!out.good()) {
        throw std::runtime_error("Failed to write file: " + path.string());
    }
}

std::string demo_mode_name(DemoMode mode) {
    switch (mode) {
        case DemoMode::cube: return "cube";
        case DemoMode::spectrum: return "spectrum";
        case DemoMode::particles: return "particles";
        case DemoMode::ribbon: return "ribbon";
        case DemoMode::reverb: return "reverb";
    }
    return "cube";
}

struct SyntheticSpectrumSource {
    static constexpr int kBlockSize = 128;
    static constexpr int kBars = 24;

    VisualizationBridge bridge;
    bool capture_waveform = false;
    float sample_rate = 48000.0f;
    double elapsed_seconds = 0.0;
    float last_rms = 0.0f;
    float last_beat = 0.0f;
    std::array<float, kBlockSize> block{};

    explicit SyntheticSpectrumSource(bool capture_waveform_enabled = false)
        : capture_waveform(capture_waveform_enabled) {
        VisualizationConfig config;
        config.fft_size = 512;
        config.hop_size = 128;
        config.num_channels = 1;
        config.sample_rate = sample_rate;
        config.capture_waveform = capture_waveform;
        config.waveform_length = capture_waveform ? 192 : 0;
        bridge.configure(config);
    }

    void advance(float dt_seconds) {
        const auto frames = std::max(1, static_cast<int>(std::ceil(std::max(dt_seconds, 1.0f / 120.0f) * sample_rate / kBlockSize)));
        for (int frame = 0; frame < frames; ++frame) {
            float block_energy = 0.0f;
            float block_beat = 0.0f;
            for (int i = 0; i < kBlockSize; ++i) {
                const auto t = elapsed_seconds + static_cast<double>(i) / sample_rate;
                const auto beat = std::pow(0.5 + 0.5 * std::sin(2.0 * 3.14159265358979323846 * 1.6 * t), 6.0);
                const auto slow = 0.55 + 0.45 * std::sin(2.0 * 3.14159265358979323846 * 0.17 * t);
                const auto sweep = 0.5 + 0.5 * std::sin(2.0 * 3.14159265358979323846 * 0.09 * t);
                const auto bass = 0.36 * (0.4 + 0.6 * beat) * std::sin(2.0 * 3.14159265358979323846 * 82.0 * t);
                const auto mid = 0.26 * (0.45 + 0.55 * slow) * std::sin(2.0 * 3.14159265358979323846 * (220.0 + 90.0 * sweep) * t);
                const auto air = 0.18 * (0.35 + 0.65 * (1.0 - slow)) * std::sin(2.0 * 3.14159265358979323846 * (860.0 + 340.0 * sweep) * t);
                const auto sample = static_cast<float>(bass + mid + air);
                block[static_cast<size_t>(i)] = sample;
                block_energy += sample * sample;
                block_beat = std::max(block_beat, static_cast<float>(beat));
            }
            const float* channels[] = {block.data()};
            bridge.process(channels, 1, kBlockSize);
            last_rms = std::clamp(std::sqrt(block_energy / static_cast<float>(kBlockSize)) * 1.95f, 0.0f, 1.0f);
            last_beat = std::clamp(0.35f * last_beat + 0.65f * block_beat, 0.0f, 1.0f);
            elapsed_seconds += static_cast<double>(kBlockSize) / sample_rate;
        }
    }

    [[nodiscard]] choc::value::Value read_frame() {
        const auto& spectrum = bridge.read_spectrum();
        std::array<float, kBars> bars{};
        float peak = 0.0f;
        const auto max_bin = std::max(2, spectrum.num_bins - 1);
        for (int bar = 0; bar < kBars; ++bar) {
            const auto t0 = static_cast<float>(bar) / static_cast<float>(kBars);
            const auto t1 = static_cast<float>(bar + 1) / static_cast<float>(kBars);
            const auto start = std::max(1, static_cast<int>(std::pow(static_cast<float>(max_bin), t0)));
            const auto end = std::min(max_bin, std::max(start + 1, static_cast<int>(std::pow(static_cast<float>(max_bin), t1))));
            float db = -120.0f;
            for (int bin = start; bin <= end; ++bin) {
                db = std::max(db, spectrum.magnitude_db[bin]);
            }
            auto normalized = std::clamp((db + 84.0f) / 84.0f, 0.0f, 1.0f);
            normalized = std::pow(normalized, 1.35f);
            bars[static_cast<size_t>(bar)] = normalized;
            peak = std::max(peak, normalized);
        }

        auto result = choc::value::createObject("");
        result.addMember("bars", choc::value::createArray(kBars, [&bars](uint32_t index) {
            return choc::value::createFloat64(static_cast<double>(bars[static_cast<size_t>(index)]));
        }));
        result.addMember("peak", choc::value::createFloat64(static_cast<double>(peak)));
        result.addMember("rms", choc::value::createFloat64(static_cast<double>(last_rms)));
        result.addMember("beat", choc::value::createFloat64(static_cast<double>(last_beat)));
        result.addMember("time", choc::value::createFloat64(elapsed_seconds));
        const auto room_width = 6.2f + 0.7f * std::sin(static_cast<float>(elapsed_seconds) * 0.32f);
        const auto room_depth = 5.2f + 0.5f * std::cos(static_cast<float>(elapsed_seconds) * 0.27f);
        const auto room_height = 3.0f + 0.25f * std::sin(static_cast<float>(elapsed_seconds) * 0.19f);
        const auto absorption = std::clamp(0.28f + 0.24f * (0.5f + 0.5f * std::sin(static_cast<float>(elapsed_seconds) * 0.14f)), 0.0f, 1.0f);
        const auto source_x = 1.15f * std::sin(static_cast<float>(elapsed_seconds) * 0.41f);
        const auto source_y = 0.55f + 0.17f * std::cos(static_cast<float>(elapsed_seconds) * 0.36f);
        const auto source_z = -0.9f + 0.35f * std::sin(static_cast<float>(elapsed_seconds) * 0.23f);
        const auto listener_x = -0.55f + 0.12f * std::cos(static_cast<float>(elapsed_seconds) * 0.18f);
        const auto listener_y = 0.42f + 0.05f * std::sin(static_cast<float>(elapsed_seconds) * 0.16f);
        const auto listener_z = 0.95f + 0.12f * std::cos(static_cast<float>(elapsed_seconds) * 0.29f);
        result.addMember("roomWidth", choc::value::createFloat64(room_width));
        result.addMember("roomDepth", choc::value::createFloat64(room_depth));
        result.addMember("roomHeight", choc::value::createFloat64(room_height));
        result.addMember("absorption", choc::value::createFloat64(absorption));
        result.addMember("sourceX", choc::value::createFloat64(source_x));
        result.addMember("sourceY", choc::value::createFloat64(source_y));
        result.addMember("sourceZ", choc::value::createFloat64(source_z));
        result.addMember("listenerX", choc::value::createFloat64(listener_x));
        result.addMember("listenerY", choc::value::createFloat64(listener_y));
        result.addMember("listenerZ", choc::value::createFloat64(listener_z));
        if (capture_waveform) {
            const auto& waveform = bridge.read_waveform();
            const auto waveform_length = std::min(192, waveform.num_samples);
            result.addMember("waveform", choc::value::createArray(static_cast<uint32_t>(waveform_length), [waveform](uint32_t index) {
                return choc::value::createFloat64(static_cast<double>(waveform.samples[static_cast<size_t>(index)]));
            }));
        } else {
            result.addMember("waveform", choc::value::createArray(0, [](uint32_t) {
                return choc::value::createFloat64(0.0);
            }));
        }
        return result;
    }
};

struct DemoEnvironment {
    View root;
    ScriptEngine engine;
    pulp::state::StateStore store;
    std::unique_ptr<pulp::render::GpuSurface> owned_gpu_surface;
    pulp::render::GpuSurface* gpu_surface = nullptr;
    std::unique_ptr<WidgetBridge> bridge;
    std::unique_ptr<SyntheticSpectrumSource> spectrum_source;

    DemoEnvironment(float width, float height)
        : engine(JsEngineType::v8) {
        root.set_bounds({0, 0, width, height});
        root.set_theme(Theme::dark());
    }

    bool has_native_gpu() const { return gpu_surface != nullptr; }

    void attach_gpu_surface(pulp::render::GpuSurface* surface) {
        gpu_surface = surface;
        bridge = std::make_unique<WidgetBridge>(engine, root, store, gpu_surface);
    }

    void initialize_offscreen_gpu(float width, float height) {
        owned_gpu_surface = pulp::render::GpuSurface::create_dawn();
        if (owned_gpu_surface) {
            pulp::render::GpuSurface::Config config{};
            config.width = static_cast<uint32_t>(std::max(1.0f, width));
            config.height = static_cast<uint32_t>(std::max(1.0f, height));
            config.native_surface_handle = nullptr;
            if (!owned_gpu_surface->initialize(config)) {
                owned_gpu_surface.reset();
            }
        }
        attach_gpu_surface(owned_gpu_surface.get());
    }

    void enable_audio_source(bool capture_waveform = false) {
        spectrum_source = std::make_unique<SyntheticSpectrumSource>(capture_waveform);
        spectrum_source->advance(1.0f / 30.0f);
        engine.register_function("__readSpectrumFrame__", [this](choc::javascript::ArgumentList) {
            if (!spectrum_source) {
                auto empty = choc::value::createObject("");
                empty.addMember("bars", choc::value::createArray(0, [](uint32_t) {
                    return choc::value::createFloat64(0.0);
                }));
                empty.addMember("peak", choc::value::createFloat64(0.0));
                empty.addMember("rms", choc::value::createFloat64(0.0));
                empty.addMember("beat", choc::value::createFloat64(0.0));
                empty.addMember("time", choc::value::createFloat64(0.0));
                empty.addMember("roomWidth", choc::value::createFloat64(0.0));
                empty.addMember("roomDepth", choc::value::createFloat64(0.0));
                empty.addMember("roomHeight", choc::value::createFloat64(0.0));
                empty.addMember("absorption", choc::value::createFloat64(0.0));
                empty.addMember("sourceX", choc::value::createFloat64(0.0));
                empty.addMember("sourceY", choc::value::createFloat64(0.0));
                empty.addMember("sourceZ", choc::value::createFloat64(0.0));
                empty.addMember("listenerX", choc::value::createFloat64(0.0));
                empty.addMember("listenerY", choc::value::createFloat64(0.0));
                empty.addMember("listenerZ", choc::value::createFloat64(0.0));
                empty.addMember("waveform", choc::value::createArray(0, [](uint32_t) {
                    return choc::value::createFloat64(0.0);
                }));
                return empty;
            }
            return spectrum_source->read_frame();
        });
    }

    void advance_sources(float dt_seconds) {
        if (spectrum_source) {
            spectrum_source->advance(dt_seconds);
        }
    }
};

std::string make_threejs_demo_module(int width, int height, DemoMode mode,
                                      int particle_count = 480) {
    // Load JS module from template file to avoid MSVC 16KB string literal limit
    namespace fs = std::filesystem;
    fs::path template_path = fs::path(__FILE__).parent_path() / "demo.js.template";
    if (!fs::exists(template_path)) {
        // Fallback: try relative to the source tree
        template_path = fs::path(PULP_THREEJS_SOURCE_DIR).parent_path().parent_path()
                      / "examples" / "threejs-native-demo" / "demo.js.template";
    }
    std::string js = read_text_file(template_path);
    if (js.empty()) {
        return "console.error('demo.js.template not found'); export default false;";
    }
    auto replace_all = [&](const std::string& from, const std::string& to) {
        size_t pos = 0;
        while ((pos = js.find(from, pos)) != std::string::npos) {
            js.replace(pos, from.length(), to);
            pos += to.length();
        }
    };
    replace_all("__WIDTH__", std::to_string(width));
    replace_all("__HEIGHT__", std::to_string(height));
    replace_all("__MODE__", demo_mode_name(mode));
    replace_all("__PARTICLE_COUNT__", std::to_string(std::max(1, particle_count)));

    return js;
}

// Original inline version preserved as comment for reference
#if 0
std::string make_threejs_demo_module_inline(int width, int height, DemoMode mode) {
    std::ostringstream js;
    js << R"JS(
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

        const rootWidth = )JS" << width << R"JS(;
        const rootHeight = )JS" << height << R"JS(;
        const demoMode = ')JS" << demo_mode_name(mode) << R"JS(';
        const isSpectrum = demoMode === 'spectrum';
        const isParticles = demoMode === 'particles';
        const isRibbon = demoMode === 'ribbon';
        const isReverb = demoMode === 'reverb';
        const hudWidth = 220;
        const sceneWidth = Math.max(260, rootWidth - hudWidth - 72);
        const sceneHeight = Math.max(240, rootHeight - 112);

        globalThis.__pulpThreeDemoState = {
            status: 'starting',
            step: 'module-start',
            message: '',
            layout: 'hybrid-2d-3d'
        };

        document.body.style.backgroundColor = '#050816';
        document.body.style.padding = '18px';

        const appShell = document.createElement('div');
        appShell.id = 'pulp-threejs-native-demo-shell';
        appShell.style.flexDirection = 'row';
        appShell.style.gap = '18px';
        appShell.style.width = Math.max(320, rootWidth - 36) + 'px';
        appShell.style.height = Math.max(280, rootHeight - 36) + 'px';
        appShell.style.backgroundColor = '#101626';
        appShell.style.borderRadius = '20px';
        appShell.style.padding = '20px';
        document.body.appendChild(appShell);

        const sceneColumn = document.createElement('div');
        sceneColumn.id = 'pulp-threejs-scene-column';
        sceneColumn.style.flexGrow = '1';
        sceneColumn.style.gap = '12px';
        appShell.appendChild(sceneColumn);

        const eyebrow = document.createElement('span');
        eyebrow.id = 'pulp-threejs-demo-eyebrow';
        eyebrow.textContent = 'Phase 13 / Native Dawn Bridge';
        eyebrow.style.color = '#7dd3fc';
        eyebrow.style.fontSize = '12px';
        sceneColumn.appendChild(eyebrow);

        const title = document.createElement('h2');
        title.id = 'pulp-threejs-demo-title';
        title.textContent = isSpectrum
            ? 'Pulp Native Spectrum Analyzer'
            : (isParticles
                ? 'Pulp Native Particle Visualizer'
                : (isRibbon
                    ? 'Pulp Native Waveform Ribbon'
                    : (isReverb ? 'Pulp Native Room Reverb Visualizer' : 'Pulp Native Three.js')));
        title.style.color = '#f8fafc';
        title.style.fontSize = '28px';
        title.style.fontWeight = '700';
        sceneColumn.appendChild(title);

        const subtitle = document.createElement('p');
        subtitle.id = 'pulp-threejs-demo-subtitle';
        subtitle.textContent = isSpectrum
            ? 'Real FFT bins flow from Pulp\'s VisualizationBridge into three.webgpu.js bars on the native Dawn canvas.'
            : (isParticles
                ? 'Beat and RMS data from Pulp\'s VisualizationBridge drive a live particle cloud through real three.webgpu.js on the native Dawn canvas.'
                : (isRibbon
                    ? 'Waveform data from Pulp\'s VisualizationBridge drives a 3D ribbon surface through real three.webgpu.js on the native Dawn canvas.'
                    : (isReverb
                        ? 'Waveform and room data from Pulp\'s VisualizationBridge drive a room shell, reflection lines, and a diffuse late-reverb cloud through real three.webgpu.js on the native Dawn canvas.'
                        : 'Hybrid 2D+3D layout: native HUD plus real three.webgpu.js on a Dawn-backed canvas.')));
        subtitle.style.color = '#cbd5e1';
        subtitle.style.fontSize = '14px';
        sceneColumn.appendChild(subtitle);

        const canvasCard = document.createElement('div');
        canvasCard.id = 'pulp-threejs-canvas-card';
        canvasCard.style.backgroundColor = '#0b1020';
        canvasCard.style.borderRadius = '18px';
        canvasCard.style.padding = '12px';
        canvasCard.style.flexGrow = '1';
        sceneColumn.appendChild(canvasCard);

        const canvas = document.createElement('canvas');
        canvas.id = 'pulp-threejs-native-demo-canvas';
        canvas.width = sceneWidth;
        canvas.height = sceneHeight;
        canvas.style.width = sceneWidth + 'px';
        canvas.style.height = sceneHeight + 'px';
        canvas.style.borderRadius = '14px';
        canvasCard.appendChild(canvas);

        const hud = document.createElement('div');
        hud.id = 'pulp-threejs-demo-hud';
        hud.style.width = hudWidth + 'px';
        hud.style.flexShrink = '0';
        hud.style.backgroundColor = '#0b1220';
        hud.style.borderRadius = '18px';
        hud.style.padding = '16px';
        hud.style.gap = '10px';
        appShell.appendChild(hud);

        const hudTitle = document.createElement('h3');
        hudTitle.id = 'pulp-threejs-hud-title';
        hudTitle.textContent = 'Runtime';
        hudTitle.style.color = '#f8fafc';
        hudTitle.style.fontSize = '20px';
        hudTitle.style.fontWeight = '700';
        hud.appendChild(hudTitle);

        function addMetric(labelText, valueText, valueId) {
            const row = document.createElement('div');
            row.style.backgroundColor = '#111c31';
            row.style.borderRadius = '12px';
            row.style.padding = '10px';
            row.style.gap = '4px';

            const label = document.createElement('span');
            label.textContent = labelText;
            label.style.color = '#94a3b8';
            label.style.fontSize = '12px';
            row.appendChild(label);

            const value = document.createElement('span');
            value.id = valueId;
            value.textContent = valueText;
            value.style.color = '#f8fafc';
            value.style.fontSize = '14px';
            value.style.fontWeight = '600';
            row.appendChild(value);

            hud.appendChild(row);
            return value;
        }

        const backendValue = addMetric('Backend', 'booting...', 'pulp-threejs-hud-backend');
        const frameValue = addMetric('Frame', '0', 'pulp-threejs-hud-frame');
        const cameraValue = addMetric('Camera Z', '2.00', 'pulp-threejs-hud-camera');
        const modeValue = addMetric('Demo', isSpectrum ? 'spectrum-analyzer' : (isParticles ? 'particle-visualizer' : (isRibbon ? 'waveform-ribbon' : (isReverb ? 'room-reverb' : 'cube-smoke'))), 'pulp-threejs-hud-mode');
        const interactionValue = addMetric(
            isSpectrum ? 'Signal Peak' : ((isParticles || isRibbon) ? 'RMS / Beat' : (isReverb ? 'Waveform / Abs' : 'Interaction')),
            (isSpectrum || isParticles || isRibbon) ? '0.00' : (isReverb ? '0.00 / 0.00' : 'drag / pinch'),
            'pulp-threejs-hud-interaction'
        );

        const context = canvas.getContext('webgpu');
        const wrappedCanvas = new PulpCanvas(canvas);

        const renderer = new THREE.WebGPURenderer({
            canvas: wrappedCanvas,
            context,
            antialias: false
        });

        await renderer.init();

        const scene = new THREE.Scene();
        scene.background = new THREE.Color(isSpectrum ? 0x08111f : (isParticles ? 0x050816 : (isRibbon ? 0x040814 : (isReverb ? 0x050713 : 0xff0000))));

        const camera = new THREE.PerspectiveCamera(70, sceneWidth / sceneHeight, 0.1, 10);
        camera.position.z = isSpectrum ? 5.4 : (isParticles ? 6.0 : (isRibbon ? 4.15 : (isReverb ? 5.3 : 2)));
        camera.position.y = isSpectrum ? 1.35 : (isParticles ? 0.8 : (isRibbon ? 0.42 : (isReverb ? 1.15 : 0)));

        const controls = new OrbitControls(camera, canvas);
        controls.enableDamping = true;
        controls.enablePan = false;

        let mesh = null;
        let interactionTarget = null;
        let spectrumBars = [];
        let peakBar = null;
        let spectrumPeak = 0;
        let signalRms = 0;
        let signalBeat = 0;
        let ribbonSurface = null;
        let ribbonTube = null;
        let ribbonLine = null;
        let ribbonTubePoints = [];
        let ribbonNodes = [];
        let ribbonPositions = null;
        let ribbonColors = null;
        let ribbonBasePositions = null;
        let ribbonColumns = 0;
        let ribbonRows = 0;
        let ribbonSegments = 0;
        let ribbonBarsGroup = null;
        let ribbonBars = [];
        let ribbonMeta = [];
        let particleCloud = null;
        let particleCore = null;
        let particlePositions = null;
        let particleColors = null;
        let particleMeta = [];
        let reverbRoomGroup = null;
        let reverbRoomFill = null;
        let reverbRoomShell = null;
        let reverbRoomEdges = null;
        let reverbSource = null;
        let reverbListener = null;
        let reverbReflectionLines = [];
        let reverbCloud = null;
        let reverbCloudPositions = null;
        let reverbCloudColors = null;
        let reverbCloudMeta = [];
        let reverbWaveformPeak = 0;
        let reverbAbsorption = 0;
        let reverbRoomWidth = 0;
        let reverbRoomDepth = 0;
        let reverbRoomHeight = 0;
        const particleColor = new THREE.Color();

        if (isSpectrum) {
            const floor = new THREE.Mesh(
                new THREE.PlaneGeometry(9, 5),
                new THREE.MeshBasicMaterial({ color: 0x0b1020 })
            );
            floor.rotation.x = -Math.PI / 2;
            floor.position.y = -0.05;
            scene.add(floor);

            const barsGroup = new THREE.Group();
            barsGroup.position.z = 0.15;
            barsGroup.rotation.x = -0.24;
            const barGeometry = new THREE.BoxGeometry(0.2, 1.0, 0.28);
            for (let i = 0; i < 24; ++i) {
                const bar = new THREE.Mesh(barGeometry, new THREE.MeshBasicMaterial({ color: 0x38bdf8 }));
                bar.position.x = (i - 11.5) * 0.24;
                bar.position.y = 0.12;
                bar.scale.y = 0.2;
                barsGroup.add(bar);
                spectrumBars.push(bar);
            }
            scene.add(barsGroup);

            peakBar = new THREE.Mesh(
                new THREE.BoxGeometry(0.52, 1.0, 0.52),
                new THREE.MeshBasicMaterial({ color: 0xa3e635 })
            );
            peakBar.position.z = -0.7;
            peakBar.position.y = 0.2;
            scene.add(peakBar);

            interactionTarget = barsGroup;
            controls.target.set(0, 0.95, -0.1);
        } else if (isParticles) {
            const particleCount = 480;
            const positions = new Float32Array(particleCount * 3);
            const colors = new Float32Array(particleCount * 3);
            particleMeta = [];
            for (let i = 0; i < particleCount; ++i) {
                const ring = 0.55 + (i % 24) * 0.055;
                const angle = (i / particleCount) * Math.PI * 16.0;
                const y = ((i % 20) - 10) * 0.085;
                particleMeta.push({
                    radius: ring,
                    angle,
                    y,
                    phase: (i % 37) * 0.17,
                    band: i % 24
                });
                const idx = i * 3;
                positions[idx + 0] = Math.cos(angle) * ring;
                positions[idx + 1] = y;
                positions[idx + 2] = Math.sin(angle) * ring * 0.7;
                colors[idx + 0] = 0.2;
                colors[idx + 1] = 0.75;
                colors[idx + 2] = 1.0;
            }
            const particleGeometry = new THREE.BufferGeometry();
            particleGeometry.setAttribute('position', new THREE.BufferAttribute(positions, 3));
            particleGeometry.setAttribute('color', new THREE.BufferAttribute(colors, 3));
            const particleMaterial = new THREE.PointsMaterial({
                size: 0.12,
                vertexColors: true,
                sizeAttenuation: true
            });
            particleCloud = new THREE.Points(particleGeometry, particleMaterial);
            particlePositions = particleGeometry.getAttribute('position');
            particleColors = particleGeometry.getAttribute('color');
            scene.add(particleCloud);

            particleCore = new THREE.Mesh(
                new THREE.SphereGeometry(0.3, 20, 16),
                new THREE.MeshBasicMaterial({ color: 0x7dd3fc })
            );
            particleCore.position.z = -0.4;
            scene.add(particleCore);

            interactionTarget = particleCloud;
            controls.target.set(0, 0.4, 0);
        } else if (isRibbon) {
            ribbonSegments = 72;
            ribbonTubePoints = Array.from({ length: ribbonSegments + 1 }, () => new THREE.Vector3());
            const ribbonFrame = typeof __readSpectrumFrame__ === 'function' ? __readSpectrumFrame__() : { bars: [], rms: 0, beat: 0, time: 0 };
            const ribbonFrameBars = Array.isArray(ribbonFrame.bars) ? ribbonFrame.bars : [];
            const ribbonFrameRms = Math.max(0.0, Math.min(1.0, Number(ribbonFrame.rms || 0.0)));
            const ribbonFrameBeat = Math.max(0.0, Math.min(1.0, Number(ribbonFrame.beat || 0.0)));
            const ribbonFrameTime = Number(ribbonFrame.time || 0.0);
            ribbonSurface = new THREE.Group();
            ribbonSurface.position.y = 0.1;
            ribbonSurface.rotation.x = -0.02;
            ribbonSurface.rotation.y = 0.0;
            scene.add(ribbonSurface);

            const ribbonBackdrop = new THREE.Mesh(
                new THREE.PlaneGeometry(10.5, 5.4),
                new THREE.MeshBasicMaterial({ color: 0x071224, transparent: true, opacity: 0.06 })
            );
            ribbonBackdrop.position.z = -1.8;
            ribbonBackdrop.position.y = 0.02;
            ribbonBackdrop.renderOrder = 1;
            ribbonSurface.add(ribbonBackdrop);

            const ribbonNodeGeometry = new THREE.SphereGeometry(0.09, 12, 8);
            for (let i = 0; i < 24; ++i) {
                const node = new THREE.Mesh(
                    ribbonNodeGeometry,
                    new THREE.MeshBasicMaterial({ color: 0xfef08a, depthTest: false, depthWrite: false })
                );
                node.renderOrder = 7;
                ribbonNodes.push(node);
                ribbonSurface.add(node);
            }

            for (let i = 0; i < ribbonTubePoints.length; ++i) {
                const t = ribbonTubePoints.length > 1 ? i / (ribbonTubePoints.length - 1) : 0.0;
                const band = Math.max(0.05, Math.min(1.0, Number(ribbonFrameBars[Math.min(23, Math.floor(t * 24.0))] || 0.0)));
                const x = (t - 0.5) * 8.8;
                const center = Math.sin(ribbonFrameTime * 1.15 + t * 8.0) * (0.14 + band * 0.34)
                    + (band - 0.5) * 0.52
                    + ribbonFrameBeat * 0.28;
                const wave = Math.cos(ribbonFrameTime * 1.9 + t * 6.2) * (0.2 + band * 0.18);
                const lift = Math.sin(ribbonFrameTime * 1.55 + t * 4.0) * (0.16 + ribbonFrameBeat * 0.24);
                ribbonTubePoints[i].set(
                    x,
                    center + Math.sin(t * 3.14159265358979323846) * (0.12 + ribbonFrameRms * 0.18),
                    wave + lift
                );
            }
            for (let i = 0; i < ribbonNodes.length; ++i) {
                const node = ribbonNodes[i];
                const t = ribbonNodes.length > 1 ? i / (ribbonNodes.length - 1) : 0.0;
                const pointIndex = Math.min(ribbonTubePoints.length - 1, Math.floor(t * (ribbonTubePoints.length - 1)));
                const p = ribbonTubePoints[pointIndex];
                node.position.set(p.x, p.y, p.z);
                node.scale.setScalar(0.75 + ribbonFrameBeat * 0.5 + ribbonFrameRms * 0.2);
                node.material.color.setHSL(0.16 - t * 0.06, 1.0, 0.78 + ribbonFrameRms * 0.08);
            }
            const ribbonTubeGeometry = new THREE.TubeGeometry(
                new THREE.CatmullRomCurve3(ribbonTubePoints, false, 'catmullrom', 0.5),
                180,
                0.14 + ribbonFrameRms * 0.03,
                8,
                false
            );
            const ribbonTubeMaterial = new THREE.MeshBasicMaterial({ color: 0x76ff7a, depthTest: false, depthWrite: false });
            ribbonTube = new THREE.Mesh(ribbonTubeGeometry, ribbonTubeMaterial);
            ribbonTube.renderOrder = 3;
            ribbonSurface.add(ribbonTube);

            const ribbonOverlayGeometry = new THREE.BufferGeometry().setFromPoints(ribbonTubePoints);
            ribbonLine = new THREE.Line(
                ribbonOverlayGeometry,
                new THREE.LineBasicMaterial({ color: 0xf8ff72, depthTest: false, depthWrite: false })
            );
            ribbonLine.renderOrder = 4;
            ribbonSurface.add(ribbonLine);

            const ribbonCore = new THREE.Mesh(
                new THREE.SphereGeometry(0.22, 20, 16),
                new THREE.MeshBasicMaterial({ color: 0xfef08a, depthTest: false, depthWrite: false })
            );
            ribbonCore.position.z = -0.24;
            ribbonCore.scale.setScalar(1.0);
            ribbonCore.renderOrder = 5;
            ribbonSurface.add(ribbonCore);

            ribbonBarsGroup = new THREE.Group();
            ribbonBarsGroup.position.y = 0.12;
            ribbonBarsGroup.position.z = 0.42;
            ribbonBarsGroup.rotation.x = -0.18;
            ribbonBarsGroup.rotation.y = 0.0;
            const ribbonBarGeometry = new THREE.BoxGeometry(0.16, 1.0, 0.24);
            for (let i = 0; i < 72; ++i) {
                const bar = new THREE.Mesh(ribbonBarGeometry, new THREE.MeshBasicMaterial({ color: 0x7dd3fc, depthTest: false, depthWrite: false }));
                bar.renderOrder = 6;
                bar.position.x = (i - 35.5) * 0.16;
                bar.position.y = 0.0;
                bar.position.z = 0.0;
                bar.scale.x = 1.6;
                bar.scale.y = 0.22;
                bar.scale.z = 1.2;
                ribbonBars.push(bar);
                ribbonBarsGroup.add(bar);
            }
            ribbonSurface.add(ribbonBarsGroup);

            interactionTarget = ribbonSurface;
            controls.target.set(0, 0.08, 0);
        } else if (isReverb) {
            const frame = typeof __readSpectrumFrame__ === 'function' ? __readSpectrumFrame__() : {
                waveform: [],
                rms: 0,
                beat: 0,
                time: 0,
                roomWidth: 6.2,
                roomDepth: 5.2,
                roomHeight: 3.0,
                absorption: 0.3,
                sourceX: 0.0,
                sourceY: 0.55,
                sourceZ: -0.9,
                listenerX: -0.55,
                listenerY: 0.42,
                listenerZ: 1.0
            };
            const waveform = Array.isArray(frame.waveform) ? frame.waveform : [];
            const roomWidth = Math.max(4.2, Number(frame.roomWidth || 6.2));
            const roomDepth = Math.max(3.8, Number(frame.roomDepth || 5.2));
            const roomHeight = Math.max(2.4, Number(frame.roomHeight || 3.0));
            reverbRoomWidth = roomWidth;
            reverbRoomDepth = roomDepth;
            reverbRoomHeight = roomHeight;
            reverbAbsorption = Math.max(0.0, Math.min(1.0, Number(frame.absorption || 0.3)));
            reverbRoomGroup = new THREE.Group();
            reverbRoomGroup.position.y = 0.02;
            scene.add(reverbRoomGroup);

            reverbRoomFill = new THREE.Mesh(
                new THREE.BoxGeometry(1, 1, 1),
                new THREE.MeshBasicMaterial({ color: 0x2563eb, transparent: true, opacity: 0.3, depthTest: false, depthWrite: false })
            );
            reverbRoomFill.scale.set(roomWidth, roomHeight, roomDepth);
            reverbRoomFill.position.y = roomHeight * 0.5;
            reverbRoomFill.renderOrder = 0;
            reverbRoomGroup.add(reverbRoomFill);

            reverbRoomShell = new THREE.Mesh(
                new THREE.BoxGeometry(1, 1, 1),
                new THREE.MeshBasicMaterial({ color: 0x0f172a, wireframe: true, transparent: true, opacity: 0.75 })
            );
            reverbRoomShell.scale.set(roomWidth, roomHeight, roomDepth);
            reverbRoomShell.position.y = roomHeight * 0.5;
            reverbRoomShell.renderOrder = 1;
            reverbRoomGroup.add(reverbRoomShell);

            reverbRoomEdges = new THREE.LineSegments(
                new THREE.EdgesGeometry(new THREE.BoxGeometry(1, 1, 1)),
                new THREE.LineBasicMaterial({ color: 0x8be9ff, transparent: true, opacity: 0.98, depthTest: false, depthWrite: false })
            );
            reverbRoomEdges.scale.set(roomWidth, roomHeight, roomDepth);
            reverbRoomEdges.position.y = roomHeight * 0.5;
            reverbRoomEdges.renderOrder = 2;
            reverbRoomGroup.add(reverbRoomEdges);

            reverbSource = new THREE.Mesh(
                new THREE.SphereGeometry(0.11, 16, 12),
                new THREE.MeshBasicMaterial({ color: 0xfb7185, depthTest: false, depthWrite: false })
            );
            reverbSource.position.set(Number(frame.sourceX || 0), Number(frame.sourceY || 0.55), Number(frame.sourceZ || -0.9));
            reverbRoomGroup.add(reverbSource);

            reverbListener = new THREE.Mesh(
                new THREE.SphereGeometry(0.13, 16, 12),
                new THREE.MeshBasicMaterial({ color: 0x7dd3fc, depthTest: false, depthWrite: false })
            );
            reverbListener.position.set(Number(frame.listenerX || -0.55), Number(frame.listenerY || 0.42), Number(frame.listenerZ || 1.0));
            reverbRoomGroup.add(reverbListener);

            const reflectionPalette = [0xfde047, 0x7dd3fc, 0xf8fafc, 0xa3e635, 0xf472b6, 0x60a5fa];
            for (let i = 0; i < 6; ++i) {
                const geometry = new THREE.BufferGeometry();
                geometry.setAttribute('position', new THREE.BufferAttribute(new Float32Array(9), 3));
                const line = new THREE.Line(
                    geometry,
                    new THREE.LineBasicMaterial({
                        color: reflectionPalette[i % reflectionPalette.length],
                        transparent: true,
                        opacity: 0.92,
                        depthTest: false,
                        depthWrite: false
                    })
                );
                line.renderOrder = 4;
                reverbReflectionLines.push(line);
                reverbRoomGroup.add(line);
            }

            const cloudCount = 224;
            const cloudPositions = new Float32Array(cloudCount * 3);
            const cloudColors = new Float32Array(cloudCount * 3);
            reverbCloudMeta = [];
            for (let i = 0; i < cloudCount; ++i) {
                reverbCloudMeta.push({
                    radius: 0.35 + (i % 14) * 0.06,
                    angle: (i / cloudCount) * Math.PI * 14.0,
                    band: i % 24,
                    phase: (i % 19) * 0.13
                });
                const idx = i * 3;
                cloudPositions[idx + 0] = Math.cos(i * 0.31) * 0.4;
                cloudPositions[idx + 1] = 0.35 + Math.sin(i * 0.17) * 0.2;
                cloudPositions[idx + 2] = Math.sin(i * 0.29) * 0.4;
                cloudColors[idx + 0] = 0.49;
                cloudColors[idx + 1] = 0.75;
                cloudColors[idx + 2] = 1.0;
            }
            const cloudGeometry = new THREE.BufferGeometry();
            cloudGeometry.setAttribute('position', new THREE.BufferAttribute(cloudPositions, 3));
            cloudGeometry.setAttribute('color', new THREE.BufferAttribute(cloudColors, 3));
            reverbCloud = new THREE.Points(
                cloudGeometry,
                new THREE.PointsMaterial({
                    size: 0.08,
                    vertexColors: true,
                    transparent: true,
                    opacity: 0.66,
                    sizeAttenuation: true,
                    depthTest: false,
                    depthWrite: false
                })
            );
            reverbCloud.renderOrder = 3;
            reverbCloudPositions = cloudGeometry.getAttribute('position');
            reverbCloudColors = cloudGeometry.getAttribute('color');
            reverbRoomGroup.add(reverbCloud);

            reverbWaveformPeak = waveform.reduce((peak, value) => Math.max(peak, Math.abs(Number(value || 0.0))), 0.0);
            interactionTarget = reverbRoomGroup;
            controls.target.set(0, 0.55, 0);
        } else {
            const geometry = new THREE.BoxGeometry(0.75, 0.75, 0.75);
            const material = new THREE.MeshBasicMaterial({ color: 0x00ff00 });
            mesh = new THREE.Mesh(geometry, material);
            mesh.rotation.x = 0.4;
            mesh.rotation.y = 0.6;
            scene.add(mesh);
            interactionTarget = mesh;
            globalThis.__pulpThreeDemoMesh = mesh;
            controls.target.set(0, 0, 0);
        }

        controls.update();

        let dragging = false;
        let dragPointerId = 0;
        let lastX = 0;
        let lastY = 0;
        let lastRenderMs = 0;
        let zoomEvents = 0;
        let pinchEvents = 0;
        function syncHud(frameValueNumber) {
            backendValue.textContent =
                renderer.backend && renderer.backend.constructor ? renderer.backend.constructor.name : 'unknown';
            frameValue.textContent = String(frameValueNumber);
            cameraValue.textContent = camera.position.z.toFixed(2);
            modeValue.textContent = isSpectrum ? 'spectrum-analyzer' : (isParticles ? 'particle-visualizer' : (isRibbon ? 'waveform-ribbon' : (isReverb ? 'room-reverb' : 'cube-smoke')));
            interactionValue.textContent = isSpectrum
                ? spectrumPeak.toFixed(2)
                : ((isParticles || isRibbon)
                    ? `${signalRms.toFixed(2)} / ${signalBeat.toFixed(2)}`
                    : (isReverb
                        ? `${reverbWaveformPeak.toFixed(2)} / ${reverbAbsorption.toFixed(2)}`
                        : (dragging ? 'dragging cube' : 'drag / pinch / wheel')));
        }
        function applyZoomDelta(delta) {
            const nextZ = camera.position.z + delta;
            camera.position.z = Math.max(1.1, Math.min(6.2, nextZ));
            globalThis.__pulpThreeDemoState.cameraZ = camera.position.z;
            cameraValue.textContent = camera.position.z.toFixed(2);
        }
        function updateSpectrumScene() {
            if (!isSpectrum) return;
            const frameData = typeof __readSpectrumFrame__ === 'function' ? __readSpectrumFrame__() : null;
            const bars = frameData && Array.isArray(frameData.bars) ? frameData.bars : [];
            spectrumPeak = 0;
            for (let i = 0; i < spectrumBars.length; ++i) {
                const bar = spectrumBars[i];
                const magnitude = Math.max(0.05, Math.min(1.0, Number(bars[i] || 0)));
                const height = 0.18 + magnitude * 2.9;
                bar.scale.y = height;
                bar.position.y = height * 0.5;
                bar.material.color.setHSL(0.58 - magnitude * 0.34, 0.88, 0.58);
                spectrumPeak = Math.max(spectrumPeak, magnitude);
            }
            if (peakBar) {
                const peakHeight = 0.28 + spectrumPeak * 3.8;
                peakBar.scale.y = peakHeight;
                peakBar.position.y = peakHeight * 0.5;
                peakBar.material.color.setHSL(0.18 + spectrumPeak * 0.18, 0.85, 0.58);
            }
            if (interactionTarget) {
                interactionTarget.rotation.y += 0.004;
            }
            globalThis.__pulpThreeDemoState.spectrumPeak = spectrumPeak;
        }
        function updateParticleScene() {
            if (!isParticles) return;
            const frameData = typeof __readSpectrumFrame__ === 'function' ? __readSpectrumFrame__() : null;
            const bars = frameData && Array.isArray(frameData.bars) ? frameData.bars : [];
            signalRms = Math.max(0.0, Math.min(1.0, Number(frameData && frameData.rms || 0.0)));
            signalBeat = Math.max(0.0, Math.min(1.0, Number(frameData && frameData.beat || 0.0)));
            const time = Number(frameData && frameData.time || 0.0);
            if (particlePositions && particleColors) {
                for (let i = 0; i < particleMeta.length; ++i) {
                    const meta = particleMeta[i];
                    const band = Math.max(0.05, Math.min(1.0, Number(bars[meta.band] || 0.0)));
                    const idx = i * 3;
                    const radius = meta.radius * (0.72 + signalRms * 0.45 + band * 0.2);
                    const swirl = meta.angle + time * (0.55 + band * 0.65) + meta.phase;
                    particlePositions.array[idx + 0] = Math.cos(swirl) * radius;
                    particlePositions.array[idx + 1] = meta.y + Math.sin(time * 2.2 + meta.phase) * (0.08 + band * 0.28) + signalBeat * 0.18;
                    particlePositions.array[idx + 2] = Math.sin(swirl) * radius * (0.52 + signalBeat * 0.38);
                    particleColor.setHSL(0.58 - band * 0.32 + signalBeat * 0.05, 0.88, 0.52 + signalRms * 0.14);
                    particleColors.array[idx + 0] = particleColor.r;
                    particleColors.array[idx + 1] = particleColor.g;
                    particleColors.array[idx + 2] = particleColor.b;
                }
                particlePositions.needsUpdate = true;
                particleColors.needsUpdate = true;
            }
            if (particleCloud) {
                particleCloud.rotation.y += 0.003 + signalBeat * 0.015;
                particleCloud.rotation.x = -0.12 + signalRms * 0.1;
            }
            if (particleCore) {
                const coreScale = 0.85 + signalBeat * 1.35;
                particleCore.scale.setScalar(coreScale);
                particleCore.material.color.setHSL(0.54 - signalBeat * 0.22, 0.92, 0.62 + signalRms * 0.12);
            }
            globalThis.__pulpThreeDemoState.signalRms = signalRms;
            globalThis.__pulpThreeDemoState.signalBeat = signalBeat;
        }
        function updateRibbonScene() {
            if (!isRibbon) return;
            const frameData = typeof __readSpectrumFrame__ === 'function' ? __readSpectrumFrame__() : null;
            const bars = frameData && Array.isArray(frameData.bars) ? frameData.bars : [];
            signalRms = Math.max(0.0, Math.min(1.0, Number(frameData && frameData.rms || 0.0)));
            signalBeat = Math.max(0.0, Math.min(1.0, Number(frameData && frameData.beat || 0.0)));
            const time = Number(frameData && frameData.time || 0.0);
            if (ribbonSurface) {
            ribbonSurface.rotation.y += 0.002 + signalBeat * 0.01;
            ribbonSurface.rotation.x = -0.04 + signalRms * 0.06;
            }
            if (ribbonTubePoints && ribbonSurface) {
                const pointCount = ribbonTubePoints.length;
                for (let i = 0; i < pointCount; ++i) {
                    const t = pointCount > 1 ? i / (pointCount - 1) : 0.0;
                    const band = Math.max(0.05, Math.min(1.0, Number(bars[Math.min(23, Math.floor(t * 24.0))] || 0.0)));
                    const x = (t - 0.5) * 8.8;
                    const center = Math.sin(time * 1.15 + t * 8.0) * (0.14 + band * 0.34)
                        + (band - 0.5) * 0.52
                        + signalBeat * 0.28;
                    const wave = Math.cos(time * 1.9 + t * 6.2) * (0.2 + band * 0.18);
                    const lift = Math.sin(time * 1.55 + t * 4.0) * (0.16 + signalBeat * 0.24);
                    ribbonTubePoints[i].set(
                        x,
                        center + Math.sin(t * 3.14159265358979323846) * (0.12 + signalRms * 0.18),
                        wave + lift
                    );
                }
                for (let i = 0; i < ribbonNodes.length; ++i) {
                    const node = ribbonNodes[i];
                    const t = ribbonNodes.length > 1 ? i / (ribbonNodes.length - 1) : 0.0;
                    const pointIndex = Math.min(pointCount - 1, Math.floor(t * (pointCount - 1)));
                    const p = ribbonTubePoints[pointIndex];
                    node.position.set(p.x, p.y, p.z);
                    node.scale.setScalar(0.75 + signalBeat * 0.5 + signalRms * 0.2);
                    node.material.color.setHSL(0.16 - t * 0.06, 1.0, 0.78 + signalRms * 0.08);
                }
                const ribbonCurve = new THREE.CatmullRomCurve3(ribbonTubePoints, false, 'catmullrom', 0.5);
                if (ribbonTube) {
                    const nextGeometry = new THREE.TubeGeometry(
                        ribbonCurve,
                        180,
                        0.14 + signalRms * 0.03,
                        8,
                        false
                    );
                    ribbonTube.geometry.dispose();
                    ribbonTube.geometry = nextGeometry;
                    ribbonTube.material.color.setHSL(0.28 - signalBeat * 0.12, 0.95, 0.68 + signalRms * 0.12);
                }
                if (ribbonLine) {
                    ribbonLine.geometry.dispose();
                    ribbonLine.geometry = new THREE.BufferGeometry().setFromPoints(ribbonTubePoints);
                    ribbonLine.material.color.setHSL(0.16 - signalBeat * 0.08, 1.0, 0.74);
                }
            }
            if (ribbonBarsGroup) {
                ribbonBarsGroup.rotation.y += 0.002 + signalBeat * 0.01;
                ribbonBarsGroup.rotation.x = -0.18 + signalRms * 0.04;
                for (let i = 0; i < ribbonBars.length; ++i) {
                    const bar = ribbonBars[i];
                    const t = ribbonBars.length > 1 ? i / (ribbonBars.length - 1) : 0.0;
                    const band = Math.max(0.05, Math.min(1.0, Number(bars[Math.min(23, Math.floor(t * 24.0))] || 0.0)));
                    const height = 0.32 + signalRms * 1.0 + band * 2.2;
                    bar.scale.y = height;
                    bar.position.y = height * 0.5 - 0.12 + Math.sin(time * 1.7 + t * 4.0) * (0.06 + signalBeat * 0.12);
                    bar.position.z = 0.36 + Math.sin(time * 1.5 + t * 5.0) * (0.08 + signalBeat * 0.16);
                    bar.material.color.setHSL(0.24 - band * 0.12 + signalBeat * 0.04, 1.0, 0.78 + signalRms * 0.08);
                }
            }
            globalThis.__pulpThreeDemoState.signalRms = signalRms;
            globalThis.__pulpThreeDemoState.signalBeat = signalBeat;
        }
        function updateReverbScene() {
            if (!isReverb) return;
            const frameData = typeof __readSpectrumFrame__ === 'function' ? __readSpectrumFrame__() : null;
            const waveform = frameData && Array.isArray(frameData.waveform) ? frameData.waveform : [];
            signalRms = Math.max(0.0, Math.min(1.0, Number(frameData && frameData.rms || 0.0)));
            signalBeat = Math.max(0.0, Math.min(1.0, Number(frameData && frameData.beat || 0.0)));
            const time = Number(frameData && frameData.time || 0.0);
            const roomWidth = Math.max(4.2, Number(frameData && frameData.roomWidth || 6.2));
            const roomDepth = Math.max(3.8, Number(frameData && frameData.roomDepth || 5.2));
            const roomHeight = Math.max(2.4, Number(frameData && frameData.roomHeight || 3.0));
            reverbAbsorption = Math.max(0.0, Math.min(1.0, Number(frameData && frameData.absorption || 0.3)));
            reverbRoomWidth = roomWidth;
            reverbRoomDepth = roomDepth;
            reverbRoomHeight = roomHeight;
            const source = new THREE.Vector3(
                Number(frameData && frameData.sourceX || 0.0),
                Math.max(0.18, Number(frameData && frameData.sourceY || 0.55)),
                Number(frameData && frameData.sourceZ || -0.9)
            );
            const listener = new THREE.Vector3(
                Number(frameData && frameData.listenerX || -0.55),
                Math.max(0.16, Number(frameData && frameData.listenerY || 0.42)),
                Number(frameData && frameData.listenerZ || 1.0)
            );
            reverbWaveformPeak = waveform.reduce((peak, value) => Math.max(peak, Math.abs(Number(value || 0.0))), 0.0);
            if (reverbRoomShell) {
                reverbRoomShell.scale.set(roomWidth, roomHeight, roomDepth);
                reverbRoomShell.position.y = roomHeight * 0.5;
                reverbRoomShell.material.opacity = Math.max(0.25, 0.82 - reverbAbsorption * 0.55);
            }
            if (reverbRoomFill) {
                reverbRoomFill.scale.set(roomWidth, roomHeight, roomDepth);
                reverbRoomFill.position.y = roomHeight * 0.5;
                reverbRoomFill.material.opacity = Math.max(0.1, 0.2 - reverbAbsorption * 0.08);
            }
            if (reverbRoomEdges) {
                reverbRoomEdges.scale.set(roomWidth, roomHeight, roomDepth);
                reverbRoomEdges.position.y = roomHeight * 0.5;
                reverbRoomEdges.material.opacity = Math.max(0.58, 0.96 - reverbAbsorption * 0.2);
            }
            if (reverbSource) {
                reverbSource.position.copy(source);
            }
            if (reverbListener) {
                reverbListener.position.copy(listener);
            }
            if (reverbReflectionLines.length > 0) {
                const walls = [
                    { axis: 'x', value: -roomWidth * 0.5 },
                    { axis: 'x', value: roomWidth * 0.5 },
                    { axis: 'y', value: 0.0 },
                    { axis: 'z', value: -roomDepth * 0.5 },
                    { axis: 'z', value: roomDepth * 0.5 },
                    { axis: 'y', value: roomHeight }
                ];
                for (let i = 0; i < reverbReflectionLines.length; ++i) {
                    const line = reverbReflectionLines[i];
                    const wall = walls[i % walls.length];
                    const midpoint = new THREE.Vector3(
                        (source.x + listener.x) * 0.5,
                        (source.y + listener.y) * 0.5,
                        (source.z + listener.z) * 0.5
                    );
                    if (wall.axis === 'x') {
                        midpoint.x = wall.value;
                        midpoint.y += Math.sin(time * 1.2 + i) * (0.08 + signalBeat * 0.12);
                        midpoint.z += Math.cos(time * 1.5 + i) * (0.06 + signalRms * 0.12);
                    } else if (wall.axis === 'y') {
                        midpoint.y = wall.value;
                        midpoint.x += Math.sin(time * 0.9 + i) * (0.1 + signalBeat * 0.1);
                        midpoint.z += Math.cos(time * 1.1 + i) * (0.08 + signalRms * 0.08);
                    } else {
                        midpoint.z = wall.value;
                        midpoint.x += Math.sin(time * 1.0 + i) * (0.08 + signalBeat * 0.1);
                        midpoint.y += Math.cos(time * 1.2 + i) * (0.06 + signalRms * 0.1);
                    }
                    const positions = line.geometry.getAttribute('position');
                    positions.setXYZ(0, source.x, source.y, source.z);
                    positions.setXYZ(1, midpoint.x, midpoint.y, midpoint.z);
                    positions.setXYZ(2, listener.x, listener.y, listener.z);
                    positions.needsUpdate = true;
                    line.material.color.setHSL(0.15 + 0.07 * i + signalBeat * 0.04, 1.0, 0.66 + signalRms * 0.08);
                    line.material.opacity = Math.max(0.42, 0.82 - reverbAbsorption * 0.28);
                }
            }
            if (reverbCloud && reverbCloudPositions && reverbCloudColors && reverbCloudMeta.length > 0) {
                const baseRadius = 0.38 + signalRms * 0.72 + reverbWaveformPeak * 0.3;
                for (let i = 0; i < reverbCloudMeta.length; ++i) {
                    const meta = reverbCloudMeta[i];
                    const idx = i * 3;
                    const sample = waveform.length > 0 ? Number(waveform[i % waveform.length] || 0.0) : 0.0;
                    const energy = Math.min(1.0, Math.abs(sample) * 1.6 + signalBeat * 0.3);
                    const swirl = meta.angle + time * (0.45 + meta.phase * 0.05) + sample * 0.8;
                    const radius = baseRadius + meta.radius * (0.45 + energy * 0.8);
                    reverbCloudPositions.array[idx + 0] = listener.x + Math.cos(swirl) * radius;
                    reverbCloudPositions.array[idx + 1] = listener.y + Math.sin(time * 2.1 + meta.phase) * (0.1 + energy * 0.22) + sample * 0.12;
                    reverbCloudPositions.array[idx + 2] = listener.z + Math.sin(swirl) * radius * (0.52 + signalRms * 0.18);
                    particleColor.setHSL(0.58 - energy * 0.18, 0.9, 0.55 + energy * 0.22);
                    reverbCloudColors.array[idx + 0] = particleColor.r;
                    reverbCloudColors.array[idx + 1] = particleColor.g;
                    reverbCloudColors.array[idx + 2] = particleColor.b;
                }
                reverbCloudPositions.needsUpdate = true;
                reverbCloudColors.needsUpdate = true;
                reverbCloud.rotation.y += 0.002 + signalBeat * 0.01;
                reverbCloud.rotation.x = -0.1 + signalRms * 0.08;
            }
            if (reverbRoomGroup) {
                reverbRoomGroup.rotation.y += 0.0018 + signalBeat * 0.007;
                reverbRoomGroup.rotation.x = -0.03 + signalRms * 0.04;
            }
            globalThis.__pulpThreeDemoState.signalRms = signalRms;
            globalThis.__pulpThreeDemoState.signalBeat = signalBeat;
            globalThis.__pulpThreeDemoState.roomWidth = roomWidth;
            globalThis.__pulpThreeDemoState.roomDepth = roomDepth;
            globalThis.__pulpThreeDemoState.roomHeight = roomHeight;
            globalThis.__pulpThreeDemoState.absorption = reverbAbsorption;
            globalThis.__pulpThreeDemoState.waveformPeak = reverbWaveformPeak;
        }
        canvas.style.cursor = 'grab';
        canvas.addEventListener('pointerdown', (event) => {
            dragging = true;
            dragPointerId = event.pointerId || 0;
            lastX = event.clientX || event.offsetX || 0;
            lastY = event.clientY || event.offsetY || 0;
            canvas.style.cursor = 'grabbing';
            if (typeof canvas.setPointerCapture === 'function') {
                canvas.setPointerCapture(dragPointerId);
            }
            globalThis.__pulpThreeDemoState.dragging = true;
        });
        canvas.addEventListener('pointermove', (event) => {
            if (!dragging || !interactionTarget) return;
            const nextX = event.clientX || event.offsetX || 0;
            const nextY = event.clientY || event.offsetY || 0;
            const dx = nextX - lastX;
            const dy = nextY - lastY;
            lastX = nextX;
            lastY = nextY;
            if (isSpectrum || isParticles || isRibbon) {
                interactionTarget.rotation.y += dx * 0.01;
                interactionTarget.rotation.x = Math.max(-0.6, Math.min(0.8, interactionTarget.rotation.x + dy * 0.003));
            } else {
                interactionTarget.rotation.y += dx * 0.01;
                interactionTarget.rotation.x += dy * 0.01;
            }
            globalThis.__pulpThreeDemoState.dragEvents =
                (globalThis.__pulpThreeDemoState.dragEvents || 0) + 1;
        });
        function endDrag(event) {
            const pointerId = event && event.pointerId != null ? event.pointerId : dragPointerId;
            dragging = false;
            globalThis.__pulpThreeDemoState.dragging = false;
            canvas.style.cursor = 'grab';
            if (typeof canvas.releasePointerCapture === 'function') {
                canvas.releasePointerCapture(pointerId);
            }
        }
        canvas.addEventListener('pointerup', endDrag);
        canvas.addEventListener('pointercancel', endDrag);
        canvas.addEventListener('wheel', (event) => {
            const deltaY = event && event.deltaY != null ? event.deltaY : 0;
            applyZoomDelta(deltaY * 0.0025);
            zoomEvents += 1;
            globalThis.__pulpThreeDemoState.zoomEvents = zoomEvents;
            if (event && typeof event.preventDefault === 'function') {
                event.preventDefault();
            }
        });
        canvas.addEventListener('gesturechange', (event) => {
            const scale = event && event.scale != null ? event.scale : 1;
            if (!Number.isFinite(scale) || scale === 1) return;
            applyZoomDelta((1 - scale) * 0.9);
            pinchEvents += 1;
            globalThis.__pulpThreeDemoState.pinchEvents = pinchEvents;
        });

        let frame = 0;
        function tick() {
            const now = (typeof performance !== 'undefined' && typeof performance.now === 'function')
                ? performance.now()
                : 0;
            const shouldRender = dragging || frame === 0 || (now - lastRenderMs) >= 33;
            if (shouldRender) {
                if (isSpectrum) {
                    updateSpectrumScene();
                } else if (isParticles) {
                    updateParticleScene();
                } else if (isRibbon) {
                    updateRibbonScene();
                } else if (isReverb) {
                    updateReverbScene();
                } else if (mesh) {
                    mesh.rotation.x += 0.02;
                    mesh.rotation.y += 0.03;
                }
                controls.update();
                renderer.render(scene, camera);
                if (typeof context.present === 'function') {
                    context.present();
                }
                lastRenderMs = now;
                globalThis.__pulpThreeDemoState.frame = frame;
                syncHud(frame);
                frame += 1;
            }
            requestAnimationFrame(tick);
        }

        if (isSpectrum) {
            updateSpectrumScene();
        } else if (isParticles) {
            updateParticleScene();
        } else if (isRibbon) {
            updateRibbonScene();
        } else if (isReverb) {
            updateReverbScene();
        }
        renderer.render(scene, camera);
        if (typeof context.present === 'function') {
            context.present();
        }
        requestAnimationFrame(tick);

        globalThis.__pulpThreeDemoState = {
            status: 'ready',
            step: 'animating',
            layout: 'hybrid-2d-3d',
            demo: demoMode,
            backend: renderer.backend && renderer.backend.constructor ? renderer.backend.constructor.name : '',
            contextType: renderer.getContext() && renderer.getContext()._objectName ? renderer.getContext()._objectName : '',
            width: renderer.domElement.width || 0,
            height: renderer.domElement.height || 0,
            frame: 0,
            dragEvents: 0,
            dragging: false,
            zoomEvents: 0,
            pinchEvents: 0,
            cameraZ: camera.position.z,
            spectrumPeak: spectrumPeak,
            signalRms: signalRms,
            signalBeat: signalBeat,
            waveformPeak: reverbWaveformPeak,
            roomWidth: reverbRoomWidth,
            roomDepth: reverbRoomDepth,
            roomHeight: reverbRoomHeight,
            absorption: reverbAbsorption,
            titleText: title.textContent
        };
        syncHud(0);

        export default true;
    )JS";
    return js.str();
}
#endif // preserved inline version

void prime_demo_frames(DemoEnvironment& env, int frames) {
    if (!env.bridge) return;
    for (int i = 0; i < frames; ++i) {
        env.advance_sources(1.0f / 60.0f);
        env.bridge->service_frame_callbacks();
        env.engine.pump_message_loop();
    }
}

#ifdef PULP_BENCHMARK

// ── Zero-copy benchmark mode (Slice 0.5 of #516) ────────────────────────────
//
// Drives the *real* JS→GPU upload path: Three.js BufferGeometry +
// BufferAttribute(Float32Array, 3), with `needsUpdate = true` per
// frame so the vertex buffer is re-uploaded via queue.WriteBuffer.
// This exercises the upload code touched in #535 (the same code path
// a real JS-scripted plugin UI would use), not the C++-driven
// VisualizationBridge path. Headless: no WindowHost, offscreen Dawn
// surface only. Emits JSON with the Slice 0 schema plus the new
// Slice 0.5 fields (base64_decode_us, gpu_buffer_upload_count,
// gpu_buffer_bytes_resident_peak).
//
// MB-fraction is computed exactly as requested by the re-evaluation
// plan: (base64_decode_us + gpu_upload_us) / frame_budget_us.

struct ParticleBenchmarkConfig {
    bool enabled = false;
    int seconds = 10;
    std::string widget = "particles";
    std::string output_path;
    int target_fps = 60;
    int particle_count = 10000;
};

std::string current_iso8601_utc_threejs() {
    const auto now = std::chrono::system_clock::now();
    const auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm_utc{};
#if defined(_WIN32)
    gmtime_s(&tm_utc, &tt);
#else
    gmtime_r(&tt, &tm_utc);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
    return buf;
}

std::string current_short_sha_threejs() {
    // Codex 2026-04-21 review on #541: MSVC's CRT exposes the pipe
    // helpers as `_popen` / `_pclose`; bare `popen` / `pclose` are
    // POSIX spellings that don't link on Windows. Mirror the pattern
    // already used in tools/mcp/pulp_mcp.cpp so `PULP_BENCHMARK`-enabled
    // Windows builds don't fail to link.
#if defined(_WIN32)
    std::FILE* pipe = _popen("git rev-parse --short HEAD 2>NUL", "r");
#else
    std::FILE* pipe = ::popen("git rev-parse --short HEAD 2>/dev/null", "r");
#endif
    if (!pipe) return "unknown";
    char buf[64] = {0};
    std::string out;
    while (std::fgets(buf, sizeof(buf), pipe) != nullptr) {
        out += buf;
    }
#if defined(_WIN32)
    _pclose(pipe);
#else
    ::pclose(pipe);
#endif
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) {
        out.pop_back();
    }
    return out.empty() ? std::string("unknown") : out;
}

std::string current_host_short_threejs() {
    char buf[256] = {0};
    if (::gethostname(buf, sizeof(buf) - 1) == 0) {
        std::string nodename = buf;
        auto dot = nodename.find('.');
        if (dot != std::string::npos) nodename.resize(dot);
        return nodename;
    }
    return "unknown";
}

std::string current_platform_tag_threejs() {
#if defined(__APPLE__)
#if defined(__aarch64__)
    return "darwin-arm64";
#else
    return "darwin-x86_64";
#endif
#elif defined(_WIN32)
#if defined(_M_ARM64)
    return "windows-arm64";
#else
    return "windows-x86_64";
#endif
#elif defined(__linux__)
#if defined(__aarch64__)
    return "linux-arm64";
#else
    return "linux-x86_64";
#endif
#else
    return "unknown";
#endif
}

// Headless JS harness that exercises the same widget_bridge.cpp
// upload path Three.js's BufferGeometry.setAttribute drives on the
// render-pipeline lane. The payload shape mirrors what
// three.webgpu.js produces for a THREE.Points cloud: per-frame
// vertex buffer uploads sized `particle_count * 3 * sizeof(float)`
// bytes (positions) + same for colors.
//
// The __gpuQueueDrawBufferedImpl function expects a `vertexBuffers`
// array with a `data` field that's either a JSON array of bytes or
// a nested Uint8Array — we use the JSON-array form and emit it from
// a JS loop so the message-format side of the bridge is exercised
// too. This keeps the benchmark faithful to the "JS→GPU upload"
// path without depending on the external three.js module loader.
std::string make_particle_benchmark_harness() {
    return R"JS(
        globalThis.__pulpBenchState = { uploads: 0, frames: 0 };

        // Create the native canvas the render pipeline will target.
        const canvas = document.createElement('canvas');
        canvas.id = 'pulp-bench-particles-canvas';
        canvas.width = 512;
        canvas.height = 512;
        document.body.appendChild(canvas);

        // Configure the GPU canvas via the bridge. Without this the
        // __gpuQueueDrawBufferedImpl call rejects because the
        // target texture hasn't been created. Note: this function
        // takes positional args (canvasId, width, height, format,
        // usage, alphaMode), not a payload object.
        globalThis.__pulpBenchCanvasId = canvas._id;
        if (typeof __gpuCanvasConfigureImpl === 'function') {
            globalThis.__pulpBenchConfigResult = __gpuCanvasConfigureImpl(
                canvas._id, canvas.width, canvas.height,
                'bgra8unorm', 0, 'opaque');
        }

        // Trivial WGSL vertex + fragment shaders that consume a
        // position attribute. The GPU pipeline validates the layout
        // end-to-end — if the upload bytes are wrong, pipeline
        // creation fails and the harness caller will see it.
        globalThis.__pulpBenchVertexCode = `
            @vertex
            fn main(@location(0) pos : vec3<f32>) -> @builtin(position) vec4<f32> {
                return vec4<f32>(pos.x * 0.5, pos.y * 0.5, pos.z * 0.5, 1.0);
            }
        `;
        globalThis.__pulpBenchFragmentCode = `
            @fragment
            fn main() -> @location(0) vec4<f32> {
                return vec4<f32>(0.2, 0.8, 1.0, 1.0);
            }
        `;

        // Generate `count` vertex bytes once (as a plain array).
        // Simulates what THREE.BufferAttribute(Float32Array(count*3),3)
        // would produce after being serialized across the bridge.
        globalThis.__pulpBenchBuildVertexBytes = function(count) {
            const floatsPerVertex = 3;
            const bytes = new Array(count * floatsPerVertex * 4);
            for (let i = 0; i < count; ++i) {
                const angle = (i / count) * Math.PI * 2.0;
                const radius = 0.3 + 0.1 * Math.sin(i * 0.07);
                const px = Math.cos(angle) * radius;
                const py = Math.sin(angle) * radius;
                const pz = (i % 10) * 0.02 - 0.1;
                const vals = [px, py, pz];
                for (let v = 0; v < floatsPerVertex; ++v) {
                    const buf = new ArrayBuffer(4);
                    new Float32Array(buf)[0] = vals[v];
                    const u8 = new Uint8Array(buf);
                    const off = (i * floatsPerVertex + v) * 4;
                    bytes[off + 0] = u8[0];
                    bytes[off + 1] = u8[1];
                    bytes[off + 2] = u8[2];
                    bytes[off + 3] = u8[3];
                }
            }
            return bytes;
        };

        globalThis.__pulpBenchLastResult = null;
        globalThis.__pulpBenchRunFrame = function(count) {
            const bytes = __pulpBenchBuildVertexBytes(count);
            const ok = __gpuQueueDrawBufferedImpl({
                canvasId: __pulpBenchCanvasId,
                vertexCode: __pulpBenchVertexCode,
                vertexEntryPoint: 'main',
                fragmentCode: __pulpBenchFragmentCode,
                fragmentEntryPoint: 'main',
                format: 'bgra8unorm',
                topology: 'point-list',
                drawType: 'draw',
                loadOp: 'clear',
                storeOp: 'store',
                clearValue: { r: 0.02, g: 0.02, b: 0.05, a: 1.0 },
                vertexBuffers: [
                    {
                        slot: 0,
                        arrayStride: 12,
                        stepMode: 'vertex',
                        attributes: [
                            { shaderLocation: 0, offset: 0, format: 'float32x3' }
                        ],
                        data: bytes
                    }
                ],
                vertexCount: count
            });
            __pulpBenchLastResult = ok;
            __pulpBenchState.uploads += 1;
            __pulpBenchState.frames += 1;
            return ok;
        };

        void 0;
    )JS";
}

int run_particle_benchmark(const ParticleBenchmarkConfig& cfg) {
    using namespace pulp::view;

    std::cout << "Pulp zero-copy particle benchmark (Slice 0.5)\n"
              << "  widget:         " << cfg.widget << "\n"
              << "  particle count: " << cfg.particle_count << "\n"
              << "  seconds:        " << cfg.seconds << "\n"
              << "  target FPS:     " << cfg.target_fps << "\n"
              << "  output:         " << cfg.output_path << "\n";

    if (cfg.output_path.empty()) {
        std::cerr << "--benchmark-seconds requires --output=<path>\n";
        return 2;
    }

    // Codex 2026-04-21 review on #541: this harness is particle-only for
    // the Slice 0.5 re-eval. `--widget=<anything-else>` used to silently
    // run the particle workload while writing the user-supplied label
    // into the output JSON, which corrupts benchmark comparisons. Reject
    // any unsupported value up front so baselines stay honest.
    if (cfg.widget != "particles") {
        std::cerr << "Unsupported --widget=\"" << cfg.widget
                  << "\"; only 'particles' is implemented in this lane.\n"
                  << "Add a real harness before re-enabling. (Codex #541)\n";
        return 2;
    }

    if (!is_engine_available(JsEngineType::v8)) {
        std::cerr << "V8 is required for the native Three.js benchmark\n";
        return 1;
    }

    const int width = 820;
    const int height = 560;
    DemoEnvironment env(static_cast<float>(width), static_cast<float>(height));
    env.initialize_offscreen_gpu(static_cast<float>(width), static_cast<float>(height));
    if (!env.has_native_gpu()) {
        std::cerr << "Native Dawn adapter unavailable — benchmark requires GPU\n";
        return 1;
    }

    // Install the perf-counter sink on the bridge. All JS→GPU
    // uploads the harness issues will be timed.
    pulp::render::bench::PerfCounters counters;
    counters.reset();
    env.bridge->set_bench_counters(&counters);

    // Load the minimal JS harness that exercises the same
    // `__gpuQueueDrawBufferedImpl` vertex-buffer upload path Three.js
    // particles would hit. Using the harness directly (rather than
    // the full three.webgpu.js module loader) keeps the benchmark
    // independent of V8 top-level-await + module loader readiness
    // timing, which has proven flaky headless on this host. Per-upload
    // bytes match: particle_count * 3 floats (x,y,z) * 4 bytes.
    env.bridge->load_script(make_particle_benchmark_harness());

    // Give the harness a few microtask drains to let canvas config
    // and the bridge state get installed. Then reset counters so we
    // measure steady-state upload cost, not first-pipeline setup.
    for (int i = 0; i < 32; ++i) {
        env.engine.pump_message_loop();
        env.bridge->service_frame_callbacks();
    }
    // Warm-up frames: first draws allocate Dawn pipeline + shader
    // modules which would otherwise dominate the timing.
    const std::string run_frame_js =
        "__pulpBenchRunFrame(" + std::to_string(cfg.particle_count) + ");void 0";
    for (int i = 0; i < 8; ++i) {
        env.engine.evaluate(run_frame_js);
        env.engine.pump_message_loop();
    }

    // Sanity: if the harness is misconfigured, fail fast rather than
    // silently emitting zero counters.
    {
        auto probe = env.engine.evaluate(
            "String(__pulpBenchLastResult)");
        const auto last = std::string(probe.getWithDefault<std::string_view>("?"));
        if (last != "true") {
            std::cerr << "Benchmark harm-up upload returned '" << last
                      << "' — native bridge rejected the draw payload.\n"
                      << "Build was likely configured without PULP_HAS_SKIA "
                      << "(check CMake: SKIA_DIR must point to a complete Skia tree).\n";
            return 1;
        }
    }
    counters.reset();

    const int total_frames = cfg.target_fps * cfg.seconds;
    const double frame_budget_us = 1.0e6 / static_cast<double>(cfg.target_fps);

    for (int f = 0; f < total_frames; ++f) {
        const double frame_t0 = pulp::render::bench::now_us();

        env.engine.evaluate(run_frame_js);
        env.engine.pump_message_loop();

        const double frame_dt = pulp::render::bench::now_us() - frame_t0;
        counters.total_frame_total_us.fetch_add(
            frame_dt, std::memory_order_relaxed);
        counters.sample_count.fetch_add(1.0, std::memory_order_relaxed);

        const double remaining_us = frame_budget_us - frame_dt;
        if (remaining_us > 1.0) {
            std::this_thread::sleep_for(
                std::chrono::microseconds(static_cast<int64_t>(remaining_us)));
        }
    }

    const auto snap = counters.snapshot_and_reset();
    const double n = std::max(1.0, snap.sample_count);
    const double u = std::max(1.0, snap.gpu_buffer_upload_count);

    auto per_frame_us = choc::value::createObject("");
    per_frame_us.addMember("audio_to_triplebuffer_copy",
                           snap.audio_copy_total_us / n);
    per_frame_us.addMember("triplebuffer_publish_latency",
                           snap.triplebuffer_publish_total_us / n);
    per_frame_us.addMember("gpu_upload_us",
                           snap.gpu_upload_total_us / n);
    per_frame_us.addMember("gpu_readback_us",
                           snap.gpu_readback_total_us / n);
    per_frame_us.addMember("gpu_dispatch_us",
                           snap.gpu_dispatch_total_us / n);
    per_frame_us.addMember("total_frame_us",
                           snap.total_frame_total_us / n);
    per_frame_us.addMember("base64_decode_us",
                           snap.base64_decode_total_us / n);

    auto per_frame_bytes = choc::value::createObject("");
    per_frame_bytes.addMember("cpu_to_gpu_bytes",
                              snap.cpu_to_gpu_bytes_total / n);
    per_frame_bytes.addMember("gpu_to_cpu_bytes",
                              snap.gpu_to_cpu_bytes_total / n);

    auto per_upload_us = choc::value::createObject("");
    per_upload_us.addMember("gpu_upload_us",
                            snap.gpu_upload_total_us / u);
    per_upload_us.addMember("base64_decode_us",
                            snap.base64_decode_total_us / u);
    per_upload_us.addMember("bytes_avg",
                            snap.cpu_to_gpu_bytes_total / u);

    // MB-fraction per the re-evaluation plan:
    //   (base64_decode_us + gpu_upload_us) / frame_budget_us
    // where the us values are per-frame averages.
    const double per_frame_decode_us = snap.base64_decode_total_us / n;
    const double per_frame_upload_us = snap.gpu_upload_total_us / n;
    const double memory_bandwidth_fraction =
        frame_budget_us > 0.0
            ? (per_frame_decode_us + per_frame_upload_us) / frame_budget_us
            : 0.0;

    auto root = choc::value::createObject("");
    root.addMember("host", current_host_short_threejs());
    root.addMember("date", current_iso8601_utc_threejs());
    root.addMember("pulp_commit", current_short_sha_threejs());
    root.addMember("platform", current_platform_tag_threejs());
    root.addMember("widget", cfg.widget);
    root.addMember("particle_count", static_cast<int64_t>(cfg.particle_count));
    root.addMember("seconds", static_cast<int64_t>(cfg.seconds));
    root.addMember("target_fps", static_cast<int64_t>(cfg.target_fps));
    root.addMember("samples", static_cast<int64_t>(snap.sample_count));
    root.addMember("gpu_buffer_upload_count",
                   static_cast<int64_t>(snap.gpu_buffer_upload_count));
    root.addMember("gpu_buffer_bytes_resident_peak",
                   snap.gpu_buffer_bytes_resident_peak);
    root.addMember("per_frame_us", per_frame_us);
    root.addMember("per_frame_bytes", per_frame_bytes);
    root.addMember("per_upload_us", per_upload_us);
    root.addMember("frame_budget_us", static_cast<int64_t>(frame_budget_us));
    root.addMember("memory_bandwidth_fraction", memory_bandwidth_fraction);

    auto json_str = choc::json::toString(root, /*pretty=*/true);

    auto out_path = std::filesystem::path(cfg.output_path);
    if (out_path.has_parent_path()) {
        std::filesystem::create_directories(out_path.parent_path());
    }
    std::ofstream out(out_path);
    if (!out.is_open()) {
        std::cerr << "Failed to open " << cfg.output_path << " for writing\n";
        return 1;
    }
    out << json_str << "\n";
    out.close();

    std::cout << "Benchmark complete. samples=" << static_cast<int64_t>(snap.sample_count)
              << " uploads=" << static_cast<int64_t>(snap.gpu_buffer_upload_count)
              << " mb_fraction=" << (memory_bandwidth_fraction * 100.0) << "%"
              << "\nJSON written to " << cfg.output_path << "\n";
    return 0;
}

#endif  // PULP_BENCHMARK

bool load_demo(DemoEnvironment& env, int width, int height, DemoMode mode,
               std::string& error_out, int particle_count) {
    env.bridge->load_script("");

    bool module_completed = false;
    std::string module_error;
    env.engine.run_module(
        make_threejs_demo_module(width, height, mode, particle_count),
        resolve_threejs_module,
        [&](const std::string& error, const choc::value::Value&) {
            module_completed = true;
            module_error = error;
        });

    // Drain microtasks AND pump requestAnimationFrame callbacks while waiting for
    // the module's top-level promise to settle. Without servicing frame callbacks
    // here, a headless `--capture` run hangs at `status: 'starting'` because the
    // frame clock (normally driven by the windowed NSTimer/run_event_loop) never
    // ticks, so any rAF registered by three.webgpu.js / OrbitControls during the
    // `await renderer.init()` chain never fires and the module promise stays
    // pending. Fixes #542.
    for (int i = 0; i < 1024; ++i) {
        env.advance_sources(1.0f / 60.0f);
        if (env.bridge) env.bridge->service_frame_callbacks();
        env.engine.pump_message_loop();
        const auto status = eval_string(
            env.engine,
            "globalThis.__pulpThreeDemoState && globalThis.__pulpThreeDemoState.status || ''");
        if (status == "ready" || status == "error") {
            break;
        }
    }

    // Post-ready drain: the module promise already settled in the loop
    // above; this second pass just lets any trailing microtasks (module
    // `export default` finalization, promise `.then` queues) flush before
    // we hand control back to the caller. We intentionally do NOT call
    // `service_frame_callbacks()` here — rAF self-rearms, so draining
    // frames would render 64 real frames of animation before `load_demo`
    // returns, skewing initial capture state (frame counter, scene age).
    // Codex 2026-04-21 review on #553. The hang itself is fixed by the
    // bounded service_frame_callbacks pump above (#542), not by this
    // drain.
    for (int i = 0; i < 64; ++i) {
        env.engine.pump_message_loop();
    }

    if (!module_completed) {
        error_out = "Three.js demo module did not complete";
        return false;
    }
    if (!module_error.empty()) {
        error_out = module_error;
        return false;
    }

    const auto status = eval_string(env.engine, "globalThis.__pulpThreeDemoState.status");
    if (status != "ready") {
        const auto state = eval_string(env.engine, "JSON.stringify(globalThis.__pulpThreeDemoState || {})");
        error_out = "Three.js demo failed: " + state;
        return false;
    }

    return true;
}

} // namespace

int main(int argc, char* argv[]) {
#ifdef PULP_BENCHMARK
    ParticleBenchmarkConfig bench_cfg;
#endif

    auto starts_with = [](const char* s, const char* prefix) -> bool {
        while (*prefix) {
            if (*s++ != *prefix++) return false;
        }
        return true;
    };

    int width = 820;
    int height = 560;
    DemoMode mode = DemoMode::spectrum;
    int particle_count = 480;
    std::optional<std::filesystem::path> capture_path;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            std::string size = argv[++i];
            const auto x = size.find('x');
            if (x != std::string::npos) {
                width = std::stoi(size.substr(0, x));
                height = std::stoi(size.substr(x + 1));
            }
        } else if (std::strcmp(argv[i], "--demo") == 0 && i + 1 < argc) {
            const std::string value = argv[++i];
            if (value == "cube") {
                mode = DemoMode::cube;
            } else if (value == "spectrum") {
                mode = DemoMode::spectrum;
            } else if (value == "particles") {
                mode = DemoMode::particles;
            } else if (value == "ribbon") {
                mode = DemoMode::ribbon;
            } else if (value == "reverb") {
                mode = DemoMode::reverb;
            } else {
                std::cerr << "Unknown demo mode: " << value << "\n";
                return 1;
            }
        } else if (std::strcmp(argv[i], "--capture") == 0 && i + 1 < argc) {
            capture_path = std::filesystem::path(argv[++i]);
        } else if (std::strcmp(argv[i], "--particle-count") == 0 && i + 1 < argc) {
            try { particle_count = std::max(1, std::stoi(argv[++i])); } catch (...) {}
        }
#ifdef PULP_BENCHMARK
        else if (starts_with(argv[i], "--benchmark-seconds=")) {
            bench_cfg.enabled = true;
            try { bench_cfg.seconds = std::stoi(argv[i] + 20); } catch (...) {}
        } else if (std::strcmp(argv[i], "--benchmark-seconds") == 0 && i + 1 < argc) {
            bench_cfg.enabled = true;
            try { bench_cfg.seconds = std::stoi(argv[++i]); } catch (...) {}
        } else if (starts_with(argv[i], "--widget=")) {
            bench_cfg.widget = argv[i] + 9;
        } else if (std::strcmp(argv[i], "--widget") == 0 && i + 1 < argc) {
            bench_cfg.widget = argv[++i];
        } else if (starts_with(argv[i], "--output=")) {
            bench_cfg.output_path = argv[i] + 9;
        } else if (std::strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            bench_cfg.output_path = argv[++i];
        } else if (starts_with(argv[i], "--target-fps=")) {
            try { bench_cfg.target_fps = std::stoi(argv[i] + 13); } catch (...) {}
        } else if (std::strcmp(argv[i], "--target-fps") == 0 && i + 1 < argc) {
            try { bench_cfg.target_fps = std::stoi(argv[++i]); } catch (...) {}
        } else if (starts_with(argv[i], "--particle-count=")) {
            try { bench_cfg.particle_count = std::max(1, std::stoi(argv[i] + 17)); } catch (...) {}
        }
#endif
    }

#ifdef PULP_BENCHMARK
    if (bench_cfg.enabled) {
        return run_particle_benchmark(bench_cfg);
    }
#endif

    if (!is_engine_available(JsEngineType::v8)) {
        std::cerr << "V8 is required for the native Three.js demo\n";
        return 1;
    }

    DemoEnvironment env(static_cast<float>(width), static_cast<float>(height));

    WindowOptions opts;
    opts.title = "Pulp Native Three.js Demo";
    opts.width = width;
    opts.height = height;
    opts.use_gpu = true;
    opts.initially_hidden = capture_path.has_value();

    auto window = WindowHost::create(env.root, opts);
    if (!window) {
        std::cerr << "WindowHost::create failed\n";
        return 1;
    }

    env.attach_gpu_surface(window->gpu_surface());
    if (!env.has_native_gpu()) {
        env.initialize_offscreen_gpu(static_cast<float>(width), static_cast<float>(height));
    }
    if (!env.has_native_gpu()) {
        std::cerr << "Native Dawn adapter unavailable on this host/backend\n";
        return 1;
    }

    if (mode != DemoMode::cube) {
        env.enable_audio_source(mode == DemoMode::reverb);
    }

    env.bridge->set_repaint_callback([host = window.get()] {
        if (host) host->repaint();
    });

    if (auto* clock = env.root.frame_clock()) {
        clock->subscribe([bridge = env.bridge.get(), &env](float dt) {
            env.advance_sources(dt);
            if (bridge) bridge->service_frame_callbacks();
            return true;
        });
    }

    std::string error;
    if (!load_demo(env, width, height, mode, error, particle_count)) {
        std::cerr << error << "\n";
        return 1;
    }

    prime_demo_frames(env, mode == DemoMode::cube ? 4 : 10);
    env.root.layout_children();
    window->repaint();
    std::cout << "Three.js native demo ready (" << demo_mode_name(mode) << "): "
              << eval_string(env.engine, "JSON.stringify(globalThis.__pulpThreeDemoState)") << "\n";

    if (capture_path) {
        const auto png = window->capture_png();
        if (png.empty()) {
            std::cerr << "Demo capture failed\n";
            return 1;
        }
        write_binary_file(*capture_path, png);
        std::cout << "Captured demo frame: " << capture_path->string() << "\n";
        return 0;
    }

    window->run_event_loop();
    return 0;
}
