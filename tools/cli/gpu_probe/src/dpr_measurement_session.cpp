#include <pulp_tooling/gpu_probe/dpr_measurement.hpp>

#include <pulp/runtime/crypto.hpp>
#include <pulp/runtime/trace.hpp>
#include <pulp/runtime/trace_session.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/plugin_frame_renderer.hpp>
#include <pulp/view/pointer_dispatch.hpp>
#include <pulp/view/screenshot_compare.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/widget_bridge.hpp>
#include <pulp/render/bench/perf_counters.hpp>
#include <pulp/render/headless_surface.hpp>

#include <choc/text/choc_JSON.h>
#include <dawn/webgpu_cpp.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <optional>
#include <limits>
#include <spawn.h>
#include <string>
#include <thread>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/utsname.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif
#include <unistd.h>
#include <vector>

extern char** environ;

namespace pulp::tooling::gpu_probe {
namespace {

using Clock = std::chrono::steady_clock;

double elapsed_ms(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const auto middle = values.size() / 2;
    return values.size() % 2 != 0 ? values[middle]
        : (values[middle - 1] + values[middle]) * 0.5;
}

double empirical_resolution(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    double resolution = std::numeric_limits<double>::infinity();
    for (std::size_t i = 1; i < values.size(); ++i)
        if (values[i] > values[i - 1])
            resolution = std::min(resolution, values[i] - values[i - 1]);
    if (std::isfinite(resolution) && resolution > 0.0) return resolution;
    return std::max(1e-9, values.empty() ? 1e-9 : values.front() / 1000.0);
}

[[maybe_unused]] bool write_bytes(const std::filesystem::path& path,
                 const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    return output.good();
}

bool write_text(const std::filesystem::path& path, std::string_view text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
    return output.good();
}

std::optional<std::string> read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(input), {});
}

double effective_dpr(const DprMeasurementRequest& request) {
    return request.mode == "configured_max"
        ? std::min(request.requested_dpr, 2.0) : request.requested_dpr;
}

constexpr std::string_view kThreeJsDprModule = R"JS(
import * as THREE from 'three/webgpu';
const canvas = document.createElement('canvas');
canvas.id = 'pulp-dpr-threejs-canvas';
canvas.width = 900; canvas.height = 600;
canvas.style.width = '900px'; canvas.style.height = '600px';
document.body.appendChild(canvas);
const label = document.createElement('div');
label.textContent = 'Pulp deterministic audio-reactive spectrum';
document.body.appendChild(label);
const context = canvas.getContext('webgpu');
const renderer = new THREE.WebGPURenderer({ canvas, context, antialias: false });
await renderer.init();
renderer.setSize(900, 600, false);
const scene = new THREE.Scene();
scene.background = new THREE.Color(0x101624);
const camera = new THREE.OrthographicCamera(-8, 8, 5, -5, 0.1, 20);
camera.position.z = 5;
const bars = [];
for (let i = 0; i < 16; ++i) {
  const mesh = new THREE.Mesh(
    new THREE.BoxGeometry(0.7, 1, 0.4),
    new THREE.MeshBasicMaterial({ color: new THREE.Color().setHSL(i / 20, 0.8, 0.55) })
  );
  mesh.position.x = -7.5 + i;
  scene.add(mesh); bars.push(mesh);
}
globalThis.__pulpDprThreeFrame = (frame) => {
  for (let i = 0; i < bars.length; ++i) {
    const magnitude = 0.35 + 2.8 * Math.abs(Math.sin(frame * 0.11 + i * 0.37));
    bars[i].scale.y = magnitude;
    bars[i].position.y = -4.5 + magnitude * 0.5;
  }
  renderer.render(scene, camera);
  if (typeof context.present === 'function') context.present();
  renderer.render(scene, camera);
  if (typeof context.present === 'function') context.present();
  return true;
};
globalThis.__pulpDprThreeReady = true;
export default true;
)JS";

struct Session {
    state::StateStore store;
    view::View root;
    view::ScriptEngine engine;
    view::WidgetBridge bridge{engine, root, store};
#ifdef PULP_BENCHMARK
    render::bench::PerfCounters counters;
#endif
    view::EditorSurfaces surfaces;
    view::PluginFrameRenderer renderer;
    view::PendingDamage damage;
    view::FrameGeometry geometry;
    render::GpuSurface::AdapterInfo adapter;
    bool hardware_adapter = false;
    std::vector<std::uint8_t> latest_rgba;
    std::uint32_t capture_width = 0;
    std::uint32_t capture_height = 0;
    bool threejs = false;

