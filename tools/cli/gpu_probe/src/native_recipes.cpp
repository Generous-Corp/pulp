#include <pulp_tooling/gpu_probe/recipes.hpp>

#include <pulp/gpu_audio/gpu_stft.hpp>
#include <pulp/render/gpu_compute.hpp>
#include <pulp/render/headless_surface.hpp>
#include <pulp/runtime/crypto.hpp>
#include <pulp/signal/fft.hpp>
#include <pulp/signal/rng.hpp>
#include "renderer3d_oracle.hpp"
#if defined(PULP_ENABLE_SCENE3D)
#include <pulp/render/renderer3d.hpp>
#endif
#if PULP_GPU_PROBE_HAS_THREEJS && PULP_GPU_PROBE_THREEJS_CALLABLE
#include <pulp/render/gpu_surface.hpp>
#include <pulp/render/skia_surface.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/canvas_widget.hpp>
#include <pulp/view/js_engine.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/widgets.hpp>
#include <dawn/webgpu_cpp.h>
#endif

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace pulp::tooling::gpu_probe {
namespace {

std::string digest(std::span<const std::uint8_t> bytes) {
    return runtime::sha256_hex(bytes.data(), bytes.size());
}

std::string digest(std::string_view text) {
    return runtime::sha256_hex(text);
}

std::string evidence_id(const RunOptions& options, std::string_view fallback) {
    if (options.gpu_evidence_id) return *options.gpu_evidence_id;
    if (auto bytes = runtime::secure_random_bytes(16); bytes && bytes->size() == 16)
        return runtime::hex_encode(*bytes);
    (void)fallback;
    throw std::runtime_error("GPU probe could not allocate a unique evidence identifier");
}

std::optional<std::string> bounded(std::string_view value) {
    if (value.empty()) return std::nullopt;
    return std::string(value.substr(0, 256));
}

AdapterClass adapter_class(std::string_view value) {
    if (value == "discrete-gpu" || value == "integrated-gpu")
        return AdapterClass::hardware;
    if (value == "cpu") return AdapterClass::software;
    return AdapterClass::unknown;
}

ProbeResult base_result(const RecipeDefinition& recipe, const RunOptions& options,
                        std::string_view source, bool source_is_digest = false) {
    ProbeResult result;
    result.recipe_id = std::string(recipe.id);
    result.source_digest = source_is_digest ? std::string(source) : digest(source);
    std::ostringstream signature;
    signature << recipe.id << '\n' << recipe.source_identity << '\n'
              << result.source_digest << '\n'
              << recipe.dimensions.width << 'x' << recipe.dimensions.height << ':'
              << recipe.dimensions.work_items << '\n' << recipe.seed << '\n'
              << recipe.clock << '\n' << recipe.input_format << '\n'
              << recipe.output_format << '\n' << recipe.encoding << '\n'
              << recipe.tolerance.absolute << ':' << recipe.tolerance.relative << '\n'
              << static_cast<int>(recipe.adapter_policy) << '\n'
              << recipe.bounds.max_numeric_samples << ':' << recipe.bounds.max_artifacts
              << ':' << recipe.bounds.max_artifact_bytes << ':'
              << recipe.bounds.max_total_artifact_bytes << '\n'
              << (options.apply_negative_mutation ? recipe.negative_mutation
                                                   : recipe.positive_control);
    result.signature_digest = digest(signature.str());
    result.gpu_evidence_id = evidence_id(options, result.signature_digest);
    result.dimensions = recipe.dimensions;
    result.seed = recipe.seed;
    result.clock = std::string(recipe.clock);
    result.input_format = std::string(recipe.input_format);
    result.output_format = std::string(recipe.output_format);
    result.encoding = std::string(recipe.encoding);
    result.tolerance = recipe.tolerance;
    result.adapter_policy = recipe.adapter_policy;
    if (options.apply_negative_mutation)
        result.mutation = std::string(recipe.negative_mutation);
    return result;
}

PassResult pass(std::uint32_t sequence, std::string_view name, Verdict verdict,
                bool work_completed, std::string code) {
    PassResult out;
    out.sequence = sequence;
    out.name = std::string(name);
    out.verdict = verdict;
    out.work_completed = work_completed;
    out.code = std::move(code);
    return out;
}

ArtifactPayload artifact(std::string name, ArtifactKind kind, std::string mime,
                         std::vector<std::uint8_t> bytes) {
    ArtifactPayload out;
    out.artifact.name = std::move(name);
    out.artifact.kind = kind;
    out.artifact.mime = std::move(mime);
    out.artifact.bytes = bytes.size();
    out.artifact.sha256 = digest(bytes);
    out.bytes = std::move(bytes);
    return out;
}

void attach(RecipeRun& run, ArtifactPayload payload) {
    run.result.artifacts.push_back(payload.artifact);
    run.payloads.push_back(std::move(payload));
}

void unavailable_passes(ProbeResult& result, const RecipeDefinition& recipe,
                        std::string code) {
    result.verdict = Verdict::unavailable;
    for (std::uint32_t i = 0; i < recipe.semantic_passes.size(); ++i)
        result.passes.push_back(pass(i, recipe.semantic_passes[i], Verdict::unavailable,
                                     false, code));
}

void unverified_passes(ProbeResult& result, const RecipeDefinition& recipe,
                       std::string code) {
    result.verdict = Verdict::unverified;
    for (std::uint32_t i = 0; i < recipe.semantic_passes.size(); ++i)
        result.passes.push_back(pass(i, recipe.semantic_passes[i], Verdict::unverified,
                                     false, code));
}

bool native_recipe_test_fault(std::string_view name) {
#if PULP_GPU_PROBE_NATIVE_TEST_FAULTS
    const auto* fault = std::getenv("PULP_GPU_PROBE_NATIVE_TEST_FAULT");
    return fault != nullptr && name == fault;
#else
    (void)name;
    return false;
#endif
}

std::vector<std::uint8_t> floats_as_bytes(std::span<const float> values) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(values.size_bytes());
    for (const float value : values) {
        const auto bits = std::bit_cast<std::uint32_t>(value);
        bytes.push_back(static_cast<std::uint8_t>(bits));
        bytes.push_back(static_cast<std::uint8_t>(bits >> 8u));
        bytes.push_back(static_cast<std::uint8_t>(bits >> 16u));
        bytes.push_back(static_cast<std::uint8_t>(bits >> 24u));
    }
    return bytes;
}

bool replace_once(std::string& value, std::string_view from, std::string_view to) {
    const auto at = value.find(from);
    if (at == std::string::npos || value.find(from, at + from.size()) != std::string::npos)
        return false;
    value.replace(at, from.size(), to);
    return true;
}

void populate_adapter_identity(ProbeResult& result,
                               const render::GpuCompute::CapabilityReport& capabilities) {
    result.adapter.status = capabilities.adapter_info_authentic
        ? IdentityStatus::authentic
        : (capabilities.available ? IdentityStatus::unverified
                                  : IdentityStatus::unavailable);
    result.adapter.classification = capabilities.adapter_info_authentic
        ? adapter_class(capabilities.adapter_type) : AdapterClass::unknown;
    result.adapter.backend = bounded(capabilities.backend);
    result.adapter.name = bounded(capabilities.name);
    result.adapter.vendor = bounded(capabilities.vendor);
    result.adapter.architecture = bounded(capabilities.architecture);
    if (capabilities.vendor_id != 0 || capabilities.device_id != 0) {
        std::ostringstream id;
        id << "vendor=0x" << std::hex << capabilities.vendor_id
           << ",device=0x" << capabilities.device_id;
        result.adapter.device = id.str();
    }
}

bool install_stft_mutation(render::GpuCompute& gpu, std::string& actual_source) {
    std::string mutated = actual_source;
    const bool complete =
        replace_once(mutated,
            "dst[2u * (base + idxD)]                   = y0_re;",
            "dst[2u * (base + idxD)]                   = 0.5 * y0_re;") &&
        replace_once(mutated,
            "dst[2u * (base + idxD) + 1u]              = y0_im;",
            "dst[2u * (base + idxD) + 1u]              = 0.5 * y0_im;") &&
        replace_once(mutated,
            "dst[2u * (base + idxD + params.ns)]       = y1_re;",
            "dst[2u * (base + idxD + params.ns)]       = 0.5 * y1_re;") &&
        replace_once(mutated,
            "dst[2u * (base + idxD + params.ns) + 1u]  = y1_im;",
            "dst[2u * (base + idxD + params.ns) + 1u]  = 0.5 * y1_im;");
    if (!complete || !gpu.override_kernel_source("fft_stockham", mutated.c_str()))
        return false;
    actual_source = std::move(mutated);
    return true;
}

#if defined(PULP_ENABLE_SCENE3D)
struct ForegroundRegion {
    std::uint32_t count = 0;
    std::uint32_t min_x = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t min_y = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t max_x = 0;
    std::uint32_t max_y = 0;
};

ForegroundRegion foreground_region(std::span<const std::uint8_t> rgba,
                                   std::uint32_t width, std::uint32_t height) {
    ForegroundRegion region;
    if (rgba.size() != static_cast<std::size_t>(width) * height * 4u || rgba.size() < 4)
        return region;
    const int bg_r = rgba[0], bg_g = rgba[1], bg_b = rgba[2];
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto offset = (static_cast<std::size_t>(y) * width + x) * 4u;
            const int delta = std::abs(static_cast<int>(rgba[offset]) - bg_r) +
                              std::abs(static_cast<int>(rgba[offset + 1]) - bg_g) +
                              std::abs(static_cast<int>(rgba[offset + 2]) - bg_b);
            if (delta <= static_cast<int>(renderer3d_oracle::kForegroundDelta)) continue;
            ++region.count;
            region.min_x = std::min(region.min_x, x);
            region.min_y = std::min(region.min_y, y);
            region.max_x = std::max(region.max_x, x);
            region.max_y = std::max(region.max_y, y);
        }
    }
    return region;
}

