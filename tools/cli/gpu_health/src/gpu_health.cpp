#include <pulp_tooling/gpu_health/health_provider.hpp>

#include <pulp/render/gpu_compute.hpp>
#include <pulp/render/headless_surface.hpp>
#if defined(PULP_ENABLE_SCENE3D)
#include <pulp/render/renderer3d.hpp>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace pulp::tooling::gpu_health {
namespace {

std::string measurement_time_utc() {
    const auto now = std::chrono::system_clock::now();
    const auto value = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &value);
#else
    gmtime_r(&value, &utc);
#endif
    std::ostringstream out;
    out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

EvidenceEvent event(Stage stage, Verdict verdict, std::string code,
                    std::string detail) {
    if (detail.size() > 1024) {
        detail.resize(1021);
        detail += "...";
    }
    return EvidenceEvent{ 0, stage, verdict, std::move(code), std::move(detail) };
}

std::optional<std::string> bounded_identity(std::string_view value) {
    if (value.empty()) return std::nullopt;
    constexpr std::size_t limit = 256;
    return std::string(value.substr(0, limit));
}

#if defined(PULP_ENABLE_SCENE3D)
AdapterIdentity renderer_identity(const render::Scene3DRenderResult& result) {
    AdapterIdentity identity;
    identity.status = result.adapter_info_available
        ? IdentityStatus::unverified
        : IdentityStatus::unavailable;
    identity.classification = AdapterClass::unknown;
    if (result.null_backend_requested || result.adapter_backend_type == "Null")
        identity.classification = AdapterClass::null_adapter;
    identity.backend = bounded_identity(result.adapter_backend);
    identity.name = bounded_identity(result.adapter_name);
    identity.vendor = bounded_identity(result.adapter_vendor);
    identity.architecture = bounded_identity(result.adapter_architecture);
    return identity;
}
#endif

AdapterClass compute_adapter_class(std::string_view adapter_type) {
    if (adapter_type == "discrete-gpu" || adapter_type == "integrated-gpu")
        return AdapterClass::hardware;
    if (adapter_type == "cpu") return AdapterClass::software;
    return AdapterClass::unknown;
}

std::optional<std::string> numeric_device_identity(
    const render::GpuCompute::CapabilityReport& capabilities) {
    if (capabilities.vendor_id == 0 && capabilities.device_id == 0)
        return std::nullopt;
    std::ostringstream out;
    out << "vendor=0x" << std::hex << capabilities.vendor_id
        << ",device=0x" << capabilities.device_id;
    return out.str();
}

AdapterIdentity compute_identity(
    const render::GpuCompute::CapabilityReport& capabilities) {
    AdapterIdentity identity;
    identity.status = capabilities.adapter_info_authentic
        ? IdentityStatus::authentic
        : (capabilities.available ? IdentityStatus::unverified
                                  : IdentityStatus::unavailable);
    identity.classification = capabilities.adapter_info_authentic
        ? compute_adapter_class(capabilities.adapter_type)
        : AdapterClass::unknown;
    identity.backend = bounded_identity(capabilities.backend);
    identity.name = bounded_identity(capabilities.name);
    identity.vendor = bounded_identity(capabilities.vendor);
    identity.architecture = bounded_identity(capabilities.architecture);
    identity.device = numeric_device_identity(capabilities);
    return identity;
}

#if defined(PULP_ENABLE_SCENE3D)
std::string renderer_failure_detail(const render::Scene3DRenderResult& result) {
    if (!result.error.empty()) return result.error;
    return "Renderer3D did not complete the bounded render/readback probe";
}
#endif

class DefaultHealthProvider final : public HealthProvider {
public:
    explicit DefaultHealthProvider(HealthProbeOptions options)
        : options_(options) {}