    bool initialize(const DprMeasurementRequest& request,
                    const std::filesystem::path& source, std::string& error) {
        const auto dpr = effective_dpr(request);
        geometry.width = static_cast<float>(request.logical_width);
        geometry.height = static_cast<float>(request.logical_height);
        geometry.scale = static_cast<float>(dpr);
        root.set_theme(view::Theme::dark());
        root.set_bounds({0, 0, geometry.width, geometry.height});
        root.flex().direction = view::FlexDirection::column;

        surfaces = view::create_editor_surfaces(
            nullptr, geometry, view::PluginViewHost::PresentPolicy::nonblocking,
            true, false, "DprMeasurement");
        if (!surfaces.ok()) {
            error = "Dawn/Skia measurement surfaces are unavailable";
            return false;
        }
        adapter = surfaces.gpu->adapter_info();
        if (!adapter.available || adapter.name.empty() || adapter.backend.empty()) {
            error = "Dawn did not return authentic adapter identity";
            return false;
        }
        auto* device = static_cast<wgpu::Device*>(surfaces.gpu->dawn_device_handle());
        wgpu::AdapterInfo exact{};
        if (!device || device->GetAdapterInfo(&exact) != wgpu::Status::Success) {
            error = "Dawn exact adapter type is unavailable";
            return false;
        }
        hardware_adapter = exact.adapterType == wgpu::AdapterType::IntegratedGPU ||
            exact.adapterType == wgpu::AdapterType::DiscreteGPU;
        if (!hardware_adapter) {
            error = "DPR measurement requires a hardware Dawn adapter";
            return false;
        }
        bridge.attach_gpu_surface(surfaces.gpu.get());
        bridge.install_runtime_import_handlers();
#ifdef PULP_BENCHMARK
        counters.reset();
        bridge.set_bench_counters(&counters);
#else
        error = "measurement target requires a PULP_BENCHMARK=ON build";
        return false;
#endif
        const auto script = read_text(source);
        if (!script || script->empty()) {
            error = "frozen fixture source could not be read";
            return false;
        }
        threejs = request.scenario_id == "threejs-audio-reactive";
        {
            PULP_TRACE_SCOPE_NAMED_ARGS("js", "dpr_fixture_script_load",
                                        "gpu_evidence_id", request.attempt_nonce);
            PULP_TRACE_SCOPE_NAMED_ARGS("text", "dpr_fixture_text_construction",
                                        "gpu_evidence_id", request.attempt_nonce);
            bridge.set_script_base_dir(source.parent_path());
            if (!threejs) {
                bridge.load_script(*script);
            } else {
#if PULP_DPR_HAS_THREEJS
                if (engine.engine_type() != view::JsEngineType::v8) {
                    error = "Three.js DPR measurement requires the V8 engine";
                    return false;
                }
                const auto runtime_root = std::filesystem::path(PULP_DPR_THREEJS_SOURCE_DIR);
                const auto webgpu = read_text(runtime_root / "build/three.webgpu.js");
                const auto core = read_text(runtime_root / "build/three.core.js");
                if (!webgpu || !core ||
                    runtime::sha256_hex(*webgpu) != PULP_DPR_THREEJS_WEBGPU_SHA256 ||
                    runtime::sha256_hex(*core) != PULP_DPR_THREEJS_CORE_SHA256) {
                    error = "pinned Three.js runtime identity is unavailable";
                    return false;
                }
                bridge.load_script("");
                bool completed = false;
                std::string module_error;
                engine.run_module(std::string(kThreeJsDprModule),
                    [webgpu, core](std::string_view path) -> std::optional<std::string> {
                        if (path == "three/webgpu") return *webgpu;
                        if (path == "./three.core.js" || path == "three.core.js") return *core;
                        return std::nullopt;
                    },
                    [&](const std::string& message, const choc::value::Value&) {
                        completed = true;
                        module_error = message;
                    });
                for (int i = 0; i < 1024 && !completed; ++i) {
                    bridge.service_frame_callbacks();
                    engine.pump_message_loop();
                }
                if (!completed || !module_error.empty() ||
                    !engine.evaluate("globalThis.__pulpDprThreeReady === true")
                         .getWithDefault<bool>(false)) {
                    error = module_error.empty()
                        ? "Three.js DPR module did not become ready" : module_error;
                    return false;
                }
#else
                error = "measurement build lacks the pinned Three.js runtime";
                return false;
#endif
            }
        }
        {
            PULP_TRACE_SCOPE_NAMED_ARGS("layout", "dpr_fixture_layout",
                                        "gpu_evidence_id", request.attempt_nonce);
            root.layout_children();
        }
        return true;
    }

    bool frame(const DprMeasurementRequest& request, std::uint32_t index,
               bool capture, double& cpu_ms, double& gpu_ms) {
        const auto started = Clock::now();
        PULP_TRACE_SCOPE_NAMED_ARGS("render", "frame_dpr_measurement",
                                    "gpu_evidence_id", request.attempt_nonce,
                                    "frame_index", index);
        if (index == 0) {
            PULP_TRACE_SCOPE_NAMED_ARGS("gpu", "gpu_resource_upload_dpr_fixture",
                                        "gpu_evidence_id", request.attempt_nonce,
                                        "frame_index", index);
        }
        if (threejs) {
            if (!engine.evaluate("globalThis.__pulpDprThreeFrame(" +
                                 std::to_string(index) + ")")
                     .getWithDefault<bool>(false))
                return false;
            for (int i = 0; i < 8; ++i) engine.pump_message_loop();
        }
        view::PluginFrameRenderer::Request frame;
        frame.root = &root;
        frame.geometry = geometry;
        frame.idle = [this] { bridge.service_frame_callbacks(); };
        if (capture) {
            latest_rgba.clear();
            frame.capture = &latest_rgba;
            frame.capture_width = &capture_width;
            frame.capture_height = &capture_height;
        }
        damage.mark_full();
        const auto result = renderer.render(*surfaces.gpu, *surfaces.skia,
                                            damage, frame);
        cpu_ms = elapsed_ms(started);
        gpu_ms = surfaces.skia->gpu_render_time_ms();
        return result.reached_output() && (!capture || result.readback_ok) &&
            surfaces.skia->gpu_render_timing_available();
    }

    bool set_scale(const DprMeasurementRequest& request, double scale,
                   std::uint32_t index, double& cpu_ms, double& gpu_ms) {
        geometry.scale = static_cast<float>(scale);
        return frame(request, index, false, cpu_ms, gpu_ms) &&
            std::abs(static_cast<double>(geometry.scale) - scale) < 1e-6;
    }
};

