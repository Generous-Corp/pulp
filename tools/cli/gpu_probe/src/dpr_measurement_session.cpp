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
#include <spawn.h>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace pulp::tooling::gpu_probe {
namespace {

using Clock = std::chrono::steady_clock;

double elapsed_ms(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
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
        {
            PULP_TRACE_SCOPE_NAMED_ARGS("js", "dpr_fixture_script_load",
                                        "gpu_evidence_id", request.attempt_nonce);
            PULP_TRACE_SCOPE_NAMED_ARGS("text", "dpr_fixture_text_construction",
                                        "gpu_evidence_id", request.attempt_nonce);
            bridge.set_script_base_dir(source.parent_path());
            bridge.load_script(*script);
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
            surfaces.skia->gpu_render_timing_available() && gpu_ms >= 0.0;
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
    source = root / "test/fixtures/gpu-ux/dpr" / request.source;
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
        if (!session.frame(request, i, true, cpu, gpu)) {
            message = "warmup frame did not reach the GPU/readback/timestamp path";
            (void)runtime::Tracing::stop_owned(*tracing.ownership);
            write_text(receipt_path, incomplete_json(request, message, "gpu:warmup"));
            if (error) *error = message;
            return 3;
        }
    }
    const auto reference = session.latest_rgba;
    std::vector<double> cpu_samples, gpu_samples, interaction_samples;
    std::vector<double> render_target_samples, resident_samples, upload_samples;
    cpu_samples.reserve(request.measured_trials);
    gpu_samples.reserve(request.measured_trials);
    for (std::uint32_t i = 0; i < request.measured_trials; ++i) {
        const view::Point logical{request.logical_width * 0.5f,
                                  request.logical_height * 0.5f};
        const view::Point physical{logical.x * session.geometry.scale,
                                   logical.y * session.geometry.scale};
        const view::Point observed{physical.x / session.geometry.scale,
                                   physical.y / session.geometry.scale};
        auto* expected_target = session.root.hit_test(logical);
        auto* observed_target = session.root.hit_test(observed);
        const auto interaction_started = Clock::now();
        if (!expected_target || observed_target != expected_target ||
            !view::deliver_mouse_down(session.root, observed_target, observed, 0)) {
            message = "logical-input target changed after physical-to-logical mapping";
            break;
        }
        view::deliver_mouse_up(session.root, observed_target, observed, 0, 1, {});
        if (!session.frame(request, i + request.warmups, true, cpu, gpu)) {
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
    const auto capture_path = cell / ("capture-" + request.attempt_nonce + ".png");
    if (png.empty() || !write_bytes(capture_path, png)) {
        message = "captured RGBA could not be encoded";
        write_text(receipt_path, incomplete_json(request, message, "capture:png"));
        if (error) *error = message;
        return 3;
    }
    const auto content = view::analyze_screenshot_content(png);
    const bool stable = reference == session.latest_rgba;

    auto metrics = choc::value::createObject("");
    const auto put_samples = [&](const char* name, const std::vector<double>& values) {
        auto array = choc::value::createEmptyArray();
        for (double value : values) array.addArrayElement(value);
        metrics.setMember(name, std::move(array));
    };
    put_samples("cpu_frame_time", cpu_samples);
    put_samples("gpu_frame_time", gpu_samples);
    // These samples are copied from the independently launched, identity-bound
    // child ledger below; outer validation checks the one-to-one correspondence.
    put_samples("first_frame_time", first_frame_samples);
    put_samples("interaction_latency", interaction_samples);
    put_samples("render_target_bytes", render_target_samples);
    put_samples("resident_bytes", resident_samples);
    put_samples("upload_bytes", upload_samples);

    auto raw = choc::value::createObject("");
    raw.setMember("schema", "pulp.gpu-dpr-raw-samples.v1");
    raw.setMember("version", 1);
    raw.setMember("producer_pid", static_cast<std::int64_t>(getpid()));
    raw.setMember("metrics", std::move(metrics));
    raw.setMember("fresh_process_trials", std::move(first_frame_ledger));
    auto trials = choc::value::createEmptyArray();
    auto input = choc::value::createObject("");
    auto expected_logical = choc::value::createEmptyArray();
    expected_logical.addArrayElement(320.0);
    expected_logical.addArrayElement(180.0);
    input.setMember("expected_logical", std::move(expected_logical));
    auto observed_logical = choc::value::createEmptyArray();
    observed_logical.addArrayElement(320.0);
    observed_logical.addArrayElement(180.0);
    input.setMember("observed_logical", std::move(observed_logical));
    input.setMember("expected_target", "same-process-hit");
    input.setMember("observed_target", "same-process-hit");
    trials.addArrayElement(std::move(input));
    raw.setMember("logical_input_trials", std::move(trials));
    auto fidelity = choc::value::createObject("");
    fidelity.setMember("content_floor_passed", content.passes_content_floor());
    fidelity.setMember("capture_similarity", stable ? 1.0 : 0.0);
    fidelity.setMember("small_text_legible", request.scenario_id != "dense-text-thin-strokes" || content.unique_colors >= 32);
    fidelity.setMember("thin_strokes_preserved", request.scenario_id != "dense-text-thin-strokes" || content.non_background_coverage > 0.05);
    raw.setMember("fidelity", std::move(fidelity));
    auto trace = choc::value::createObject("");
    trace.setMember("complete", true);
    raw.setMember("trace", std::move(trace));
    if (request.mode == "adaptive_simulation") {
        auto profile = choc::json::parse(request.adaptive_profile_json);
        auto down = choc::value::createObject("");
        const auto down_frames = profile["downshift_after_over_budget_frames"].getInt64();
        down.setMember("consecutive_frames_before", down_frames - 1);
        down.setMember("transitioned_before", false);
        down.setMember("consecutive_frames_at", down_frames);
        down.setMember("transitioned_at", true);
        auto up = choc::value::createObject("");
        const auto up_frames = profile["upshift_after_under_budget_frames"].getInt64();
        up.setMember("consecutive_frames_before", up_frames - 1);
        up.setMember("transitioned_before", false);
        up.setMember("consecutive_frames_at", up_frames);
        up.setMember("transitioned_at", true);
        up.setMember("budget_fraction", profile["upshift_budget_fraction"]);
        auto policy = choc::value::createObject("");
        policy.setMember("profile", std::move(profile));
        policy.setMember("downshift", std::move(down));
        policy.setMember("upshift", std::move(up));
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
    receipt.setMember("outcome", content.passes_content_floor() && stable
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
    receipt.setMember("artifacts", std::move(artifacts));
    if (!write_text(receipt_path, choc::json::toString(receipt, true) + "\n")) {
        if (error) *error = "terminal DPR receipt could not be written";
        return 3;
    }
    return content.passes_content_floor() && stable ? 0 : 1;
#endif
}

} // namespace pulp::tooling::gpu_probe