std::uint64_t rgba_fingerprint(std::uint32_t width, std::uint32_t height,
                               const std::vector<std::uint8_t>& pixels) {
    render::HeadlessSurface::Rgba rgba;
    rgba.width = width;
    rgba.height = height;
    rgba.pixels = pixels;
    return render::HeadlessSurface::rgba_fingerprint(rgba);
}
#endif

bool validation_fail(std::string* error, std::string message) {
    if (error) *error = std::move(message);
    return false;
}

#if PULP_GPU_PROBE_HAS_THREEJS && PULP_GPU_PROBE_THREEJS_CALLABLE
std::optional<std::string> read_text(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(stream),
                       std::istreambuf_iterator<char>());
}

struct SampleRgb {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
};

SampleRgb sample_rgb(std::span<const std::uint8_t> rgba, std::uint32_t width,
                     std::uint32_t x, std::uint32_t y) {
    const auto offset = (static_cast<std::size_t>(y) * width + x) * 4u;
    if (offset + 2u >= rgba.size()) return {};
    return {rgba[offset], rgba[offset + 1u], rgba[offset + 2u]};
}

bool near_rgb(SampleRgb value, SampleRgb expected, std::uint8_t tolerance = 24) {
    const auto near = [tolerance](std::uint8_t actual, std::uint8_t target) {
        return std::abs(static_cast<int>(actual) - static_cast<int>(target)) <= tolerance;
    };
    return near(value.r, expected.r) && near(value.g, expected.g) &&
        near(value.b, expected.b);
}

bool is_red(SampleRgb value) {
    return value.r > 140 && value.r > value.g + 60 && value.r > value.b + 60;
}

bool is_green(SampleRgb value) {
    return value.g > 140 && value.g > value.r + 60 && value.g > value.b + 40;
}

template <typename Predicate>
double region_match_fraction(std::span<const std::uint8_t> rgba, std::uint32_t width,
                             std::uint32_t x0, std::uint32_t y0,
                             std::uint32_t x1, std::uint32_t y1,
                             Predicate&& predicate) {
    std::uint32_t matched = 0;
    std::uint32_t total = 0;
    for (std::uint32_t y = y0; y < y1; ++y) {
        for (std::uint32_t x = x0; x < x1; ++x) {
            matched += predicate(sample_rgb(rgba, width, x, y)) ? 1u : 0u;
            ++total;
        }
    }
    return total == 0 ? 0.0 : static_cast<double>(matched) / total;
}