[[maybe_unused]] choc::value::Value adapter_json(const render::GpuSurface::AdapterInfo& adapter) {
    auto value = choc::value::createObject("");
    value.setMember("name", adapter.name);
    value.setMember("backend", adapter.backend);
    value.setMember("driver", adapter.description.empty()
        ? adapter.architecture : adapter.description);
    value.setMember("class", "hardware");
    value.setMember("authentic_identity", adapter.available && adapter.native_bridge);
    return value;
}

[[maybe_unused]] choc::value::Value machine_json() {
    struct utsname info{};
    (void)uname(&info);
    char host[256]{};
    (void)gethostname(host, sizeof(host) - 1);
    auto value = choc::value::createObject("");
    value.setMember("id", std::string(host) + ":" + info.machine);
    value.setMember("hostname", std::string(host));
    value.setMember("os", std::string(info.sysname) + " " + info.release);
    value.setMember("architecture", std::string(info.machine));
    std::string cpu_model = info.machine;
#if defined(__APPLE__)
    std::size_t cpu_model_size = 0;
    if (sysctlbyname("machdep.cpu.brand_string", nullptr, &cpu_model_size,
                     nullptr, 0) == 0 && cpu_model_size > 1) {
        std::string detected(cpu_model_size, '\0');
        if (sysctlbyname("machdep.cpu.brand_string", detected.data(),
                         &cpu_model_size, nullptr, 0) == 0) {
            detected.resize(std::char_traits<char>::length(detected.c_str()));
            if (!detected.empty()) cpu_model = std::move(detected);
        }
    }
#endif
    value.setMember("cpu_model", cpu_model);
    value.setMember("logical_cpu_count", static_cast<std::int64_t>(
        std::thread::hardware_concurrency()));
    return value;
}

[[maybe_unused]] bool validate_source(const DprMeasurementRequest& request,
                     std::filesystem::path& source, std::string& error) {
    std::error_code ec;
    const auto root = std::filesystem::canonical(request.pulp_source_root, ec);
    if (ec || root != request.pulp_source_root) {
        error = "Pulp source root is not its canonical directory";
        return false;
    }
    source = request.scenario_id == "threejs-audio-reactive"
        ? root / request.source
        : root / "test/fixtures/gpu-ux/dpr" / request.source;
    const auto digest = runtime::sha256_file_hex(source, 1024 * 1024);
    if (!digest || *digest != request.expected_content_digest) {
        error = "frozen fixture digest differs from the request";
        return false;
    }
    return true;
}

[[maybe_unused]] std::optional<choc::value::Value> run_first_frame_child(
    const DprMeasurementRequest& request,
    const std::filesystem::path& request_path,
    const std::filesystem::path& producer_path,
    const std::filesystem::path& output_path,
    std::string& error) {
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    std::vector<std::string> storage{
        producer_path.string(), "--request", request_path.string(),
        "--first-frame-trial", output_path.string(),
    };
    std::vector<char*> arguments;
    for (auto& item : storage) arguments.push_back(item.data());
    arguments.push_back(nullptr);
    pid_t pid = 0;
    const int spawned = posix_spawn(&pid, producer_path.c_str(), &actions, nullptr,
                                    arguments.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (spawned != 0) {
        error = "could not launch a fresh-process first-frame trial";
        return std::nullopt;
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        error = "fresh-process first-frame trial failed";
        return std::nullopt;
    }
    const auto text = read_text(output_path);
    if (!text) {
        error = "fresh-process trial ledger entry is missing";
        return std::nullopt;
    }
    try {
        auto value = choc::json::parse(*text);
        if (!value.isObject() || value["attempt_nonce"].getString() != request.attempt_nonce ||
            value["attempt_number"].getInt64() != request.attempt_number ||
            value["pid"].getInt64() != static_cast<std::int64_t>(pid)) {
            error = "fresh-process trial pid or attempt nonce is unbound";
            return std::nullopt;
        }
        return value;
    } catch (const std::exception&) {
        error = "fresh-process trial ledger entry is malformed";
        return std::nullopt;
    }
}

std::string incomplete_json(const DprMeasurementRequest& request,
                            std::string_view reason, std::string_view dependency) {
    auto result = evaluate_dpr_measurement_readiness(request);
    result.reason = std::string(reason);
    result.dependencies = {std::string(dependency)};
    return to_json(result, true) + "\n";
}

} // namespace

int run_dpr_first_frame_trial(const DprMeasurementRequest& request,
                              const std::filesystem::path& output_path,
                              const std::filesystem::path& producer_path,
                              std::string* error) {
#if !defined(PULP_BENCHMARK) || !defined(PULP_TRACING_ENABLED) || !PULP_TRACING_ENABLED
    if (error) *error = "first-frame trial requires PULP_BENCHMARK=ON and PULP_TRACING=ON";
    return 3;
#else
    std::filesystem::path source;
    std::string message;
    const auto producer_digest = runtime::sha256_file_hex(
        producer_path, 512ull * 1024ull * 1024ull);
    if (!producer_digest || !validate_source(request, source, message)) {
        if (error) *error = message.empty() ? "producer digest unavailable" : message;
        return 3;
    }
    const auto started = Clock::now();
    Session session;
    if (!session.initialize(request, source, message)) {
        if (error) *error = message;
        return 3;
    }
    double cpu = 0.0, gpu = 0.0;
    if (!session.frame(request, 0, true, cpu, gpu)) {
        if (error) *error = "first GPU frame or timestamp query did not complete";
        return 3;
    }
    auto root = choc::value::createObject("");
    root.setMember("schema", "pulp.gpu-dpr-first-frame-trial.v1");
    root.setMember("version", 1);
    root.setMember("attempt_nonce", request.attempt_nonce);
    root.setMember("attempt_number", static_cast<std::int64_t>(request.attempt_number));
    root.setMember("pid", static_cast<std::int64_t>(getpid()));
    root.setMember("producer_sha256", *producer_digest);
    root.setMember("content_digest", request.expected_content_digest);
    root.setMember("pulp_sha", request.pulp_sha);
    root.setMember("first_frame_time_ms", elapsed_ms(started));
    root.setMember("adapter", adapter_json(session.adapter));
    if (!write_text(output_path, choc::json::toString(root, true) + "\n")) {
        if (error) *error = "could not write first-frame trial";
        return 3;
    }
    return 0;
#endif
}