    ProbeEvidence probe_renderer3d() override {
        ProbeEvidence probe;
        probe.probe_id = "renderer3d";

#if !defined(PULP_ENABLE_SCENE3D)
        probe.required = false;
        probe.adapter.status = IdentityStatus::unavailable;
        probe.adapter.classification = AdapterClass::unknown;
        probe.verdict = Verdict::unavailable;
        probe.events.push_back(event(Stage::configuration, Verdict::unavailable,
            "renderer3d_not_compiled",
            "Renderer3D support is not compiled into this Pulp build"));
        return probe;
#else
        render::HardcodedCubeRenderConfig config;
        config.width = 64;
        config.height = 64;
        const auto result = render::Renderer3D::render_hardcoded_textured_cube(config);
        probe.adapter = renderer_identity(result);
        probe.measurements.command_submitted = result.command_submitted;
        probe.measurements.readback_completed = result.readback_completed;
        probe.measurements.pixel_output_produced =
            result.non_transparent_pixel_count > 0;
        probe.measurements.content_floor_passed =
            result.non_transparent_pixel_count > 0 && result.distinct_color_count > 1;
        if (result.non_transparent_pixel_count > 0) {
            probe.measurements.non_transparent_pixel_count =
                result.non_transparent_pixel_count;
            probe.measurements.distinct_color_count = result.distinct_color_count;
        }

        if (result.success) {
            probe.verdict = Verdict::pass;
            probe.events.push_back(event(Stage::render, Verdict::pass,
                "renderer3d_render_completed",
                "Renderer3D completed a bounded Dawn render"));
            probe.events.push_back(event(Stage::submit, Verdict::pass,
                "renderer3d_submit_completed",
                "Renderer3D submitted GPU commands"));
            probe.events.push_back(event(Stage::readback, Verdict::pass,
                "renderer3d_readback_completed",
                "Renderer3D read the rendered frame back to CPU memory"));
            probe.events.push_back(event(Stage::content, Verdict::pass,
                "renderer3d_content_floor_passed",
                "The frame contains non-transparent pixels and multiple colors"));
            return probe;
        }

        const auto detail = renderer_failure_detail(result);
        if (!result.gpu_available) {
            probe.verdict = Verdict::unavailable;
            probe.events.push_back(event(Stage::adapter, Verdict::unavailable,
                "renderer3d_adapter_unavailable", detail));
        } else if (!result.command_submitted) {
            probe.verdict = Verdict::fail;
            probe.events.push_back(event(Stage::pipeline_create, Verdict::fail,
                "renderer3d_setup_failed", detail));
        } else if (!result.readback_completed) {
            probe.verdict = Verdict::fail;
            probe.events.push_back(event(Stage::readback, Verdict::fail,
                "renderer3d_readback_failed", detail));
        } else {
            probe.verdict = Verdict::fail;
            probe.events.push_back(event(Stage::content, Verdict::fail,
                "renderer3d_blank_output", detail));
        }
        return probe;
#endif
    }

