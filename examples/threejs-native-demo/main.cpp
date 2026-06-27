#include <pulp/render/gpu_surface.hpp>
#include <pulp/render/headless_surface.hpp>
#include <pulp/render/skia_surface.hpp>
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
#include <array>
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
#include <vector>
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
    reverb,
    gltf_box
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
        case DemoMode::gltf_box: return "gltf-box";
    }
    return "cube";
}

std::string js_single_quoted_string(std::string value) {
    std::string out;
    out.reserve(value.size() + 2);
    for (char c : value) {
        if (c == '\\' || c == '\'') {
            out.push_back('\\');
            out.push_back(c);
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::vector<uint8_t> make_textured_box_glb() {
    struct Vertex {
        float px, py, pz;
        float nx, ny, nz;
        float u, v;
    };

    const std::array<Vertex, 24> vertices = {{
        {-0.5f, -0.5f,  0.5f,  0,  0,  1, 0, 0}, { 0.5f, -0.5f,  0.5f,  0,  0,  1, 1, 0}, { 0.5f,  0.5f,  0.5f,  0,  0,  1, 1, 1}, {-0.5f,  0.5f,  0.5f,  0,  0,  1, 0, 1},
        { 0.5f, -0.5f, -0.5f,  0,  0, -1, 0, 0}, {-0.5f, -0.5f, -0.5f,  0,  0, -1, 1, 0}, {-0.5f,  0.5f, -0.5f,  0,  0, -1, 1, 1}, { 0.5f,  0.5f, -0.5f,  0,  0, -1, 0, 1},
        {-0.5f, -0.5f, -0.5f, -1,  0,  0, 0, 0}, {-0.5f, -0.5f,  0.5f, -1,  0,  0, 1, 0}, {-0.5f,  0.5f,  0.5f, -1,  0,  0, 1, 1}, {-0.5f,  0.5f, -0.5f, -1,  0,  0, 0, 1},
        { 0.5f, -0.5f,  0.5f,  1,  0,  0, 0, 0}, { 0.5f, -0.5f, -0.5f,  1,  0,  0, 1, 0}, { 0.5f,  0.5f, -0.5f,  1,  0,  0, 1, 1}, { 0.5f,  0.5f,  0.5f,  1,  0,  0, 0, 1},
        {-0.5f,  0.5f,  0.5f,  0,  1,  0, 0, 0}, { 0.5f,  0.5f,  0.5f,  0,  1,  0, 1, 0}, { 0.5f,  0.5f, -0.5f,  0,  1,  0, 1, 1}, {-0.5f,  0.5f, -0.5f,  0,  1,  0, 0, 1},
        {-0.5f, -0.5f, -0.5f,  0, -1,  0, 0, 0}, { 0.5f, -0.5f, -0.5f,  0, -1,  0, 1, 0}, { 0.5f, -0.5f,  0.5f,  0, -1,  0, 1, 1}, {-0.5f, -0.5f,  0.5f,  0, -1,  0, 0, 1}
    }};
    const std::array<uint16_t, 36> indices = {{
        0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7, 8, 9, 10, 8, 10, 11,
        12, 13, 14, 12, 14, 15, 16, 17, 18, 16, 18, 19, 20, 21, 22, 20, 22, 23
    }};

    std::vector<uint8_t> bin;
    auto append_float = [&](float value) {
        uint32_t bits = 0;
        static_assert(sizeof(bits) == sizeof(value));
        std::memcpy(&bits, &value, sizeof(value));
        bin.push_back(static_cast<uint8_t>(bits & 0xff));
        bin.push_back(static_cast<uint8_t>((bits >> 8) & 0xff));
        bin.push_back(static_cast<uint8_t>((bits >> 16) & 0xff));
        bin.push_back(static_cast<uint8_t>((bits >> 24) & 0xff));
    };
    auto append_u16 = [&](uint16_t value) {
        bin.push_back(static_cast<uint8_t>(value & 0xff));
        bin.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    };

    const uint32_t position_offset = static_cast<uint32_t>(bin.size());
    for (const auto& v : vertices) {
        append_float(v.px); append_float(v.py); append_float(v.pz);
    }
    const uint32_t normal_offset = static_cast<uint32_t>(bin.size());
    for (const auto& v : vertices) {
        append_float(v.nx); append_float(v.ny); append_float(v.nz);
    }
    const uint32_t uv_offset = static_cast<uint32_t>(bin.size());
    for (const auto& v : vertices) {
        append_float(v.u); append_float(v.v);
    }
    const uint32_t index_offset = static_cast<uint32_t>(bin.size());
    for (auto index : indices) {
        append_u16(index);
    }

    const char* texture_png =
        "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAFklEQVR42mP4Yh/33+TyzP8MIALEAQBa+Qpfg6yfXgAAAABJRU5ErkJggg==";
    std::ostringstream json;
    json
        << "{\"asset\":{\"version\":\"2.0\",\"generator\":\"pulp-threejs-native-demo\"},"
        << "\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
        << "\"nodes\":[{\"mesh\":0,\"name\":\"BoxTextured\"}],"
        << "\"meshes\":[{\"name\":\"BoxTextured\",\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},\"indices\":3,\"material\":0}]}],"
        << "\"materials\":[{\"name\":\"PulpTextureMaterial\",\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0},\"metallicFactor\":0,\"roughnessFactor\":0.7}}],"
        << "\"textures\":[{\"sampler\":0,\"source\":0}],\"samplers\":[{\"magFilter\":9728,\"minFilter\":9728,\"wrapS\":10497,\"wrapT\":10497}],"
        << "\"images\":[{\"mimeType\":\"image/png\",\"uri\":\"data:image/png;base64," << texture_png << "\"}],"
        << "\"buffers\":[{\"byteLength\":" << bin.size() << "}],"
        << "\"bufferViews\":["
        << "{\"buffer\":0,\"byteOffset\":" << position_offset << ",\"byteLength\":" << (vertices.size() * 3 * sizeof(float)) << ",\"target\":34962},"
        << "{\"buffer\":0,\"byteOffset\":" << normal_offset << ",\"byteLength\":" << (vertices.size() * 3 * sizeof(float)) << ",\"target\":34962},"
        << "{\"buffer\":0,\"byteOffset\":" << uv_offset << ",\"byteLength\":" << (vertices.size() * 2 * sizeof(float)) << ",\"target\":34962},"
        << "{\"buffer\":0,\"byteOffset\":" << index_offset << ",\"byteLength\":" << (indices.size() * sizeof(uint16_t)) << ",\"target\":34963}],"
        << "\"accessors\":["
        << "{\"bufferView\":0,\"componentType\":5126,\"count\":24,\"type\":\"VEC3\",\"min\":[-0.5,-0.5,-0.5],\"max\":[0.5,0.5,0.5]},"
        << "{\"bufferView\":1,\"componentType\":5126,\"count\":24,\"type\":\"VEC3\"},"
        << "{\"bufferView\":2,\"componentType\":5126,\"count\":24,\"type\":\"VEC2\"},"
        << "{\"bufferView\":3,\"componentType\":5123,\"count\":36,\"type\":\"SCALAR\"}]}";

    std::string json_text = json.str();
    while ((json_text.size() % 4) != 0) json_text.push_back(' ');
    while ((bin.size() % 4) != 0) bin.push_back(0);

    const uint32_t total_length = static_cast<uint32_t>(12 + 8 + json_text.size() + 8 + bin.size());
    std::vector<uint8_t> out;
    out.reserve(total_length);
    auto append_u32 = [&](uint32_t value) {
        out.push_back(static_cast<uint8_t>(value & 0xff));
        out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
        out.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
        out.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
    };
    append_u32(0x46546c67);
    append_u32(2);
    append_u32(total_length);
    append_u32(static_cast<uint32_t>(json_text.size()));
    append_u32(0x4e4f534a);
    out.insert(out.end(), json_text.begin(), json_text.end());
    append_u32(static_cast<uint32_t>(bin.size()));
    append_u32(0x004e4942);
    out.insert(out.end(), bin.begin(), bin.end());
    return out;
}

std::string ensure_gltf_box_asset_url() {
    const auto path = std::filesystem::temp_directory_path()
        / "pulp-threejs-native-demo" / "BoxTextured.glb";
    std::filesystem::create_directories(path.parent_path());
    write_binary_file(path, make_textured_box_glb());
    const auto generic = path.generic_string();
    return std::string("file://") + (generic.empty() || generic.front() == '/' ? "" : "/") + generic;
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
                                      int particle_count = 480,
                                      const std::string& gltf_box_url = "") {
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
    replace_all("__GLTF_BOX_URL__", js_single_quoted_string(gltf_box_url));

    return js;
}

void prime_demo_frames(DemoEnvironment& env, int frames) {
    if (!env.bridge) return;
    for (int i = 0; i < frames; ++i) {
        env.advance_sources(1.0f / 60.0f);
        env.bridge->service_frame_callbacks();
        env.engine.pump_message_loop();
    }
}

std::vector<uint8_t> capture_demo_offscreen_png(DemoEnvironment& env,
                                                int width,
                                                int height,
                                                std::string& error_out) {
    if (!env.gpu_surface) {
        error_out = "Offscreen capture has no GPU surface";
        return {};
    }

    auto skia = pulp::render::SkiaSurface::create(*env.gpu_surface, {
        .width = static_cast<uint32_t>(std::max(1, width)),
        .height = static_cast<uint32_t>(std::max(1, height)),
        .scale_factor = 2.0f
    });
    if (!skia || !skia->is_available()) {
        error_out = "Offscreen capture Skia surface unavailable";
        return {};
    }

    if (!env.gpu_surface->begin_frame()) {
        error_out = "Offscreen capture GpuSurface::begin_frame failed";
        return {};
    }

    auto* canvas = skia->begin_frame();
    if (!canvas) {
        env.gpu_surface->end_frame();
        error_out = "Offscreen capture SkiaSurface::begin_frame failed";
        return {};
    }

    env.root.set_bounds({0, 0, static_cast<float>(width), static_cast<float>(height)});
    env.root.layout_children();
    canvas->set_fill_color(pulp::canvas::Color::rgba8(30, 30, 46));
    canvas->fill_rect(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
    env.root.paint_all(*canvas);
    pulp::view::View::paint_overlays(*canvas, &env.root);

    skia->end_frame();

    std::vector<uint8_t> pixels;
    uint32_t pixel_width = 0;
    uint32_t pixel_height = 0;
    const bool read = skia->read_current_rgba(pixels, pixel_width, pixel_height);
    env.gpu_surface->end_frame();

    if (!read) {
        error_out = "Offscreen capture read_current_rgba failed";
        return {};
    }

    pulp::render::HeadlessSurface::Rgba rgba;
    rgba.pixels = std::move(pixels);
    rgba.width = pixel_width;
    rgba.height = pixel_height;
    std::string encode_error;
    auto png = pulp::render::HeadlessSurface::encode_png(rgba, &encode_error);
    if (png.empty()) {
        error_out = encode_error.empty() ? "Offscreen capture PNG encode failed" : encode_error;
    }
    return png;
}

#ifdef PULP_BENCHMARK

// ── Zero-copy benchmark mode ────────────────────────────────────────────────
//
// Drives the *real* JS→GPU upload path: Three.js BufferGeometry +
// BufferAttribute(Float32Array, 3), with `needsUpdate = true` per
// frame so the vertex buffer is re-uploaded via queue.WriteBuffer.
// This exercises the same upload code path a real JS-scripted plugin UI
// would use, not the C++-driven VisualizationBridge path. Headless: no
// WindowHost, offscreen Dawn surface only. Emits JSON with the benchmark
// schema fields base64_decode_us, gpu_buffer_upload_count, and
// gpu_buffer_bytes_resident_peak.
//
// MB-fraction is computed as:
//   (base64_decode_us + gpu_upload_us) / frame_budget_us.

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
    // MSVC's CRT exposes the pipe helpers as `_popen` / `_pclose`;
    // bare `popen` / `pclose` are POSIX spellings that don't link on
    // Windows. Mirror the pattern already used in tools/mcp/pulp_mcp.cpp
    // so `PULP_BENCHMARK`-enabled Windows builds don't fail to link.
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

    std::cout << "Pulp zero-copy particle benchmark\n"
              << "  widget:         " << cfg.widget << "\n"
              << "  particle count: " << cfg.particle_count << "\n"
              << "  seconds:        " << cfg.seconds << "\n"
              << "  target FPS:     " << cfg.target_fps << "\n"
              << "  output:         " << cfg.output_path << "\n";

    if (cfg.output_path.empty()) {
        std::cerr << "--benchmark-seconds requires --output=<path>\n";
        return 2;
    }

    // This harness is particle-only. `--widget=<anything-else>` used to
    // silently run the particle workload while writing the user-supplied
    // label into the output JSON, which corrupts benchmark comparisons.
    // Reject any unsupported value up front so baselines stay honest.
    if (cfg.widget != "particles") {
        std::cerr << "Unsupported --widget=\"" << cfg.widget
                  << "\"; only 'particles' is implemented in this lane.\n"
                  << "Add a real harness before re-enabling.\n";
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
            std::cerr << "Benchmark warm-up upload returned '" << last
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

    // MB-fraction:
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
               std::string& error_out, int particle_count,
               const std::string& gltf_box_url) {
    env.bridge->load_script("");

    bool module_completed = false;
    std::string module_error;
    env.engine.run_module(
        make_threejs_demo_module(width, height, mode, particle_count, gltf_box_url),
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
    // pending.
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
    // The hang itself is fixed by the bounded service_frame_callbacks pump
    // above, not by this drain.
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

// Emit a machine-parseable identity block proving exactly which JS runtime and
// GPU adapter this binary is actually running on. Used by the strict provider-
// identity CTest and provider report. Brings up a real V8 ScriptEngine and a
// real offscreen Dawn surface so every field is observed, not assumed.
int print_engine_identity(int width, int height) {
    const bool has_v8 = is_engine_available(JsEngineType::v8);
    // Only construct the engine when V8 is linked — ScriptEngine(v8) throws
    // otherwise. When it is missing we still emit a truthful block with empty
    // engine fields and pulp_has_v8=0.
    std::optional<ScriptEngine> engine;
    if (has_v8)
        engine.emplace(JsEngineType::v8);

    bool gpu_available = false;
    bool gpu_native_bridge = false;
    std::string gpu_backend = "unavailable";
    bool gpu_software = true;

    auto surface = pulp::render::GpuSurface::create_dawn();
    if (surface) {
        pulp::render::GpuSurface::Config config{};
        config.width = static_cast<uint32_t>(std::max(1, width));
        config.height = static_cast<uint32_t>(std::max(1, height));
        config.native_surface_handle = nullptr;
        if (surface->initialize(config)) {
            const auto info = surface->adapter_info();
            gpu_available = info.available;
            gpu_native_bridge = info.native_bridge;
            gpu_backend = info.backend_type;
            // "software" = no real hardware adapter: a fallback/null backend or
            // an adapter that never resolved a concrete backend type.
            gpu_software = !info.available
                || info.backend_type.empty()
                || info.backend_type == "Unknown"
                || info.backend_type == "Null";
        }
    }

    std::cout << "PULP_ENGINE_IDENTITY_BEGIN\n";
    std::cout << "engine_type=" << (engine ? engine_type_name(engine->engine_type()) : "none") << "\n";
    std::cout << "runtime_version=" << (engine ? engine->runtime_version() : std::string{}) << "\n";
    std::cout << "provider_kind=" << (engine ? engine->provider_kind() : std::string{}) << "\n";
    std::cout << "provider_path=" << (engine ? engine->provider_path() : std::string{}) << "\n";
    std::cout << "expected_runtime_version=" << (engine ? engine->expected_runtime_version() : std::string{}) << "\n";
    std::cout << "pulp_has_v8=" << (has_v8 ? 1 : 0) << "\n";
    std::cout << "gpu_available=" << (gpu_available ? 1 : 0) << "\n";
    std::cout << "gpu_native_bridge=" << (gpu_native_bridge ? 1 : 0) << "\n";
    std::cout << "gpu_backend=" << gpu_backend << "\n";
    std::cout << "gpu_software=" << (gpu_software ? 1 : 0) << "\n";
    std::cout << "PULP_ENGINE_IDENTITY_END\n";
    std::cout.flush();
    return 0;
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
    bool print_identity = false;
    std::optional<std::filesystem::path> capture_path;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--print-engine-identity") == 0) {
            print_identity = true;
        } else if (std::strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
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
            } else if (value == "gltf-box") {
                mode = DemoMode::gltf_box;
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

    if (print_identity) {
        return print_engine_identity(width, height);
    }

    if (!is_engine_available(JsEngineType::v8)) {
        std::cerr << "V8 is required for the native Three.js demo\n";
        return 1;
    }

    const auto gltf_box_url = mode == DemoMode::gltf_box ? ensure_gltf_box_asset_url() : std::string();

    if (capture_path) {
        DemoEnvironment capture_env(static_cast<float>(width), static_cast<float>(height));
        capture_env.initialize_offscreen_gpu(static_cast<float>(width), static_cast<float>(height));
        if (!capture_env.has_native_gpu()) {
            std::cerr << "Native Dawn adapter unavailable on this host/backend\n";
            return 1;
        }

        if (mode != DemoMode::cube && mode != DemoMode::gltf_box) {
            capture_env.enable_audio_source(mode == DemoMode::reverb);
        }

        std::string error;
        if (!load_demo(capture_env, width, height, mode, error, particle_count, gltf_box_url)) {
            std::cerr << error << "\n";
            return 1;
        }

        prime_demo_frames(capture_env, mode == DemoMode::cube ? 4 : 10);
        capture_env.root.layout_children();
        std::cout << "Three.js native demo ready (" << demo_mode_name(mode) << "): "
                  << eval_string(capture_env.engine, "JSON.stringify(globalThis.__pulpThreeDemoState)") << "\n";

        std::string capture_error;
        const auto png = capture_demo_offscreen_png(capture_env, width, height, capture_error);
        if (png.empty()) {
            std::cerr << (capture_error.empty() ? "Demo capture failed" : capture_error) << "\n";
            return 1;
        }
        write_binary_file(*capture_path, png);
        std::cout << "Captured demo frame: " << capture_path->string() << "\n";
        return 0;
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

    if (mode != DemoMode::cube && mode != DemoMode::gltf_box) {
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
    if (!load_demo(env, width, height, mode, error, particle_count, gltf_box_url)) {
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