std::string dawn_adapter_string(wgpu::StringView value) {
    if (value.data == nullptr || value.length == 0) return {};
    return std::string(value.data, value.length);
}

std::string_view dawn_backend(wgpu::BackendType backend) {
    switch (backend) {
        case wgpu::BackendType::Metal: return "Metal";
        case wgpu::BackendType::D3D12: return "D3D12";
        case wgpu::BackendType::D3D11: return "D3D11";
        case wgpu::BackendType::Vulkan: return "Vulkan";
        case wgpu::BackendType::OpenGL: return "OpenGL";
        case wgpu::BackendType::OpenGLES: return "OpenGLES";
        case wgpu::BackendType::WebGPU: return "WebGPU";
        case wgpu::BackendType::Null: return "Null";
        default: return "Unknown";
    }
}

AdapterClass dawn_adapter_class(wgpu::AdapterType type) {
    if (type == wgpu::AdapterType::DiscreteGPU ||
        type == wgpu::AdapterType::IntegratedGPU)
        return AdapterClass::hardware;
    if (type == wgpu::AdapterType::CPU) return AdapterClass::software;
    return AdapterClass::unknown;
}

void populate_surface_adapter_identity(ProbeResult& result, render::GpuSurface& surface) {
    auto* device = static_cast<wgpu::Device*>(surface.dawn_device_handle());
    wgpu::AdapterInfo info{};
    if (device == nullptr || device->GetAdapterInfo(&info) != wgpu::Status::Success) {
        result.adapter.status = IdentityStatus::unverified;
        return;
    }
    result.adapter.status = IdentityStatus::authentic;
    result.adapter.classification = dawn_adapter_class(info.adapterType);
    result.adapter.backend = bounded(dawn_backend(info.backendType));
    result.adapter.name = bounded(dawn_adapter_string(info.device));
    result.adapter.vendor = bounded(dawn_adapter_string(info.vendor));
    result.adapter.architecture = bounded(dawn_adapter_string(info.architecture));
    result.adapter.device = bounded(dawn_adapter_string(info.description));
}
#endif

} // namespace