    ProbeEvidence probe_headless_surface() override {
        ProbeEvidence probe;
        probe.probe_id = "skia-graphite-headless";
        probe.adapter.status = IdentityStatus::unavailable;
        probe.adapter.classification = AdapterClass::unknown;

        render::HeadlessSurface::Config config;
        config.width = 64;
        config.height = 64;
        config.clear_r = 19;
        config.clear_g = 113;
        config.clear_b = 197;
        config.clear_a = 255;

        std::string create_error;
        auto surface = render::HeadlessSurface::create(config, &create_error);
        if (!surface || !surface->is_ready()) {
            probe.verdict = Verdict::unavailable;
            probe.events.push_back(event(Stage::configuration, Verdict::unavailable,
                "skia_graphite_unavailable",
                create_error.empty() ? "The headless Skia/Graphite surface is unavailable"
                                     : create_error));
            return probe;
        }

        auto rgba = surface->render_rgba(nullptr);
        if (rgba.empty()) {
            probe.verdict = Verdict::fail;
            probe.measurements.pixel_output_produced = false;
            probe.measurements.content_floor_passed = false;
            probe.events.push_back(event(Stage::render, Verdict::fail,
                "skia_graphite_frame_failed",
                surface->last_error().empty()
                    ? "The headless Skia/Graphite frame did not return pixels; the existing API cannot distinguish a pre-submit failure from readback failure"
                    : surface->last_error()));
            return probe;
        }

        probe.measurements.command_submitted = true;
        probe.measurements.readback_completed = true;

        std::uint64_t non_transparent = 0;
        bool expected_clear = rgba.pixels.size()
            == static_cast<std::size_t>(config.width) * config.height * 4;
        for (std::size_t i = 0; i + 3 < rgba.pixels.size(); i += 4) {
            if (rgba.pixels[i + 3] != 0) ++non_transparent;
            expected_clear = expected_clear
                && rgba.pixels[i] == config.clear_r
                && rgba.pixels[i + 1] == config.clear_g
                && rgba.pixels[i + 2] == config.clear_b
                && rgba.pixels[i + 3] == config.clear_a;
        }
        if (options_.seed_headless_content_mismatch)
            expected_clear = false;

        probe.measurements.pixel_output_produced = non_transparent > 0;
        probe.measurements.content_floor_passed = expected_clear;
        probe.measurements.non_transparent_pixel_count = non_transparent;
        probe.measurements.distinct_color_count = 1;
        probe.measurements.rgba_fingerprint =
            render::HeadlessSurface::rgba_fingerprint(rgba)
            & static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());

        if (!expected_clear) {
            probe.verdict = Verdict::fail;
            probe.events.push_back(event(Stage::content, Verdict::fail,
                "skia_graphite_content_mismatch",
                "The readback did not match the deterministic clear color"));
            return probe;
        }

        probe.verdict = Verdict::pass;
        probe.events.push_back(event(Stage::render, Verdict::pass,
            "skia_graphite_render_completed",
            "HeadlessSurface completed a bounded Skia/Graphite frame"));
        probe.events.push_back(event(Stage::readback, Verdict::pass,
            "skia_graphite_readback_completed",
            "HeadlessSurface read the frame back to CPU memory"));
        probe.events.push_back(event(Stage::content, Verdict::pass,
            "skia_graphite_content_floor_passed",
            "The frame matches the deterministic clear-color oracle"));
        return probe;
    }

