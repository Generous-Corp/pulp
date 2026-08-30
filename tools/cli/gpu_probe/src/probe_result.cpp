#include <pulp_tooling/gpu_probe/probe_result.hpp>

#include "gpu_probe_recipe_catalog_data.h"

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace pulp::tooling::gpu_probe {
namespace {

constexpr std::array renderer_passes{
    std::string_view{"adapter"}, std::string_view{"render"},
    std::string_view{"readback"}, std::string_view{"content"}};
constexpr std::array compute_passes{
    std::string_view{"adapter"}, std::string_view{"dispatch"},
    std::string_view{"oracle"}};
constexpr std::array stft_passes{
    std::string_view{"prepare"}, std::string_view{"forward"},
    std::string_view{"magnitude"}, std::string_view{"oracle"}};
constexpr std::array threejs_passes{
    std::string_view{"adapter"}, std::string_view{"threejs-init"},
    std::string_view{"background-pass"}, std::string_view{"left-swatch-pass"},
    std::string_view{"final-swatch-pass"}, std::string_view{"oracle"}};

constexpr Dimensions renderer_negative_dimensions{32, 32, 1'024};

constexpr std::array registry{
    RecipeDefinition{kRecipeIds[0], "pulp.renderer3d.hardcoded-cube", {128, 128, 16'384},
                     0x52454e4433440001ULL, "fixed-step-0", "rgba32float", "rgba8-srgb",
                     "png", {1.0 / 255.0, 0.0}, AdapterPolicy::hardware_preferred,
                     renderer_passes, {}, "content-fingerprint-match",
                     "pre-submit-framebuffer-downscale"},
    RecipeDefinition{kRecipeIds[1], "pulp.gpu-compute.magnitude", {256, 1, 256},
                     0x434f4d5055544501ULL, "fixed-step-0", "complex-f32", "f32",
                     "little-endian-f32", {1.0e-5, 1.0e-5}, AdapterPolicy::hardware_required,
                     compute_passes, {}, "cpu-sqrt-re2-im2-match", "wgsl-imaginary-weight"},
    RecipeDefinition{kRecipeIds[2], "pulp.gpu-audio.GpuStft.analyze/fft_stockham",
                     {1024, 1, 1024}, 0x535446544f464601ULL,
                     "offline-sample-index@48000Hz", "f32-mono", "complex-f32",
                     "little-endian-f32", {1.0e-2, 1.0e-2},
                     AdapterPolicy::hardware_required, stft_passes,
                     {1024, 6, 8192, 32 * 1024}, "cpu-fft-magnitude-match",
                     "stockham-stage-output-half"},
    RecipeDefinition{kRecipeIds[3], "three.js@077dd13c/native-webgpu-multi-pass",
                     {96, 96, 9'216}, 0x54485245454a5301ULL,
                     "fixed-step-0", "threejs-esm-scene", "rgba8-srgb",
                     "row-major-rgba8", {32.0 / 255.0, 0.0},
                     AdapterPolicy::hardware_required, threejs_passes,
                     {15, 4, 40 * 1024, 128 * 1024},
                     "cpu-region-color-oracle-match",
                     "seeded-final-swatch-channel"},
};

bool is_lower_hex(std::string_view value, std::size_t size) {
    return value.size() == size && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

bool finite_nonnegative(double value) {
    return std::isfinite(value) && value >= 0.0;
}

bool safe_relative_name(std::string_view value) {
    if (value.empty() || value.size() > 240) return false;
    const std::filesystem::path path(value);
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory()) return false;
    for (const auto& part : path)
        if (part == ".." || part == ".") return false;
    return path.lexically_normal().generic_string() == value;
}

bool fail(std::string* error, std::string message) {
    if (error) *error = std::move(message);
    return false;
}

const choc::value::Value& recipe_catalog() {
    static const auto catalog = [] {
        auto parsed = choc::json::parse(detail::kCatalogJson);
        if (!parsed.isObject() || !parsed.hasObjectMember("schema") ||
            !parsed["schema"].isString() ||
            parsed["schema"].getString() != "pulp.gpu-recipes.v1" ||
            !parsed.hasObjectMember("catalog_revision") ||
            parsed["catalog_revision"].getInt64() != 1 ||
            !parsed.hasObjectMember("recipes") || !parsed["recipes"].isArray())
            throw std::runtime_error("embedded GPU recipe catalog is malformed");

        const auto rows = parsed["recipes"];
        if (rows.size() != registry.size())
            throw std::runtime_error("embedded GPU recipe catalog omits native recipes");
        std::array<bool, registry.size()> seen{};
        for (uint32_t index = 0; index < rows.size(); ++index) {
            const auto row = rows[index];
            if (!row.isObject() || !row.hasObjectMember("id") || !row["id"].isString() ||
                !row.hasObjectMember("native_registry_index") ||
                !(row["native_registry_index"].isInt32() ||
                  row["native_registry_index"].isInt64()))
                throw std::runtime_error("embedded GPU recipe row is malformed");
            const auto native_index = row["native_registry_index"].getInt64();
            if (native_index < 0 || static_cast<std::size_t>(native_index) >= registry.size() ||
                seen[static_cast<std::size_t>(native_index)] ||
                row["id"].getString() != registry[static_cast<std::size_t>(native_index)].id)
                throw std::runtime_error("embedded GPU recipe catalog disagrees with registry");
            seen[static_cast<std::size_t>(native_index)] = true;
        }
        return parsed;
    }();
    return catalog;
}

bool row_matches_symptom(const choc::value::ValueView& row, std::string_view symptom) {
    if (!row.hasObjectMember("symptoms") || !row["symptoms"].isArray())
        return false;
    const auto symptoms = row["symptoms"];
    for (uint32_t index = 0; index < symptoms.size(); ++index)
        if (symptoms[index].isString() && symptoms[index].getString() == symptom)
            return true;
    return false;
}

} // namespace

std::string_view to_string(Verdict value) {
    switch (value) {
        case Verdict::pass: return "pass";
        case Verdict::fail: return "fail";
        case Verdict::unavailable: return "unavailable";
        case Verdict::unverified: return "unverified";
    }
    return "unverified";
}

std::string_view to_string(AdapterPolicy value) {
    switch (value) {
        case AdapterPolicy::hardware_required: return "hardware-required";
        case AdapterPolicy::hardware_preferred: return "hardware-preferred";
        case AdapterPolicy::any_supported: return "any-supported";
    }
    return "hardware-required";
}

std::string_view to_string(AdapterClass value) {
    switch (value) {
        case AdapterClass::hardware: return "hardware";
        case AdapterClass::software: return "software";
        case AdapterClass::null_adapter: return "null";
        case AdapterClass::unknown: return "unknown";
    }
    return "unknown";
}

std::string_view to_string(IdentityStatus value) {
    switch (value) {
        case IdentityStatus::authentic: return "authentic";
        case IdentityStatus::unverified: return "unverified";
        case IdentityStatus::unavailable: return "unavailable";
    }
    return "unavailable";
}

std::string_view to_string(ArtifactKind value) {
    switch (value) {
        case ArtifactKind::json: return "json";
        case ArtifactKind::image: return "image";
        case ArtifactKind::numeric_samples: return "numeric-samples";
        case ArtifactKind::trace: return "trace";
    }
    return "json";
}

const RecipeDefinition* find_recipe(std::string_view id) {
    const auto found = std::find_if(registry.begin(), registry.end(),
                                    [&](const auto& recipe) { return recipe.id == id; });
    return found == registry.end() ? nullptr : &*found;
}

bool is_recipe_callable(std::string_view id) {
    if (find_recipe(id) == nullptr)
        return false;
    if (id == kRecipeIds[0])
        return PULP_GPU_PROBE_RENDERER3D_CALLABLE != 0;
    if (id == kRecipeIds[1] || id == kRecipeIds[2])
        return PULP_GPU_PROBE_GPU_CALLABLE != 0;
    if (id == kRecipeIds[3])
        return PULP_GPU_PROBE_THREEJS_CALLABLE != 0;
    return false;
}

std::span<const RecipeDefinition> recipes() { return registry; }

std::string_view recipe_catalog_json() { return detail::kCatalogJson; }

std::optional<std::string> recipe_discovery_json(
    std::optional<std::string_view> recipe_id,
    std::optional<std::string_view> symptom) {
    if (recipe_id && symptom)
        return std::nullopt;

    const auto& catalog = recipe_catalog();
    const auto rows = catalog["recipes"];
    std::string output =
        "{\"schema\":\"pulp.gpu-recipes-discovery.v1\",\"catalog_revision\":1,\"recipes\":[";
    bool matched = false;
    for (uint32_t index = 0; index < rows.size(); ++index) {
        const auto row = rows[index];
        const std::string_view id = row["id"].getString();
        if ((recipe_id && id != *recipe_id) ||
            (symptom && !row_matches_symptom(row, *symptom)))
            continue;
        if (matched)
            output += ',';
        output += "{\"callable\":";
        output += is_recipe_callable(id) ? "true" : "false";
        output += ",\"recipe\":" + choc::json::toString(row, false) + '}';
        matched = true;
    }
    if ((recipe_id || symptom) && !matched)
        return std::nullopt;
    return output + "]}";
}

bool validate(const ProbeResult& result, std::string* error) {
    if (error) error->clear();
    if (result.schema != kSchema) return fail(error, "schema must be pulp.gpu-probe-result.v1");
    if (result.version != kVersion) return fail(error, "version must be 1");
    if (!is_lower_hex(result.gpu_evidence_id, 32))
        return fail(error, "gpu_evidence_id must be 128-bit lowercase hex");
    if (!is_lower_hex(result.source_digest, 64) || !is_lower_hex(result.signature_digest, 64))
        return fail(error, "source and signature digests must be SHA-256 lowercase hex");

    const auto* recipe = find_recipe(result.recipe_id);
    if (!recipe) return fail(error, "recipe_id is not registered");
    if (result.mutation &&
        (result.mutation->empty() || result.mutation->size() > 128 ||
         *result.mutation != recipe->negative_mutation))
        return fail(error, "mutation must be absent or the exact bounded recipe mutation");
    const bool renderer_negative = recipe->id == kRecipeIds[0] && result.mutation;
    const auto& expected_dimensions = renderer_negative
        ? renderer_negative_dimensions : recipe->dimensions;
    if (result.dimensions.width != expected_dimensions.width ||
        result.dimensions.height != expected_dimensions.height ||
        result.dimensions.work_items != expected_dimensions.work_items ||
        result.seed != recipe->seed || result.clock != recipe->clock ||
        result.input_format != recipe->input_format || result.output_format != recipe->output_format ||
        result.encoding != recipe->encoding || result.adapter_policy != recipe->adapter_policy)
        return fail(error, "result execution identity does not match the registered recipe");
    if (result.dimensions.width == 0 || result.dimensions.width > kMaxDimension ||
        result.dimensions.height == 0 || result.dimensions.height > kMaxDimension ||
        result.dimensions.work_items == 0 || result.dimensions.work_items > kMaxWorkItems)
        return fail(error, "recipe dimensions exceed the global bound");
    if (!finite_nonnegative(result.tolerance.absolute) ||
        !finite_nonnegative(result.tolerance.relative) ||
        result.tolerance.absolute != recipe->tolerance.absolute ||
        result.tolerance.relative != recipe->tolerance.relative)
        return fail(error, "tolerance must be finite, nonnegative, and recipe-bound");
    if (result.adapter_policy == AdapterPolicy::hardware_required &&
        result.verdict == Verdict::pass &&
        (result.adapter.status != IdentityStatus::authentic ||
         result.adapter.classification != AdapterClass::hardware))
        return fail(error, "hardware-required pass needs authentic hardware adapter identity");
    if (result.adapter.classification == AdapterClass::null_adapter &&
        result.verdict == Verdict::pass)
        return fail(error, "a null adapter cannot produce a passing probe result");
    const auto check_identity_field = [&](const auto& field) {
        return !field || (!field->empty() && field->size() <= 256);
    };
    if (!check_identity_field(result.adapter.backend) ||
        !check_identity_field(result.adapter.name) ||
        !check_identity_field(result.adapter.vendor) ||
        !check_identity_field(result.adapter.architecture) ||
        !check_identity_field(result.adapter.device))
        return fail(error, "adapter identity fields must be bounded nonempty strings");
    if (result.numeric_sample_count > recipe->bounds.max_numeric_samples)
        return fail(error, "numeric sample count exceeds the recipe bound");
    if (result.passes.size() != recipe->semantic_passes.size())
        return fail(error, "semantic pass count does not match the recipe");

    bool all_pass = true;
    Verdict aggregate = Verdict::pass;
    for (std::size_t i = 0; i < result.passes.size(); ++i) {
        const auto& pass = result.passes[i];
        if (pass.sequence != i || pass.name != recipe->semantic_passes[i])
            return fail(error, "semantic passes must be contiguous and recipe-ordered");
        if (pass.code.empty() || pass.code.size() > 128)
            return fail(error, "semantic pass code is missing or too long");
        if (pass.verdict == Verdict::pass && !pass.work_completed)
            return fail(error, "a passing semantic pass must prove work completed");
        if (pass.expected.has_value() != pass.observed.has_value())
            return fail(error, "numeric expected and observed values must appear together");
        if (pass.expected && (!std::isfinite(*pass.expected) || !std::isfinite(*pass.observed)))
            return fail(error, "numeric evidence must be finite");
        if (pass.expected.has_value() != pass.absolute_error.has_value())
            return fail(error, "numeric evidence requires an absolute error");
        if (pass.absolute_error) {
            if (!finite_nonnegative(*pass.absolute_error))
                return fail(error, "absolute error must be finite and nonnegative");
            const double calculated = std::abs(*pass.observed - *pass.expected);
            const double epsilon = std::numeric_limits<double>::epsilon() *
                                   std::max({1.0, calculated, *pass.absolute_error}) * 8.0;
            if (std::abs(calculated - *pass.absolute_error) > epsilon)
                return fail(error, "absolute error does not match observed minus expected");
        }
        all_pass = all_pass && pass.verdict == Verdict::pass;
        if (pass.verdict == Verdict::fail)
            aggregate = Verdict::fail;
        else if (aggregate != Verdict::fail && pass.verdict == Verdict::unavailable)
            aggregate = Verdict::unavailable;
        else if (aggregate == Verdict::pass && pass.verdict == Verdict::unverified)
            aggregate = Verdict::unverified;
    }
    if ((result.verdict == Verdict::pass) != all_pass || result.verdict != aggregate)
        return fail(error, "top-level verdict must aggregate the semantic pass verdicts");

    if (result.artifacts.size() > recipe->bounds.max_artifacts)
        return fail(error, "artifact count exceeds the recipe bound");
    std::unordered_set<std::string> names;
    std::uint64_t total = 0;
    for (const auto& artifact : result.artifacts) {
        if (!safe_relative_name(artifact.name))
            return fail(error, "artifact name must be a confined normalized relative path");
        if (!names.insert(artifact.name).second)
            return fail(error, "artifact names must be unique");
        if (artifact.mime.empty() || artifact.mime.size() > 128)
            return fail(error, "artifact MIME is missing or too long");
        if (artifact.bytes > recipe->bounds.max_artifact_bytes)
            return fail(error, "artifact exceeds the per-artifact byte bound");
        if (!is_lower_hex(artifact.sha256, 64))
            return fail(error, "artifact sha256 must be lowercase hex");
        if (total > recipe->bounds.max_total_artifact_bytes - artifact.bytes)
            return fail(error, "artifacts exceed the total byte bound");
        total += artifact.bytes;
    }
    if (recipe->id == kRecipeIds[0]) {
        const auto rgba = std::find_if(result.artifacts.begin(), result.artifacts.end(),
                                       [](const auto& artifact) {
                                           return artifact.name == "observed.rgba8";
                                       });
        if (rgba != result.artifacts.end()) {
            const auto expected_bytes = result.dimensions.work_items * 4;
            if (rgba->kind != ArtifactKind::image ||
                rgba->mime != "application/octet-stream" ||
                rgba->bytes != expected_bytes)
                return fail(error, "observed RGBA artifact must match declared dimensions");
        }
    }
    if (result.recommendations.size() > 16)
        return fail(error, "recommendation count exceeds the bound");
    for (const auto& recommendation : result.recommendations)
        if (recommendation.empty() || recommendation.size() > 512)
            return fail(error, "recommendation is missing or too long");
    return true;
}

} // namespace pulp::tooling::gpu_probe