RecipeRun run_renderer3d_recipe(const RunOptions& options) {
    const auto& recipe = *find_recipe(kRecipeIds[0]);
    RecipeRun run;
    run.result = base_result(recipe, options,
                             renderer3d_oracle::kTranslationUnitSha256, true);
    // The mutation changes the declared execution shape even when this build
    // cannot execute Renderer3D. This keeps typed unavailable evidence valid
    // against the same recipe-bound identity as a callable run.
    if (options.apply_negative_mutation)
        run.result.dimensions = {32, 32, 1'024};

#if !defined(PULP_ENABLE_SCENE3D)
    unavailable_passes(run.result, recipe, "renderer3d_not_compiled");
    run.result.recommendations.emplace_back(
        "Reconfigure the SDK with PULP_ENABLE_SCENE3D=ON and rerun the recipe.");
    return run;
#else
    render::HardcodedCubeRenderConfig config;
    config.width = run.result.dimensions.width;
    config.height = run.result.dimensions.height;
    // The planted regression must affect bytes produced by the GPU. A 32x32
    // render still exercises submission and readback while making the recipe's
    // 1,500-pixel foreground floor impossible to satisfy.
    run.result.dimensions = {
        config.width,
        config.height,
        static_cast<std::uint64_t>(config.width) * config.height,
    };
    const auto rendered = render::Renderer3D::render_hardcoded_textured_cube(config);

    run.result.adapter.status = rendered.adapter_info_available
        ? IdentityStatus::unverified : IdentityStatus::unavailable;
    run.result.adapter.classification = rendered.null_backend_requested
        ? AdapterClass::null_adapter : AdapterClass::unknown;
    run.result.adapter.backend = bounded(rendered.adapter_backend);
    run.result.adapter.name = bounded(rendered.adapter_name);
    run.result.adapter.vendor = bounded(rendered.adapter_vendor);
    run.result.adapter.architecture = bounded(rendered.adapter_architecture);

    if (!rendered.gpu_available) {
        unavailable_passes(run.result, recipe, "renderer3d_adapter_unavailable");
        run.result.recommendations.emplace_back(
            "Run pulp doctor gpu to inspect adapter acquisition and render health.");
        return run;
    }

    run.result.passes.push_back(pass(0, recipe.semantic_passes[0], Verdict::pass,
                                     true, "adapter_acquired"));
    run.result.passes.push_back(pass(1, recipe.semantic_passes[1],
        rendered.command_submitted ? Verdict::pass : Verdict::fail,
        rendered.command_submitted, rendered.command_submitted
            ? "render_submitted" : "render_not_submitted"));
    run.result.passes.push_back(pass(2, recipe.semantic_passes[2],
        rendered.readback_completed ? Verdict::pass : Verdict::fail,
        rendered.readback_completed, rendered.readback_completed
            ? "readback_completed" : "readback_incomplete"));

    auto observed = rendered.rgba;
    const auto region = foreground_region(observed, rendered.width, rendered.height);
    const bool portable_structure = rendered.success &&
        region.count > renderer3d_oracle::kMinForegroundPixels &&
        region.min_x > 3 && region.min_y > 3 &&
        region.max_x < rendered.width - 3 && region.max_y < rendered.height - 3;
    const bool metal_scope = rendered.adapter_info_available &&
                             rendered.adapter_backend_type == "Metal";
    const auto fingerprint = observed.empty()
        ? 0 : rgba_fingerprint(rendered.width, rendered.height, observed);
    const bool fingerprint_match = metal_scope && portable_structure &&
        rendered.distinct_color_count >= renderer3d_oracle::kMinDistinctColors &&
        rendered.non_transparent_pixel_count >=
            renderer3d_oracle::kMinNonTransparentPixels &&
        fingerprint == renderer3d_oracle::kMacosDefaultMetalFingerprint;
    const auto content_verdict = !portable_structure
        ? Verdict::fail : (metal_scope ? (fingerprint_match ? Verdict::pass : Verdict::fail)
                                          : Verdict::unverified);
    run.result.passes.push_back(pass(3, recipe.semantic_passes[3],
        content_verdict, rendered.readback_completed && !rendered.rgba.empty(),
        !portable_structure ? "portable_structure_mismatch"
        : (metal_scope ? (fingerprint_match ? "metal_fingerprint_match"
                                            : "metal_fingerprint_mismatch")
                       : "portable_structure_verified_exact_golden_unavailable")));
    run.result.verdict = content_verdict;

    std::ostringstream oracle_json;
    oracle_json << "{\"adapter_scope\":\""
                << (metal_scope ? "macos_default_metal" : "portable_structure")
                << "\",\"expected_fingerprint\":\""
                << renderer3d_oracle::kMacosDefaultMetalFingerprint
                << "\",\"observed_fingerprint\":\"" << fingerprint
                << "\",\"foreground_pixels\":" << region.count
                << ",\"bounds\":[" << region.min_x << ',' << region.min_y << ','
                << region.max_x << ',' << region.max_y << "]}";
    const auto oracle_text = oracle_json.str();
    attach(run, artifact("content-oracle.json", ArtifactKind::json,
                         "application/json",
                         std::vector<std::uint8_t>(oracle_text.begin(),
                                                   oracle_text.end())));

    if (!rendered.png.empty())
        attach(run, artifact("final.png", ArtifactKind::image, "image/png", rendered.png));
    if (!observed.empty())
        attach(run, artifact("observed.rgba8", ArtifactKind::image,
                             "application/octet-stream", std::move(observed)));
    if (run.result.verdict == Verdict::unverified)
        run.result.recommendations.emplace_back(
            "Portable structure passed, but this adapter has no scoped exact golden.");
    else if (run.result.verdict == Verdict::fail)
        run.result.recommendations.emplace_back(
            "Inspect the named render/readback/content pass before comparing screenshots.");
    return run;
#endif
}

RecipeRun run_gpu_compute_magnitude_recipe(const RunOptions& options) {
    const auto& recipe = *find_recipe(kRecipeIds[1]);
    auto gpu = render::GpuCompute::create();
    const char* builtin = gpu ? gpu->kernel_source("magnitude") : nullptr;
    std::string actual_source = builtin ? std::string{builtin}
                                        : std::string{recipe.source_identity};
    if (options.apply_negative_mutation &&
        native_recipe_test_fault("magnitude-mutation-source-drift"))
        actual_source = "planted magnitude mutation source drift";
    bool mutation_installed = false;
    if (gpu && builtin && options.apply_negative_mutation) {
        mutation_installed = replace_once(actual_source, "re * re + im * im",
                                                         "re * re + 0.25 * im * im") &&
                             gpu->override_kernel_source("magnitude",
                                                         actual_source.c_str());
    }
    RecipeRun run;
    run.result = base_result(recipe, options, actual_source);
    if (!gpu || !builtin) {
        unavailable_passes(run.result, recipe, "gpu_compute_not_compiled");
        run.result.recommendations.emplace_back(
            "Use a GPU-enabled SDK and run pulp doctor gpu before retrying.");
        return run;
    }

    if (options.apply_negative_mutation && !mutation_installed) {
        unverified_passes(run.result, recipe, "magnitude_mutation_source_drift");
        run.result.recommendations.emplace_back(
            "Update the magnitude negative-control mutation for the current kernel source.");
        return run;
    }
    if (!gpu->initialize_standalone()) {
        unavailable_passes(run.result, recipe, "gpu_compute_adapter_unavailable");
        run.result.recommendations.emplace_back(
            "Run pulp doctor gpu to inspect standalone compute initialization.");
        return run;
    }

    const auto capabilities = gpu->capabilities();
    populate_adapter_identity(run.result, capabilities);

    constexpr std::uint32_t count = 256;
    std::array<float, count * 2> input{};
    std::array<float, count> expected{};
    std::array<float, count> observed{};
    for (std::uint32_t i = 0; i < count; ++i) {
        const float x = static_cast<float>(i + 1);
        input[i * 2] = std::sin(x * 0.173f) * 7.0f + 0.25f;
        input[i * 2 + 1] = std::cos(x * 0.113f) * 5.0f - 0.5f;
        expected[i] = std::sqrt(input[i * 2] * input[i * 2] +
                                input[i * 2 + 1] * input[i * 2 + 1]);
    }
    const bool dispatched = gpu->compute_magnitude(input.data(), observed.data(), count);
    double max_error = 0.0;
    bool finite = dispatched;
    bool within_tolerance = dispatched;
    for (std::uint32_t i = 0; i < count && dispatched; ++i) {
        finite = finite && std::isfinite(observed[i]);
        const double error = std::abs(static_cast<double>(observed[i]) - expected[i]);
        const double allowed = recipe.tolerance.absolute +
                               recipe.tolerance.relative * std::abs(expected[i]);
        max_error = std::max(max_error, error);
        within_tolerance = within_tolerance && error <= allowed;
    }
    const bool oracle_match = finite && within_tolerance;
    run.result.numeric_sample_count = count;
    const bool eligible_hardware = capabilities.available &&
        capabilities.adapter_info_authentic &&
        adapter_class(capabilities.adapter_type) == AdapterClass::hardware;
    run.result.passes.push_back(pass(0, recipe.semantic_passes[0],
        eligible_hardware ? Verdict::pass : Verdict::unverified,
        capabilities.available, eligible_hardware
            ? "compute_hardware_adapter_verified" : "compute_adapter_unverified"));
    run.result.passes.push_back(pass(1, recipe.semantic_passes[1],
        dispatched ? Verdict::pass : Verdict::fail, dispatched,
        dispatched ? "magnitude_dispatch_completed" : "magnitude_dispatch_failed"));
    auto oracle = pass(2, recipe.semantic_passes[2],
        oracle_match ? Verdict::pass : Verdict::fail, dispatched,
        oracle_match ? "cpu_oracle_match" : "cpu_oracle_mismatch");
    oracle.expected = 0.0;
    oracle.observed = max_error;
    oracle.absolute_error = max_error;
    run.result.passes.push_back(std::move(oracle));
    if (!dispatched || !oracle_match)
        run.result.verdict = Verdict::fail;
    else
        run.result.verdict = eligible_hardware ? Verdict::pass : Verdict::unverified;

    attach(run, artifact("input.complex-f32", ArtifactKind::numeric_samples,
                         "application/octet-stream", floats_as_bytes(input)));
    attach(run, artifact("expected.f32", ArtifactKind::numeric_samples,
                         "application/octet-stream", floats_as_bytes(expected)));
    attach(run, artifact("observed.f32", ArtifactKind::numeric_samples,
                         "application/octet-stream", floats_as_bytes(observed)));
    if (!eligible_hardware)
        run.result.recommendations.emplace_back(
            "Repeat on a runtime that reports authentic hardware adapter identity.");
    if (!oracle_match)
        run.result.recommendations.emplace_back(
            "Inspect the magnitude oracle pass and its bounded numeric artifacts.");
    return run;
}

RecipeRun run_gpu_audio_stft_recipe(const RunOptions& options) {
    constexpr std::uint32_t count = 1024;
    constexpr std::uint64_t input_field = 0x53544654ULL;

    const auto& recipe = *find_recipe(kRecipeIds[2]);
    auto gpu = render::GpuCompute::create();
    const char* builtin = gpu ? gpu->kernel_source("fft_stockham") : nullptr;
    std::string actual_source = builtin ? std::string{builtin}
                                        : std::string{recipe.source_identity};
    if (options.apply_negative_mutation &&
        native_recipe_test_fault("stft-mutation-source-drift"))
        actual_source = "planted STFT mutation source drift";
    bool mutation_installed = false;
    if (gpu && builtin && options.apply_negative_mutation)
        mutation_installed = install_stft_mutation(*gpu, actual_source);

    RecipeRun run;
    run.result = base_result(recipe, options, actual_source);
    if (!gpu || !builtin) {
        unavailable_passes(run.result, recipe, "gpu_stft_not_compiled");
        run.result.recommendations.emplace_back(
            "Use a GPU-enabled SDK and run pulp doctor gpu before retrying.");
        return run;
    }
    if (options.apply_negative_mutation && !mutation_installed) {
        unverified_passes(run.result, recipe, "stft_mutation_source_drift");
        run.result.recommendations.emplace_back(
            "Update the STFT negative-control mutation for the current kernel source.");
        return run;
    }
    if (!gpu->initialize_standalone()) {
        unavailable_passes(run.result, recipe, "gpu_stft_adapter_unavailable");
        run.result.recommendations.emplace_back(
            "Run pulp doctor gpu to inspect standalone compute initialization.");
        return run;
    }

    const auto capabilities = gpu->capabilities();
    populate_adapter_identity(run.result, capabilities);
    const bool eligible_hardware = capabilities.available &&
        capabilities.adapter_info_authentic &&
        adapter_class(capabilities.adapter_type) == AdapterClass::hardware;

    std::array<float, count> input{};
    for (std::uint32_t i = 0; i < count; ++i) {
        const float unit = signal::unit_from<float>(
            signal::mix64(recipe.seed, i, input_field));
        input[i] = 0.5f * (unit * 2.0f - 1.0f);
    }

    gpu_audio::GpuStft stft;
    const bool prepared = stft.prepare(
        count, signal::WindowFunction::Type::hann, 0.0f, gpu.get());
    const auto expected_window = signal::WindowFunction::generate<float>(
        static_cast<int>(count), signal::WindowFunction::Type::hann);
    const bool window_matches = prepared && stft.gpu_available() &&
        stft.fft_size() == count && stft.window() == expected_window;

    signal::FftT<double> cpu_fft(static_cast<int>(count));
    const bool cpu_ready = cpu_fft.ready();
    std::array<std::complex<double>, count> cpu_spectrum{};
    std::array<double, count> expected_exact{};
    std::array<float, count> expected_magnitude{};
    if (window_matches && cpu_ready) {
        for (std::uint32_t i = 0; i < count; ++i) {
            const float windowed = input[i] * expected_window[i];
            cpu_spectrum[i] = {static_cast<double>(windowed), 0.0};
        }
        cpu_fft.forward(cpu_spectrum.data());
        for (std::uint32_t i = 0; i < count; ++i) {
            expected_exact[i] = std::abs(cpu_spectrum[i]);
            expected_magnitude[i] = static_cast<float>(expected_exact[i]);
        }
    }

    std::array<float, count * 2> observed_spectrum{};
    const bool analyzed = window_matches && cpu_ready &&
        stft.analyze(input.data(), observed_spectrum.data());
    std::array<float, count> observed_magnitude{};
    std::array<float, count> absolute_error{};
    bool finite = analyzed;
    bool nonzero = false;
    bool within_tolerance = analyzed;
    double max_error = 0.0;
    for (std::uint32_t i = 0; i < count && analyzed; ++i) {
        const double real = observed_spectrum[i * 2u];
        const double imaginary = observed_spectrum[i * 2u + 1u];
        const double magnitude = std::hypot(real, imaginary);
        observed_magnitude[i] = static_cast<float>(magnitude);
        finite = finite && std::isfinite(real) && std::isfinite(imaginary) &&
                 std::isfinite(magnitude);
        nonzero = nonzero || magnitude > 1.0e-6;
        const double error = std::abs(magnitude - expected_exact[i]);
        absolute_error[i] = static_cast<float>(error);
        max_error = std::max(max_error, error);
        const double allowed = recipe.tolerance.absolute +
            recipe.tolerance.relative * expected_exact[i];
        within_tolerance = within_tolerance && error <= allowed;
    }
    const bool magnitude_valid = analyzed && finite && nonzero;
    const bool oracle_match = magnitude_valid && within_tolerance;
    run.result.numeric_sample_count = count;

    const bool prepare_ok = window_matches && cpu_ready;
    const Verdict prepare_verdict = !prepare_ok
        ? Verdict::fail
        : (eligible_hardware ? Verdict::pass : Verdict::unverified);
    std::string prepare_code;
    if (!prepared)
        prepare_code = "stft_prepare_failed";
    else if (!window_matches)
        prepare_code = "stft_window_mismatch";
    else if (!cpu_ready)
        prepare_code = "cpu_fft_unavailable";
    else
        prepare_code = eligible_hardware ? "stft_hardware_adapter_prepared"
                                         : "stft_adapter_unverified";
    run.result.passes.push_back(pass(0, recipe.semantic_passes[0],
                                     prepare_verdict, prepare_ok,
                                     std::move(prepare_code)));
    run.result.passes.push_back(pass(1, recipe.semantic_passes[1],
        analyzed ? Verdict::pass : Verdict::fail, analyzed,
        analyzed ? "stft_forward_completed" : "stft_forward_failed"));
    run.result.passes.push_back(pass(2, recipe.semantic_passes[2],
        magnitude_valid ? Verdict::pass : Verdict::fail, analyzed,
        magnitude_valid ? "stft_magnitude_finite_nonzero"
                        : "stft_magnitude_invalid"));
    auto oracle = pass(3, recipe.semantic_passes[3],
        oracle_match ? Verdict::pass : Verdict::fail, analyzed,
        oracle_match ? "cpu_fft_oracle_match" : "cpu_fft_oracle_mismatch");
    oracle.expected = 0.0;
    oracle.observed = max_error;
    oracle.absolute_error = max_error;
    run.result.passes.push_back(std::move(oracle));

    if (!prepare_ok || !analyzed || !magnitude_valid || !oracle_match)
        run.result.verdict = Verdict::fail;
    else
        run.result.verdict = eligible_hardware ? Verdict::pass : Verdict::unverified;

    attach(run, artifact("input.f32", ArtifactKind::numeric_samples,
                         "application/octet-stream", floats_as_bytes(input)));
    attach(run, artifact("window.f32", ArtifactKind::numeric_samples,
                         "application/octet-stream", floats_as_bytes(expected_window)));
    attach(run, artifact("expected-magnitude.f32", ArtifactKind::numeric_samples,
                         "application/octet-stream", floats_as_bytes(expected_magnitude)));
    attach(run, artifact("observed-spectrum.complex-f32",
                         ArtifactKind::numeric_samples, "application/octet-stream",
                         floats_as_bytes(observed_spectrum)));
    attach(run, artifact("observed-magnitude.f32", ArtifactKind::numeric_samples,
                         "application/octet-stream", floats_as_bytes(observed_magnitude)));
    attach(run, artifact("absolute-error.f32", ArtifactKind::numeric_samples,
                         "application/octet-stream", floats_as_bytes(absolute_error)));

    if (!eligible_hardware)
        run.result.recommendations.emplace_back(
            "Repeat on a runtime that reports authentic hardware adapter identity.");
    if (!oracle_match)
        run.result.recommendations.emplace_back(
            "Inspect the forward, magnitude, and CPU-oracle artifacts for the first divergence.");
    return run;
}

RecipeRun run_threejs_multi_pass_recipe(
    const RunOptions& options, std::optional<std::string> threejs_runtime_root) {
    const auto& recipe = *find_recipe(kRecipeIds[3]);
#if !PULP_GPU_PROBE_THREEJS_CALLABLE
    (void)threejs_runtime_root;
    RecipeRun run;
    run.result = base_result(recipe, options, recipe.source_identity);
    unavailable_passes(run.result, recipe, "threejs_runtime_not_compiled");
    run.result.recommendations.emplace_back(
        "Use a GPU-enabled SDK carrying the pinned Three.js runtime and V8.");
    return run;
#else
#if !PULP_GPU_PROBE_HAS_THREEJS
    RecipeRun run;
    run.result = base_result(recipe, options, recipe.source_identity);
    unavailable_passes(run.result, recipe, "threejs_runtime_not_compiled");
    run.result.recommendations.emplace_back(
        "Use a GPU-enabled SDK carrying the pinned Three.js runtime.");
    return run;
#else
    namespace fs = std::filesystem;
    const fs::path runtime_root = threejs_runtime_root
        ? fs::path(*threejs_runtime_root)
        : fs::path(PULP_GPU_PROBE_THREEJS_SOURCE_DIR);
    const auto webgpu = read_text(runtime_root / "build" / "three.webgpu.js");
    const auto core = read_text(runtime_root / "build" / "three.core.js");

    constexpr std::string_view module_template = R"JS(
        import * as THREE from 'three/webgpu';

        const canvas = document.createElement('canvas');
        canvas.id = 'pulp-threejs-probe-canvas';
        canvas.width = 96;
        canvas.height = 96;
        canvas.style.width = '96px';
        canvas.style.height = '96px';
        document.body.appendChild(canvas);

        const context = canvas.getContext('webgpu');
        const renderer = new THREE.WebGPURenderer({ canvas, context, antialias: false });
        await renderer.init();
        renderer.setSize(96, 96, false);

        const scene = new THREE.Scene();
        scene.background = new THREE.Color(0x204080);
        const camera = new THREE.OrthographicCamera(-1, 1, 1, -1, 0.1, 10);
        camera.position.z = 2;
        const geometry = new THREE.PlaneGeometry(0.78, 1.35);
        const swatch = new THREE.Mesh(
            geometry,
            new THREE.MeshBasicMaterial({ color: 0xf03030, side: THREE.DoubleSide })
        );
        scene.add(swatch);

        globalThis.__pulpProbeRender = (stage) => {
            swatch.position.x = stage === 1 ? -0.5 : stage === 2 ? 0.5 : 0;
            swatch.scale.set(stage === 0 ? 3 : 1, stage === 0 ? 2 : 1, 1);
            swatch.material.color.set(stage === 0 ? 0x204080
                : stage === 1 ? 0xf03030 : __RIGHT_COLOR__);
            renderer.render(scene, camera);
            if (typeof context.present === 'function') context.present();
            renderer.render(scene, camera);
            if (typeof context.present === 'function') context.present();
            return JSON.stringify({
                stage,
                contextType: renderer.getContext() && renderer.getContext()._objectName || '',
                backend: renderer.backend && renderer.backend.constructor
                    ? renderer.backend.constructor.name : '',
                bufferedSkipCount: (globalThis.__phase13BufferedSkips || []).length
            });
        };
        globalThis.__pulpProbeReady = true;
        export default true;
    )JS";

    std::string module(module_template);
    const auto seeded_channel = static_cast<std::uint32_t>(recipe.seed ^ (recipe.seed >> 32u)) % 2u;
    const std::string_view right_color = options.apply_negative_mutation
        ? (seeded_channel == 0u ? "0x3040f0" : "0xf030d0")
        : "0x30f060";
    if (!replace_once(module, "__RIGHT_COLOR__", right_color))
        throw std::runtime_error("Three.js probe module mutation seam drifted");

    std::string source_identity;
    if (webgpu) source_identity += *webgpu;
    if (core) source_identity += *core;
    source_identity += module;
    RecipeRun run;
    run.result = base_result(recipe, options, source_identity);

    if (!webgpu || !core) {
        unavailable_passes(run.result, recipe, "threejs_runtime_missing");
        run.result.recommendations.emplace_back(
            "Select an installed or source runtime containing the complete pinned Three.js payload.");
        return run;
    }
    if (digest(*webgpu) != PULP_GPU_PROBE_THREEJS_WEBGPU_SHA256 ||
        digest(*core) != PULP_GPU_PROBE_THREEJS_CORE_SHA256) {
        unavailable_passes(run.result, recipe, "threejs_runtime_identity_mismatch");
        run.result.recommendations.emplace_back(
            "Restore the immutable Three.js runtime recorded by the SDK manifest.");
        return run;
    }
    if (!view::is_engine_available(view::JsEngineType::v8)) {
        unavailable_passes(run.result, recipe, "threejs_v8_unavailable");
        run.result.recommendations.emplace_back(
            "Run this recipe with a Pulp CLI built against the sealed V8 provider.");
        return run;
    }

    auto gpu = render::GpuSurface::create_dawn();
    render::GpuSurface::Config gpu_config{};
    gpu_config.width = recipe.dimensions.width;
    gpu_config.height = recipe.dimensions.height;
    gpu_config.native_surface_handle = nullptr;
    if (!gpu || !gpu->initialize(gpu_config)) {
        unavailable_passes(run.result, recipe, "threejs_adapter_unavailable");
        run.result.recommendations.emplace_back(
            "Run pulp doctor gpu to inspect native Dawn adapter acquisition.");
        return run;
    }

    const auto surface_adapter = gpu->adapter_info();
    populate_surface_adapter_identity(run.result, *gpu);
    const bool eligible_hardware = surface_adapter.available &&
        surface_adapter.native_bridge &&
        run.result.adapter.status == IdentityStatus::authentic &&
        run.result.adapter.classification == AdapterClass::hardware;

    auto skia = render::SkiaSurface::create(
        *gpu, {.width = recipe.dimensions.width, .height = recipe.dimensions.height});
    if (!skia || !skia->is_available()) {
        unavailable_passes(run.result, recipe, "threejs_readback_unavailable");
        run.result.recommendations.emplace_back(
            "Use a GPU-enabled SDK with the Skia/Dawn readback path available.");
        return run;
    }
    if (!eligible_hardware) {
        for (std::uint32_t i = 0; i < recipe.semantic_passes.size(); ++i) {
            run.result.passes.push_back(pass(
                i, recipe.semantic_passes[i], Verdict::unverified,
                i == 0 && surface_adapter.available,
                "threejs_hardware_identity_required"));
        }
        run.result.verdict = Verdict::unverified;
        run.result.recommendations.emplace_back(
            "Repeat on a runtime that reports authentic hardware adapter identity.");
        return run;
    }

    view::View root;
    root.set_bounds({0, 0, 96, 96});
    root.set_theme(view::Theme::dark());
    view::ScriptEngine engine(view::JsEngineType::v8);
    state::StateStore store;
    view::WidgetBridge bridge(engine, root, store, gpu.get());
    bridge.load_script("");

    bool module_completed = false;
    std::string module_error;
    const auto resolver = [&](std::string_view path) -> std::optional<std::string> {
        if (path == "three/webgpu") return *webgpu;
        if (path == "./three.core.js" || path == "three.core.js") return *core;
        return std::nullopt;
    };
    engine.run_module(module, resolver,
        [&](const std::string& error, const choc::value::Value&) {
            module_completed = true;
            module_error = error;
        });
    for (int i = 0; i < 512 && !module_completed; ++i)
        engine.pump_message_loop();

    run.result.passes.push_back(pass(0, recipe.semantic_passes[0],
        eligible_hardware ? Verdict::pass : Verdict::unverified,
        surface_adapter.available, eligible_hardware
            ? "threejs_hardware_adapter_verified" : "threejs_adapter_unverified"));
    const bool module_ready = module_completed && module_error.empty() &&
        engine.evaluate("globalThis.__pulpProbeReady === true")
            .getWithDefault<bool>(false);
    run.result.passes.push_back(pass(1, recipe.semantic_passes[1],
        module_ready ? Verdict::pass : Verdict::fail, module_completed,
        module_ready ? "threejs_module_initialized" : "threejs_module_initialization_failed"));
    if (!module_ready) {
        for (std::uint32_t i = 2; i < recipe.semantic_passes.size(); ++i)
            run.result.passes.push_back(pass(i, recipe.semantic_passes[i], Verdict::fail,
                                             false, "threejs_module_not_ready"));
        run.result.verdict = Verdict::fail;
        run.result.recommendations.emplace_back(
            "Inspect the pinned Three.js module initialization and native WebGPU bridge.");
        return run;
    }

    root.layout_children();
    const auto native_canvas_id = std::string(engine.evaluate(
        "document.getElementById('pulp-threejs-probe-canvas')._id")
        .getWithDefault<std::string_view>(""));
    auto* canvas_widget = dynamic_cast<view::CanvasWidget*>(
        bridge.widget(native_canvas_id));
    std::array<std::vector<std::uint8_t>, 3> frames;
    std::array<bool, 3> captured{};
    for (std::uint32_t stage = 0; stage < 3; ++stage) {
        const auto state_json = std::string(engine.evaluate(
            "globalThis.__pulpProbeRender(" + std::to_string(stage) + ")")
            .getWithDefault<std::string_view>(""));
        for (int i = 0; i < 8; ++i) engine.pump_message_loop();
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        bool ok = canvas_widget && skia && skia->is_available() && gpu->begin_frame();
        if (ok) {
            auto* canvas = skia->begin_frame();
            ok = canvas != nullptr;
            if (ok) canvas_widget->paint(*canvas);
            ok = ok && canvas_widget->last_native_gpu_texture_draw_succeeded();
            skia->end_frame();
            ok = ok && skia->read_current_rgba(frames[stage], width, height);
            gpu->end_frame();
        }
        captured[stage] = ok && width == 96 && height == 96 &&
            frames[stage].size() == 96u * 96u * 4u &&
            state_json.find("\"bufferedSkipCount\":0") != std::string_view::npos;
    }

    constexpr SampleRgb expected_background{0x20, 0x40, 0x80};
    const auto background_coverage = region_match_fraction(
        frames[0], 96, 8, 8, 88, 88,
        [&](SampleRgb value) { return near_rgb(value, expected_background); });
    const auto left_coverage = region_match_fraction(
        frames[1], 96, 12, 20, 38, 76, is_red);
    const auto left_right_background_coverage = region_match_fraction(
        frames[1], 96, 58, 20, 84, 76,
        [&](SampleRgb value) { return near_rgb(value, expected_background); });
    const auto final_left_background_coverage = region_match_fraction(
        frames[2], 96, 12, 20, 38, 76,
        [&](SampleRgb value) { return near_rgb(value, expected_background); });
    const auto final_right_coverage = region_match_fraction(
        frames[2], 96, 58, 20, 84, 76, is_green);
    const bool background_ok = captured[0] && background_coverage >= 0.95;
    const bool left_ok = captured[1] && left_coverage >= 0.90 &&
        left_right_background_coverage >= 0.95;
    const bool final_render_ok = captured[2];
    const bool oracle_ok = background_ok && left_ok && final_render_ok &&
        final_left_background_coverage >= 0.95 && final_right_coverage >= 0.90;
    run.result.numeric_sample_count = 5;
    run.result.passes.push_back(pass(2, recipe.semantic_passes[2],
        background_ok ? Verdict::pass : Verdict::fail, captured[0],
        background_ok ? "background_region_match" : "background_region_mismatch"));
    run.result.passes.push_back(pass(3, recipe.semantic_passes[3],
        left_ok ? Verdict::pass : Verdict::fail, captured[1],
        left_ok ? "left_swatch_region_match" : "left_swatch_region_mismatch"));
    run.result.passes.push_back(pass(4, recipe.semantic_passes[4],
        final_render_ok ? Verdict::pass : Verdict::fail, captured[2],
        final_render_ok ? "final_swatch_readback_completed"
                        : "final_swatch_readback_failed"));
    auto oracle = pass(5, recipe.semantic_passes[5],
        oracle_ok ? Verdict::pass : Verdict::fail, captured[2],
        oracle_ok ? "cpu_region_color_oracle_match"
                  : "cpu_region_color_oracle_mismatch");
    oracle.expected = 5.0;
    oracle.observed = static_cast<double>(background_ok) +
                      static_cast<double>(captured[1] && left_coverage >= 0.90) +
                      static_cast<double>(captured[1] &&
                                          left_right_background_coverage >= 0.95) +
                      static_cast<double>(captured[2] &&
                                          final_left_background_coverage >= 0.95) +
                      static_cast<double>(captured[2] && final_right_coverage >= 0.90);
    oracle.absolute_error = std::abs(*oracle.expected - *oracle.observed);
    run.result.passes.push_back(std::move(oracle));
    run.result.verdict = (!background_ok || !left_ok || !final_render_ok || !oracle_ok)
        ? Verdict::fail : (eligible_hardware ? Verdict::pass : Verdict::unverified);

    attach(run, artifact("background.rgba8", ArtifactKind::image,
                         "application/octet-stream", std::move(frames[0])));
    attach(run, artifact("left-swatch.rgba8", ArtifactKind::image,
                         "application/octet-stream", std::move(frames[1])));
    attach(run, artifact("final.rgba8", ArtifactKind::image,
                         "application/octet-stream", std::move(frames[2])));
    std::ostringstream oracle_json;
    oracle_json << std::fixed << std::setprecision(6)
                << "{\"background_coverage\":" << background_coverage
                << ",\"left_coverage\":" << left_coverage
                << ",\"left_right_background_coverage\":"
                << left_right_background_coverage
                << ",\"final_left_background_coverage\":"
                << final_left_background_coverage
                << ",\"final_right_coverage\":" << final_right_coverage
                << ",\"matched\":" << (oracle_ok ? "true" : "false") << '}';
    const auto oracle_text = oracle_json.str();
    attach(run, artifact("content-oracle.json", ArtifactKind::json,
                         "application/json",
                         std::vector<std::uint8_t>(oracle_text.begin(), oracle_text.end())));
    if (!oracle_ok)
        run.result.recommendations.emplace_back(
            "Inspect the named intermediate readbacks and independent color-region oracle.");
    return run;
#endif
#endif
}

bool validate(const RecipeRun& run, std::string* error) {
    if (!validate(run.result, error)) return false;
    if (run.result.artifacts.size() != run.payloads.size())
        return validation_fail(error, "artifact descriptors and payloads must be one-to-one");
    for (std::size_t i = 0; i < run.payloads.size(); ++i) {
        const auto& declared = run.result.artifacts[i];
        const auto& payload = run.payloads[i];
        const auto& embedded = payload.artifact;
        if (declared.name != embedded.name || declared.kind != embedded.kind ||
            declared.mime != embedded.mime || declared.bytes != embedded.bytes ||
            declared.sha256 != embedded.sha256)
            return validation_fail(error,
                "artifact descriptor does not match its payload descriptor");
        if (declared.bytes != payload.bytes.size())
            return validation_fail(error, "artifact byte count does not match its payload");
        if (declared.sha256 != digest(payload.bytes))
            return validation_fail(error, "artifact sha256 does not match its payload");
    }
    return true;
}

} // namespace pulp::tooling::gpu_probe