    ProbeEvidence probe_compute() override {
        ProbeEvidence probe;
        probe.probe_id = "gpu-compute-magnitude";
        probe.adapter.status = IdentityStatus::unavailable;
        probe.adapter.classification = AdapterClass::unknown;

#if !defined(PULP_HAS_DAWN)
        probe.verdict = Verdict::unavailable;
        probe.measurements.compute_initialized = false;
        probe.events.push_back(event(Stage::configuration, Verdict::unavailable,
            "gpu_compute_not_built",
            "This Pulp build does not include Dawn GPU compute"));
        return probe;
#else
        auto compute = render::GpuCompute::create();
        if (!compute || !compute->initialize_standalone()) {
            probe.verdict = Verdict::unavailable;
            probe.measurements.compute_initialized = false;
            probe.events.push_back(event(Stage::adapter, Verdict::unavailable,
                "gpu_compute_initialization_unavailable",
                "GpuCompute could not acquire and initialize a standalone device"));
            return probe;
        }

        const auto capabilities = compute->capabilities();
        probe.adapter = compute_identity(capabilities);
        probe.measurements.compute_initialized = true;
        probe.measurements.device_lost = compute->device_lost();
        if (compute->device_lost()) {
            probe.verdict = Verdict::fail;
            probe.events.push_back(event(Stage::device_state, Verdict::fail,
                "gpu_compute_device_lost",
                "The compute device was lost before the oracle ran"));
            return probe;
        }

        constexpr std::uint32_t bins = 64;
        std::array<float, bins * 2> input{};
        std::array<float, bins> output{};
        for (std::uint32_t i = 0; i < bins; ++i) {
            input[i * 2] = 3.0f;
            input[i * 2 + 1] = 4.0f;
        }
        const bool dispatched =
            compute->compute_magnitude(input.data(), output.data(), bins);
        const bool oracle_passed = dispatched && std::all_of(
            output.begin(), output.end(), [](float value) {
                return std::abs(value - 5.0f) < 1.0e-4f;
            });
        probe.measurements.compute_oracle_passed = oracle_passed;
        probe.measurements.device_lost = compute->device_lost();

        if (!dispatched || compute->device_lost()) {
            probe.verdict = Verdict::fail;
            probe.events.push_back(event(
                compute->device_lost() ? Stage::device_state : Stage::compute,
                Verdict::fail,
                compute->device_lost() ? "gpu_compute_device_lost"
                                       : "gpu_compute_execution_failed",
                compute->device_lost()
                    ? "The compute device was lost during the oracle"
                    : "The magnitude dispatch or blocking readback failed"));
            return probe;
        }
        if (!oracle_passed) {
            probe.verdict = Verdict::fail;
            probe.events.push_back(event(Stage::compute, Verdict::fail,
                "gpu_compute_oracle_mismatch",
                "The GPU magnitude output did not match the independent 3-4-5 oracle"));
            return probe;
        }

        const bool authentic_identity =
            probe.adapter.status == IdentityStatus::authentic;
        probe.verdict = authentic_identity ? Verdict::pass : Verdict::unverified;
        probe.events.push_back(event(Stage::adapter,
            authentic_identity ? Verdict::pass : Verdict::unverified,
            authentic_identity ? "gpu_compute_adapter_acquired"
                               : "gpu_compute_adapter_identity_unverified",
            authentic_identity
                ? "GpuCompute acquired a standalone Dawn adapter and device with authentic GetInfo identity"
                : "GpuCompute completed work, but Dawn did not provide authentic adapter identity"));
        probe.events.push_back(event(Stage::compute, Verdict::pass,
            "gpu_compute_oracle_passed",
            "The GPU magnitude output matched the independent 3-4-5 oracle"));
        return probe;
#endif
    }

private:
    HealthProbeOptions options_;
};

int verdict_rank(Verdict verdict) {
    switch (verdict) {
        case Verdict::fail: return 3;
        case Verdict::unavailable: return 2;
        case Verdict::unverified: return 1;
        case Verdict::pass: return 0;
    }
    return 3;
}

void add_recommendation(HealthResult& result, std::string_view code) {
    std::string recommendation;
    if (code == "renderer3d_not_compiled") {
        recommendation =
            "Reconfigure Pulp with -DPULP_ENABLE_SCENE3D=ON to include the direct-Dawn Renderer3D probe.";
    } else if (code.find("adapter_unavailable") != std::string_view::npos
        || code.find("initialization_unavailable") != std::string_view::npos
        || code == "skia_graphite_unavailable") {
        recommendation =
            "Verify the installed Pulp GPU build, adapter availability, and Dawn/Skia runtime libraries.";
    } else if (code == "renderer3d_setup_failed") {
        recommendation =
            "Enable Dawn validation logging and inspect the first pipeline or device error.";
    } else if (code.find("readback") != std::string_view::npos) {
        recommendation =
            "Verify that the selected adapter supports bounded texture and buffer readback.";
    } else if (code.find("content") != std::string_view::npos
               || code.find("blank_output") != std::string_view::npos) {
        recommendation =
            "Inspect render-target clears, pass inputs, and final content before presentation.";
    } else if (code.find("device_lost") != std::string_view::npos) {
        recommendation =
            "Recreate the device and dependent GPU resources, then rerun the diagnostic.";
    } else if (code.find("oracle") != std::string_view::npos
               || code.find("dispatch") != std::string_view::npos) {
        recommendation =
            "Compare the compute inputs and output buffer against the independent CPU oracle.";
    }
    if (!recommendation.empty()
        && std::find(result.recommendations.begin(), result.recommendations.end(),
                     recommendation) == result.recommendations.end())
        result.recommendations.push_back(std::move(recommendation));
}

} // namespace