int run_dpr_measurement(const DprMeasurementRequest& request,
                        const std::filesystem::path& request_path,
                        const std::filesystem::path& receipt_path,
                        const std::filesystem::path& producer_path,
                        std::string* error) {
    (void)request_path;
#if !defined(PULP_BENCHMARK) || !defined(PULP_TRACING_ENABLED) || !PULP_TRACING_ENABLED
    const std::string reason =
        "native measurement requires an exact PULP_BENCHMARK=ON, PULP_TRACING=ON build";
    write_text(receipt_path, incomplete_json(request, reason,
                                             "build:benchmark-and-tracing"));
    if (error) *error = reason;
    return 3;
#else
    std::string message;
    std::filesystem::path source;
    if (!validate_source(request, source, message)) {
        write_text(receipt_path, incomplete_json(request, message, "source:exact-fixture"));
        if (error) *error = message;
        return 3;
    }
    const auto cell = std::filesystem::weakly_canonical(request.cell_directory);
    if (cell != std::filesystem::weakly_canonical(receipt_path.parent_path())) {
        message = "receipt path differs from the runner-bound cell directory";
        write_text(receipt_path, incomplete_json(request, message, "path:cell-directory"));
        if (error) *error = message;
        return 3;
    }
    const auto producer_digest = runtime::sha256_file_hex(
        producer_path, 512ull * 1024ull * 1024ull);
    if (!producer_digest) {
        message = "measurement producer digest is unavailable";
        write_text(receipt_path, incomplete_json(request, message, "build:producer-digest"));
        if (error) *error = message;
        return 3;
    }
    auto first_frame_ledger = choc::value::createEmptyArray();
    std::vector<double> first_frame_samples;
    std::vector<std::int64_t> first_frame_pids;
    for (std::uint32_t trial = 0;
         trial < request.fresh_process_first_frame_trials; ++trial) {
        const auto trial_path = cell / ("first-frame-" + request.attempt_nonce +
                                        "-" + std::to_string(trial) + ".json");
        auto entry = run_first_frame_child(request, request_path, producer_path,
                                           trial_path, message);
        if (!entry || (*entry)["producer_sha256"].getString() != *producer_digest ||
            (*entry)["content_digest"].getString() != request.expected_content_digest ||
            (*entry)["pulp_sha"].getString() != request.pulp_sha) {
            if (message.empty()) message = "fresh-process trial identity differs";
            write_text(receipt_path, incomplete_json(
                request, message, "first-frame:identity-ledger"));
            if (error) *error = message;
            return 3;
        }
        const auto pid = (*entry)["pid"].getInt64();
        if (pid == getpid() ||
            std::find(first_frame_pids.begin(), first_frame_pids.end(), pid) !=
                first_frame_pids.end()) {
            message = "fresh-process ledger reused the parent or a prior pid";
            write_text(receipt_path, incomplete_json(
                request, message, "first-frame:unique-process"));
            if (error) *error = message;
            return 3;
        }
        first_frame_pids.push_back(pid);
        first_frame_samples.push_back((*entry)["first_frame_time_ms"].getFloat64());
        first_frame_ledger.addArrayElement(std::move(*entry));
    }
    const auto trace_path = cell / ("trace-" + request.attempt_nonce + ".pftrace");
    auto tracing = runtime::Tracing::start_exclusive({}, trace_path.string(), 80u * 1024u);
    if (tracing.status != runtime::TraceStartStatus::Started || !tracing.ownership) {
        message = "exclusive in-process Perfetto session unavailable";
        write_text(receipt_path, incomplete_json(request, message, "trace:exclusive-session"));
        if (error) *error = message;
        return 3;
    }

    Session session;
    if (!session.initialize(request, source, message)) {
        (void)runtime::Tracing::stop_owned(*tracing.ownership);
        write_text(receipt_path, incomplete_json(request, message, "gpu:measurement-surface"));
        if (error) *error = message;
        return 3;
    }
    for (std::uint32_t index = 0; index < first_frame_ledger.size(); ++index) {
        const auto child_adapter = first_frame_ledger[index]["adapter"];
        if (child_adapter["name"].getString() != session.adapter.name ||
            child_adapter["backend"].getString() != session.adapter.backend ||
            child_adapter["driver"].getString() !=
                (session.adapter.description.empty() ? session.adapter.architecture
                                                     : session.adapter.description)) {
            message = "fresh-process trial used a different graphics adapter";
            (void)runtime::Tracing::stop_owned(*tracing.ownership);
            write_text(receipt_path, incomplete_json(
                request, message, "first-frame:one-adapter"));
            if (error) *error = message;
            return 3;
        }
    }
    {
        PULP_TRACE_SCOPE_NAMED_ARGS("gpu", "gpu_health_transition_dpr",
                                    "gpu_evidence_id", request.attempt_nonce,
                                    "health_state", "healthy", "sequence", 0);
        PULP_TRACE_SCOPE_NAMED_ARGS("gpu", "gpu_probe_dpr_measurement",
                                    "gpu_evidence_id", request.attempt_nonce,
                                    "health_state", "healthy", "sequence", 0,
                                    "diagnostic_code", "healthy-diagnostic");
    }

    double cpu = 0.0, gpu = 0.0;
    for (std::uint32_t i = 0; i < request.warmups; ++i) {
        if (!session.frame(request, i, false, cpu, gpu)) {
            message = "warmup frame did not reach the GPU timestamp path";
            (void)runtime::Tracing::stop_owned(*tracing.ownership);
            write_text(receipt_path, incomplete_json(request, message, "gpu:warmup"));
            if (error) *error = message;
            return 3;
        }
    }
    // Readback finalizes the current recording before SkiaSurface::end_frame()
    // can attach its elapsed-time callback. Keep captures outside the timed
    // sequence so a zero sentinel can never be promoted into a GPU sample.
    if (!session.frame(request, request.warmups, true, cpu, gpu)) {
        message = "reference frame did not reach the GPU readback path";
        (void)runtime::Tracing::stop_owned(*tracing.ownership);
        write_text(receipt_path, incomplete_json(request, message, "capture:reference"));
        if (error) *error = message;
        return 3;
    }
    bool gpu_sample_ready = false;
    for (std::uint32_t i = 0; i < 16; ++i) {
        if (!session.frame(request, request.warmups + 1 + i, false, cpu, gpu)) {
            message = "GPU timing prime frame did not complete";
            break;
        }
        if (std::isfinite(gpu) && gpu > 0.0) {
            gpu_sample_ready = true;
            break;
        }
    }
    if (!message.empty() || !gpu_sample_ready) {
        if (message.empty()) message = "GPU elapsed-time callback produced no usable sample";
        (void)runtime::Tracing::stop_owned(*tracing.ownership);
        write_text(receipt_path, incomplete_json(request, message, "gpu:timestamp-sample"));
        if (error) *error = message;
        return 3;
    }
    std::vector<double> calibration_baseline, calibration_extra;
    for (std::uint32_t trial = 0; trial < request.gpu_timer_calibration_trials;
         ++trial) {
        if (!session.frame(request, 1000 + trial, false, cpu, gpu) ||
            !std::isfinite(gpu) || gpu <= 0.0) {
            message = "GPU timer baseline calibration did not complete";
            break;
        }
        calibration_baseline.push_back(gpu);
        double extra_gpu = 0.0;
        for (std::uint32_t repeat = 0;
             repeat < request.gpu_timer_extra_work_multiplier; ++repeat) {
            if (!session.frame(request, 2000 + trial *
                    request.gpu_timer_extra_work_multiplier + repeat,
                    false, cpu, gpu) || !std::isfinite(gpu) || gpu <= 0.0) {
                message = "GPU timer extra-work calibration did not complete";
                break;
            }
            extra_gpu += gpu;
        }
        if (!message.empty()) break;
        calibration_extra.push_back(extra_gpu);
    }
    auto calibration_values = calibration_baseline;
    calibration_values.insert(calibration_values.end(), calibration_extra.begin(),
                              calibration_extra.end());
    const double timer_resolution = empirical_resolution(calibration_values);
    const double timer_baseline_median = calibration_baseline.empty()
        ? 0.0 : median(calibration_baseline);
    const double timer_extra_median = calibration_extra.empty()
        ? 0.0 : median(calibration_extra);
    const bool timer_control_detected = message.empty() &&
        timer_extra_median >= timer_baseline_median +
            std::max(timer_resolution * 2.0, timer_baseline_median * 0.10);
    if (!timer_control_detected) {
        if (message.empty()) message = "GPU timer did not detect the known-extra-work control";
        (void)runtime::Tracing::stop_owned(*tracing.ownership);
        write_text(receipt_path, incomplete_json(request, message,
                                                 "gpu:timer-calibration"));
        if (error) *error = message;
        return 3;
    }

    struct AdaptiveObservation {
        std::string phase;
        std::uint32_t frame_index = 0;
        double sample_ms = 0.0;
        double scale_before = 0.0;
        double scale_after = 0.0;
        std::string transition;
    };
    std::vector<AdaptiveObservation> adaptive_observations;
    double adaptive_budget = 0.0;
    double adaptive_initial_scale = effective_dpr(request);
    double adaptive_final_scale = adaptive_initial_scale;
    if (request.mode == "adaptive_simulation") {
        const std::array<double, 4> ladder{1.0, 1.5, 2.0, 3.0};
        const auto initial = std::find(ladder.begin(), ladder.end(),
                                       adaptive_initial_scale);
        if (initial == ladder.end()) {
            message = "adaptive initial DPR is outside the frozen ladder";
        } else {
            const auto index = static_cast<std::size_t>(initial - ladder.begin());
            const double down_target = ladder[index == 0 ? 0 : index - 1];
            const double up_target = ladder[std::min(ladder.size() - 1,
                                                     index == 0 ? 1 : index)];
            auto profile = choc::json::parse(request.adaptive_profile_json);
            const auto down_frames = static_cast<std::uint32_t>(
                profile["downshift_after_over_budget_frames"].getInt64());
            const auto up_frames = static_cast<std::uint32_t>(
                profile["upshift_after_under_budget_frames"].getInt64());
            adaptive_budget = (timer_baseline_median + timer_extra_median) * 0.5;
            double scale = adaptive_initial_scale;
            for (std::uint32_t frame = 0; frame < down_frames; ++frame) {
                const auto before = scale;
                const bool boundary = frame + 1 == down_frames;
                if (boundary && !session.set_scale(request, down_target,
                        3000 + frame, cpu, gpu)) {
                    message = "adaptive downshift did not reach an observed frame";
                    break;
                }
                if (boundary) scale = session.geometry.scale;
                adaptive_observations.push_back({
                    "over-budget", frame,
                    calibration_extra[frame % calibration_extra.size()], before, scale,
                    boundary ? (scale < before ? "downshift" : "floor-hold") : "",
                });
            }
            for (std::uint32_t frame = 0; message.empty() && frame < up_frames; ++frame) {
                const auto before = scale;
                const bool boundary = frame + 1 == up_frames;
                if (boundary && !session.set_scale(request, up_target,
                        4000 + frame, cpu, gpu)) {
                    message = "adaptive upshift did not reach an observed frame";
                    break;
                }
                if (boundary) scale = session.geometry.scale;
                adaptive_observations.push_back({
                    "under-budget", down_frames + frame,
                    calibration_baseline[frame % calibration_baseline.size()], before,
                    scale,
                    boundary ? (scale > before ? "upshift" : "ceiling-hold") : "",
                });
            }
            adaptive_final_scale = scale;
            if (message.empty() && !session.set_scale(request, adaptive_initial_scale,
                    5000, cpu, gpu))
                message = "adaptive driver could not restore the requested DPR";
        }
        if (!message.empty()) {
            (void)runtime::Tracing::stop_owned(*tracing.ownership);
            write_text(receipt_path, incomplete_json(request, message,
                                                     "adaptive:observed-transitions"));
            if (error) *error = message;
            return 3;
        }
    }
    std::vector<double> cpu_samples, gpu_samples, interaction_samples;
    std::vector<double> render_target_samples, resident_samples, upload_samples;
    view::Point last_physical{};
    view::Point last_observed_logical{};
    std::string last_observed_target;
    bool input_event_received = false;
    cpu_samples.reserve(request.measured_trials);
    gpu_samples.reserve(request.measured_trials);
    for (std::uint32_t i = 0; i < request.measured_trials; ++i) {
        const view::Point logical{static_cast<float>(request.logical_input_x),
                                  static_cast<float>(request.logical_input_y)};
        const view::Point physical{logical.x * session.geometry.scale,
                                   logical.y * session.geometry.scale};
        const view::Point observed{physical.x / session.geometry.scale,
                                   physical.y / session.geometry.scale};
        auto* expected_target = session.root.hit_test(logical);
        auto* observed_target = session.root.hit_test(observed);
        const auto interaction_started = Clock::now();
        const bool delivered = observed_target &&
            view::deliver_mouse_down(session.root, observed_target, observed, 0);
        if (!expected_target || observed_target != expected_target ||
            !delivered) {
            message = "logical-input target changed after physical-to-logical mapping";
            break;
        }
        last_physical = physical;
        last_observed_logical = observed;
        last_observed_target = "root-hit";
        input_event_received = true;
        view::deliver_mouse_up(session.root, observed_target, observed, 0, 1, {});
        if (!session.frame(request, i + request.warmups + 17, false, cpu, gpu) ||
            !std::isfinite(gpu) || gpu <= 0.0) {
            message = "measured GPU frame did not complete";
            break;
        }
        interaction_samples.push_back(elapsed_ms(interaction_started));
        cpu_samples.push_back(cpu);
        gpu_samples.push_back(gpu);
        const double target_bytes = static_cast<double>(session.capture_width) *
            session.capture_height * 4.0;
        render_target_samples.push_back(target_bytes);
#ifdef PULP_BENCHMARK
        const auto counters = session.counters.snapshot_and_reset();
        resident_samples.push_back(target_bytes +
            counters.gpu_buffer_bytes_resident_peak);
        upload_samples.push_back(counters.cpu_to_gpu_bytes_total);
#endif
    }
    if (!message.empty()) {
        (void)runtime::Tracing::stop_owned(*tracing.ownership);
        write_text(receipt_path, incomplete_json(request, message, "gpu:steady-trials"));
        if (error) *error = message;
        return 3;
    }
    const auto fidelity_frame = request.warmups + request.measured_trials + 17;
    if (!session.frame(request, fidelity_frame, true, cpu, gpu)) {
        message = "same-content reference frame did not reach the GPU readback path";
        (void)runtime::Tracing::stop_owned(*tracing.ownership);
        write_text(receipt_path, incomplete_json(request, message, "capture:reference"));
        if (error) *error = message;
        return 3;
    }
    const auto reference_rgba = session.latest_rgba;
    const auto reference_width = session.capture_width;
    const auto reference_height = session.capture_height;
    if (!session.frame(request, fidelity_frame, true, cpu, gpu)) {
        message = "same-content comparison frame did not reach the GPU readback path";
        (void)runtime::Tracing::stop_owned(*tracing.ownership);
        write_text(receipt_path, incomplete_json(request, message, "capture:final"));
        if (error) *error = message;
        return 3;
    }
    const auto stopped = runtime::Tracing::stop_owned(*tracing.ownership);
    if (!stopped.ok || stopped.trace_bytes == 0) {
        message = "Perfetto trace did not flush";
        write_text(receipt_path, incomplete_json(request, message, "trace:flush"));
        if (error) *error = message;
        return 3;
    }

    render::HeadlessSurface::Rgba rgba{session.latest_rgba,
                                       session.capture_width,
                                       session.capture_height};
    const auto png = render::HeadlessSurface::encode_png(rgba, &message);
    render::HeadlessSurface::Rgba reference_image{
        reference_rgba, reference_width, reference_height};
    const auto reference_png = render::HeadlessSurface::encode_png(
        reference_image, &message);
    const auto capture_path = cell / ("capture-" + request.attempt_nonce + ".png");
    const auto reference_path = cell /
        ("reference-" + request.attempt_nonce + ".png");
    if (png.empty() || reference_png.empty() || !write_bytes(capture_path, png) ||
        !write_bytes(reference_path, reference_png)) {
        message = "captured RGBA could not be encoded";
        write_text(receipt_path, incomplete_json(request, message, "capture:png"));
        if (error) *error = message;
        return 3;
    }
    const auto content = view::analyze_screenshot_content(png);
    const auto comparison = view::compare_screenshots(reference_png, png, 0);
    if (!comparison.valid) {
        message = "same-content fidelity comparison could not decode its captures";
        write_text(receipt_path, incomplete_json(request, message, "capture:comparison"));
        if (error) *error = message;
        return 3;
    }

    auto metrics = choc::value::createObject("");
    const auto put_samples = [&](const char* name, const char* provenance,
                                 const char* definition,
                                 const std::vector<double>& values) {
        auto array = choc::value::createEmptyArray();
        for (double value : values) array.addArrayElement(value);
        auto metric = choc::value::createObject("");
        metric.setMember("provenance", provenance);
        metric.setMember("definition", definition);
        metric.setMember("samples", std::move(array));
        metrics.setMember(name, std::move(metric));
    };
    put_samples("cpu_frame_time", "measured",
                "steady-frame CPU submit latency", cpu_samples);
    put_samples("gpu_frame_time", "measured",
                "Dawn GPU timestamp elapsed time", gpu_samples);
    // These samples are copied from the independently launched, identity-bound
    // child ledger below; outer validation checks the one-to-one correspondence.
    put_samples("first_frame_time", "measured",
                "fresh process launch through first GPU readback", first_frame_samples);
    put_samples("interaction_latency", "measured",
                "pointer dispatch through rendered GPU output", interaction_samples);
    put_samples("render_target_bytes", "derived",
                "RGBA8 physical capture width times height times four",
                render_target_samples);
    put_samples("resident_bytes", "derived",
                "derived render target plus observed peak GPU buffer residency",
                resident_samples);
    put_samples("upload_bytes", "measured",
                "instrumented CPU-to-GPU bytes for the measured frame",
                upload_samples);

    auto raw = choc::value::createObject("");
    raw.setMember("schema", "pulp.gpu-dpr-raw-samples.v1");
    raw.setMember("version", 1);
    raw.setMember("producer_pid", static_cast<std::int64_t>(getpid()));
    raw.setMember("metrics", std::move(metrics));
    auto calibration = choc::value::createObject("");
    calibration.setMember("schema", "pulp.gpu-dpr-timer-calibration.v1");
    calibration.setMember("version", 1);
    calibration.setMember("clock", "dawn-gpu-timestamp");
    calibration.setMember("resolution_ms", timer_resolution);
    auto baseline_values = choc::value::createEmptyArray();
    for (double value : calibration_baseline) baseline_values.addArrayElement(value);
    calibration.setMember("baseline_samples_ms", std::move(baseline_values));
    auto extra_values = choc::value::createEmptyArray();
    for (double value : calibration_extra) extra_values.addArrayElement(value);
    calibration.setMember("extra_work_samples_ms", std::move(extra_values));
    calibration.setMember("extra_work_multiplier", static_cast<std::int64_t>(
        request.gpu_timer_extra_work_multiplier));
    calibration.setMember("control_detected", timer_control_detected);
    raw.setMember("gpu_timer_calibration", std::move(calibration));
    raw.setMember("fresh_process_trials", std::move(first_frame_ledger));
    auto trials = choc::value::createEmptyArray();
    auto input = choc::value::createObject("");
    auto expected_logical = choc::value::createEmptyArray();
    expected_logical.addArrayElement(request.logical_input_x);
    expected_logical.addArrayElement(request.logical_input_y);
    input.setMember("expected_logical", std::move(expected_logical));
    auto observed_logical = choc::value::createEmptyArray();
    observed_logical.addArrayElement(last_observed_logical.x);
    observed_logical.addArrayElement(last_observed_logical.y);
    input.setMember("observed_logical", std::move(observed_logical));
    auto requested_physical = choc::value::createEmptyArray();
    requested_physical.addArrayElement(request.logical_input_x * effective_dpr(request));
    requested_physical.addArrayElement(request.logical_input_y * effective_dpr(request));
    input.setMember("requested_physical", std::move(requested_physical));
    auto observed_physical = choc::value::createEmptyArray();
    observed_physical.addArrayElement(last_physical.x);
    observed_physical.addArrayElement(last_physical.y);
    input.setMember("observed_physical", std::move(observed_physical));
    input.setMember("expected_target", request.logical_input_target);
    input.setMember("observed_target", last_observed_target);
    input.setMember("event_received", input_event_received);
    trials.addArrayElement(std::move(input));
    raw.setMember("logical_input_trials", std::move(trials));
    auto fidelity = choc::value::createObject("");
    fidelity.setMember("content_floor_passed", content.passes_content_floor());
    fidelity.setMember("capture_similarity", comparison.similarity);
    fidelity.setMember("small_text_luminance_stddev", content.luminance_stddev);
    fidelity.setMember("thin_stroke_coverage", content.non_background_coverage);
    auto comparison_identity = choc::value::createObject("");
    comparison_identity.setMember("method", "pulp-png-pixel-comparison");
    comparison_identity.setMember("reference_sha256",
        runtime::sha256_file_hex(reference_path, 128ull * 1024ull * 1024ull).value_or(""));
    comparison_identity.setMember("capture_sha256",
        runtime::sha256_file_hex(capture_path, 128ull * 1024ull * 1024ull).value_or(""));
    comparison_identity.setMember("same_content_token",
        request.expected_content_digest + ":frame=" + std::to_string(fidelity_frame));
    fidelity.setMember("comparison", std::move(comparison_identity));
    raw.setMember("fidelity", std::move(fidelity));
    auto trace = choc::value::createObject("");
    trace.setMember("complete", true);
    raw.setMember("trace", std::move(trace));
    if (request.mode == "adaptive_simulation") {
        auto profile = choc::json::parse(request.adaptive_profile_json);
        auto policy = choc::value::createObject("");
        policy.setMember("profile", std::move(profile));
        policy.setMember("budget_ms", adaptive_budget);
        policy.setMember("initial_scale", adaptive_initial_scale);
        policy.setMember("final_scale", adaptive_final_scale);
        auto observations = choc::value::createEmptyArray();
        for (const auto& item : adaptive_observations) {
            auto observation = choc::value::createObject("");
            observation.setMember("phase", item.phase);
            observation.setMember("frame_index",
                                  static_cast<std::int64_t>(item.frame_index));
            observation.setMember("sample_ms", item.sample_ms);
            observation.setMember("scale_before", item.scale_before);
            observation.setMember("scale_after", item.scale_after);
            if (item.transition.empty()) observation.setMember("transition", false);
            else observation.setMember("transition", item.transition);
            observations.addArrayElement(std::move(observation));
        }
        policy.setMember("observations", std::move(observations));
        raw.setMember("adaptive_policy_evidence", std::move(policy));
    }
    const auto raw_path = cell / ("raw-samples-" + request.attempt_nonce + ".json");
    const auto input_path = cell / ("input-receipt-" + request.attempt_nonce + ".json");
    auto input_receipt = choc::value::createObject("");
    input_receipt.setMember("schema", "pulp.gpu-dpr-input-receipt.v1");
    input_receipt.setMember("attempt_nonce", request.attempt_nonce);
    input_receipt.setMember("same_process", true);
    if (!write_text(raw_path, choc::json::toString(raw, true) + "\n") ||
        !write_text(input_path, choc::json::toString(input_receipt, true) + "\n")) {
        message = "raw sample or input artifact could not be written";
        write_text(receipt_path, incomplete_json(request, message, "artifact:write"));
        if (error) *error = message;
        return 3;
    }

    auto artifacts = choc::value::createEmptyArray();
    const auto add_artifact = [&](const char* kind, const std::filesystem::path& path) {
        auto artifact = choc::value::createObject("");
        artifact.setMember("kind", kind);
        artifact.setMember("path", path.filename().string());
        const auto digest = runtime::sha256_file_hex(
            path, kind == std::string_view("trace") ? 512ull * 1024ull * 1024ull
                                                     : 128ull * 1024ull * 1024ull);
        artifact.setMember("sha256", digest.value_or(""));
        artifacts.addArrayElement(std::move(artifact));
        return digest.has_value();
    };
    if (!add_artifact("capture", capture_path) ||
        !add_artifact("reference_capture", reference_path) ||
        !add_artifact("trace", trace_path) ||
        !add_artifact("raw_samples", raw_path) ||
        !add_artifact("input_receipt", input_path)) {
        message = "evidence artifact digest is unavailable";
        write_text(receipt_path, incomplete_json(request, message, "artifact:digest"));
        if (error) *error = message;
        return 3;
    }

    auto same_process = choc::value::createObject("");
    for (const char* field : {"adapter_identity", "capture", "frame_metrics",
                              "memory_metrics", "logical_input", "trace_correlation"})
        same_process.setMember(field, true);
    auto scope = choc::value::createObject("");
    scope.setMember("schema", "pulp.gpu-dpr-native-measurement-scope.v1");
    scope.setMember("same_process", std::move(same_process));
    scope.setMember("audio_device_opened", false);
    auto build = choc::value::createObject("");
    build.setMember("pulp_sha", request.pulp_sha);
    auto receipt = choc::value::createObject("");
    receipt.setMember("schema", std::string(kDprCellReceiptSchema));
    receipt.setMember("version", 1);
    receipt.setMember("attempt_nonce", request.attempt_nonce);
    receipt.setMember("attempt_number", static_cast<std::int64_t>(request.attempt_number));
    receipt.setMember("scenario_id", request.scenario_id);
    receipt.setMember("scenario_kind", request.scenario_kind);
    receipt.setMember("mode", request.mode);
    receipt.setMember("requested_dpr", request.requested_dpr);
    const bool fidelity_passed = content.passes_content_floor() &&
        comparison.similarity >= 0.99f && content.luminance_stddev >= 1.0 &&
        content.non_background_coverage >= 0.001;
    receipt.setMember("outcome", fidelity_passed
        ? "pass" : "fail");
    receipt.setMember("observed_dpr", effective_dpr(request));
    auto physical = choc::value::createObject("");
    physical.setMember("width", static_cast<std::int64_t>(session.capture_width));
    physical.setMember("height", static_cast<std::int64_t>(session.capture_height));
    receipt.setMember("physical_size", std::move(physical));
    receipt.setMember("content_digest", request.expected_content_digest);
    receipt.setMember("machine", machine_json());
    receipt.setMember("adapter", adapter_json(session.adapter));
    receipt.setMember("build_identity", std::move(build));
    receipt.setMember("measurement_scope", std::move(scope));
    if (request.scenario_id == "threejs-audio-reactive")
        receipt.setMember("audio_thread_excluded", true);
    receipt.setMember("artifacts", std::move(artifacts));
    if (!write_text(receipt_path, choc::json::toString(receipt, true) + "\n")) {
        if (error) *error = "terminal DPR receipt could not be written";
        return 3;
    }
    return fidelity_passed ? 0 : 1;
#endif
}

} // namespace pulp::tooling::gpu_probe