std::unique_ptr<HealthProvider> make_default_health_provider(
    HealthProbeOptions options) {
    return std::make_unique<DefaultHealthProvider>(options);
}

HealthResult run_health_check(HealthProvider& provider, bool render_requested) {
    HealthResult result;
    result.run_id = "pulp-doctor-gpu-v1";
    result.measured_at_utc = measurement_time_utc();
    result.render_requested = render_requested;

    if (!render_requested) {
        ProbeEvidence probe;
        probe.probe_id = "gpu-build-preflight";
        probe.verdict = Verdict::unverified;
        probe.adapter.status = IdentityStatus::unavailable;
        probe.adapter.classification = AdapterClass::unknown;
        probe.events.push_back(event(Stage::configuration, Verdict::unverified,
            "render_not_requested",
            "GPU device acquisition, rendering, compute, and readback were not requested"));
        result.probes.push_back(std::move(probe));
        result.verdict = Verdict::unverified;
        result.health_state = HealthState::unverified;
    } else {
        result.probes.push_back(provider.probe_renderer3d());
        result.probes.push_back(provider.probe_headless_surface());
        result.probes.push_back(provider.probe_compute());

        result.verdict = Verdict::pass;
        for (const auto& probe : result.probes) {
            if (!probe.required) continue;
            if (verdict_rank(probe.verdict) > verdict_rank(result.verdict))
                result.verdict = probe.verdict;
        }
        bool lost = false;
        for (const auto& probe : result.probes)
            lost = lost || (probe.required &&
                            probe.measurements.device_lost.value_or(false));
        if (lost) result.health_state = HealthState::lost;
        else if (result.verdict == Verdict::pass) result.health_state = HealthState::healthy;
        else if (result.verdict == Verdict::fail) result.health_state = HealthState::failed;
        else if (result.verdict == Verdict::unavailable)
            result.health_state = HealthState::unavailable;
        else result.health_state = HealthState::unverified;
    }

    std::uint32_t sequence = 0;
    for (auto& probe : result.probes) {
        for (auto& evidence : probe.events) {
            evidence.sequence = sequence++;
            if (evidence.verdict != Verdict::pass)
                add_recommendation(result, evidence.code);
        }
    }
    return result;
}

int exit_code(const HealthResult& result) {
    switch (result.verdict) {
        case Verdict::pass: return 0;
        case Verdict::fail: return 1;
        case Verdict::unavailable:
        case Verdict::unverified: return 2;
    }
    return 1;
}

std::string render_human(const HealthResult& result) {
    std::ostringstream out;
    out << "Pulp Doctor — GPU\n"
        << "=================\n\n"
        << "Verdict: " << to_string(result.verdict)
        << " (" << to_string(result.health_state) << ")\n";
    for (const auto& probe : result.probes) {
        out << "\n[" << to_string(probe.verdict) << "] " << probe.probe_id << "\n";
        out << "  adapter: " << to_string(probe.adapter.classification)
            << " (identity " << to_string(probe.adapter.status) << ")\n";
        if (probe.adapter.name) out << "  name: " << *probe.adapter.name << "\n";
        if (probe.adapter.backend) out << "  backend: " << *probe.adapter.backend << "\n";
        for (const auto& evidence : probe.events)
            out << "  - " << to_string(evidence.stage) << ": "
                << evidence.code << " — " << evidence.detail << "\n";
    }
    if (!result.recommendations.empty()) {
        out << "\nNext actions:\n";
        for (const auto& recommendation : result.recommendations)
            out << "  - " << recommendation << "\n";
    }
    return out.str();
}

} // namespace pulp::tooling::gpu_health
