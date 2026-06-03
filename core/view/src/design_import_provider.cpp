#include <pulp/view/design_import.hpp>
#include <pulp/view/view.hpp>
#include <pulp/runtime/crypto.hpp>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pulp::view {
namespace {

constexpr std::size_t kMaxProviderDiagnostics = 128;
constexpr std::size_t kMaxProviderDiagnosticFieldBytes = 4096;
constexpr std::size_t kMaxProviderProvenanceFieldBytes = 512;

ImportDiagnostic provider_diagnostic(ImportDiagnosticSeverity severity,
                                     std::string code,
                                     std::string path,
                                     std::string message,
                                     std::optional<std::string> property = std::nullopt) {
    ImportDiagnostic diagnostic;
    diagnostic.severity = severity;
    diagnostic.kind = severity == ImportDiagnosticSeverity::error
        ? ImportDiagnosticKind::fallback_used
        : ImportDiagnosticKind::snapshot_semantics_warning;
    diagnostic.code = std::move(code);
    diagnostic.path = std::move(path);
    diagnostic.message = std::move(message);
    diagnostic.property = std::move(property);
    return diagnostic;
}

int severity_rank(ImportDiagnosticSeverity severity) {
    switch (severity) {
        case ImportDiagnosticSeverity::error: return 0;
        case ImportDiagnosticSeverity::warning: return 1;
        case ImportDiagnosticSeverity::info: return 2;
    }
    return 3;
}

void sort_diagnostics(std::vector<ImportDiagnostic>& diagnostics) {
    std::stable_sort(diagnostics.begin(), diagnostics.end(),
                     [](const ImportDiagnostic& a, const ImportDiagnostic& b) {
                         if (severity_rank(a.severity) != severity_rank(b.severity))
                             return severity_rank(a.severity) < severity_rank(b.severity);
                         if (a.code != b.code) return a.code < b.code;
                         if (a.path != b.path) return a.path < b.path;
                         const auto a_anchor = a.anchor_id.value_or(std::string{});
                         const auto b_anchor = b.anchor_id.value_or(std::string{});
                         if (a_anchor != b_anchor) return a_anchor < b_anchor;
                         return a.message < b.message;
                     });
}

bool has_secret_shaped_text(std::string_view value) {
    return value.find("FIGMA_TOKEN") != std::string_view::npos
        || value.find("Authorization:") != std::string_view::npos
        || value.find("Bearer ") != std::string_view::npos
        || value.find("X-Figma-Token") != std::string_view::npos;
}

bool has_absolute_host_path(std::string_view value) {
    return value.rfind("/Users/", 0) == 0
        || value.rfind("/var/folders/", 0) == 0
        || value.rfind("/tmp/", 0) == 0
        || value.rfind("C:\\", 0) == 0;
}

std::string bounded_string(std::string value, std::size_t limit) {
    if (value.size() <= limit) return value;
    value.resize(limit);
    value += "...";
    return value;
}

std::vector<ImportDiagnostic> bounded_provider_diagnostics(
    const std::vector<ImportDiagnostic>& input,
    std::vector<ImportDiagnostic>& validation_diagnostics) {
    std::vector<ImportDiagnostic> out;
    const auto count = std::min(input.size(), kMaxProviderDiagnostics);
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        auto diagnostic = input[i];
        if (diagnostic.code.size() > kMaxProviderDiagnosticFieldBytes
            || diagnostic.path.size() > kMaxProviderDiagnosticFieldBytes
            || diagnostic.message.size() > kMaxProviderDiagnosticFieldBytes
            || (diagnostic.property && diagnostic.property->size() > kMaxProviderDiagnosticFieldBytes)
            || (diagnostic.anchor_id && diagnostic.anchor_id->size() > kMaxProviderDiagnosticFieldBytes)) {
            validation_diagnostics.push_back(provider_diagnostic(
                ImportDiagnosticSeverity::error,
                "provider-diagnostic-too-large",
                "$",
                "provider diagnostic exceeds the bounded field size"));
        }
        diagnostic.code = bounded_string(std::move(diagnostic.code), kMaxProviderDiagnosticFieldBytes);
        diagnostic.path = bounded_string(std::move(diagnostic.path), kMaxProviderDiagnosticFieldBytes);
        diagnostic.message = bounded_string(std::move(diagnostic.message), kMaxProviderDiagnosticFieldBytes);
        if (diagnostic.property)
            diagnostic.property = bounded_string(std::move(*diagnostic.property), kMaxProviderDiagnosticFieldBytes);
        if (diagnostic.anchor_id)
            diagnostic.anchor_id = bounded_string(std::move(*diagnostic.anchor_id), kMaxProviderDiagnosticFieldBytes);
        out.push_back(std::move(diagnostic));
    }
    if (input.size() > kMaxProviderDiagnostics) {
        validation_diagnostics.push_back(provider_diagnostic(
            ImportDiagnosticSeverity::error,
            "provider-diagnostic-count-exceeded",
            "$",
            "provider emitted too many diagnostics"));
    }
    return out;
}

bool is_relative_path_contained(const std::string& path) {
    if (path.empty()) return true;
    if (path.find('\\') != std::string::npos
        || path.rfind("//", 0) == 0
        || path.rfind("\\\\", 0) == 0
        || (path.size() >= 2
            && std::isalpha(static_cast<unsigned char>(path[0]))
            && path[1] == ':')) {
        return false;
    }
    const std::filesystem::path fs_path(path);
    if (fs_path.is_absolute()) return false;
    for (const auto& part : fs_path.lexically_normal()) {
        if (part == "..") return false;
    }
    return true;
}

bool is_asset_reference_key(std::string_view key) {
    return key == "asset_ref"
        || key == "assetRef"
        || key == "srcAssetId"
        || key == "backgroundImageAssetId"
        || key == "imageAssetId"
        || key == "maskImageAssetId";
}

void add_if_bad_float(std::vector<ImportDiagnostic>& diagnostics,
                      std::optional<float> value,
                      std::string path,
                      std::string property,
                      bool reject_negative = false) {
    if (!value) return;
    if (!std::isfinite(*value)) {
        diagnostics.push_back(provider_diagnostic(
            ImportDiagnosticSeverity::error,
            "provider-non-finite-geometry",
            std::move(path),
            "provider DesignIR contains non-finite geometry",
            std::move(property)));
    } else if (reject_negative && *value < 0.0f) {
        diagnostics.push_back(provider_diagnostic(
            ImportDiagnosticSeverity::error,
            "provider-invalid-geometry",
            std::move(path),
            "provider DesignIR contains a negative size",
            std::move(property)));
    }
}

void validate_node(const IRNode& node,
                   const IRAssetManifest& manifest,
                   const NativeDesignProviderValidationOptions& options,
                   std::string path,
                   std::set<std::string>& source_anchors,
                   std::set<std::string>& provider_anchors,
                   std::vector<ImportDiagnostic>& diagnostics) {
    if (node.type.empty()) {
        diagnostics.push_back(provider_diagnostic(
            ImportDiagnosticSeverity::error,
            "provider-empty-node-type",
            path,
            "provider DesignIR contains a node without a type"));
    }

    const auto& s = node.style;
    add_if_bad_float(diagnostics, s.opacity, path, "opacity");
    add_if_bad_float(diagnostics, s.border_radius, path, "border_radius", true);
    add_if_bad_float(diagnostics, s.border_width, path, "border_width", true);
    add_if_bad_float(diagnostics, s.border_top_width, path, "border_top_width", true);
    add_if_bad_float(diagnostics, s.border_right_width, path, "border_right_width", true);
    add_if_bad_float(diagnostics, s.border_bottom_width, path, "border_bottom_width", true);
    add_if_bad_float(diagnostics, s.border_left_width, path, "border_left_width", true);
    add_if_bad_float(diagnostics, s.border_top_left_radius, path, "border_top_left_radius", true);
    add_if_bad_float(diagnostics, s.border_top_right_radius, path, "border_top_right_radius", true);
    add_if_bad_float(diagnostics, s.border_bottom_right_radius, path, "border_bottom_right_radius", true);
    add_if_bad_float(diagnostics, s.border_bottom_left_radius, path, "border_bottom_left_radius", true);
    add_if_bad_float(diagnostics, s.font_size, path, "font_size", true);
    add_if_bad_float(diagnostics, s.letter_spacing, path, "letter_spacing");
    add_if_bad_float(diagnostics, s.line_height, path, "line_height", true);
    add_if_bad_float(diagnostics, s.top, path, "top");
    add_if_bad_float(diagnostics, s.left, path, "left");
    add_if_bad_float(diagnostics, s.right, path, "right");
    add_if_bad_float(diagnostics, s.bottom, path, "bottom");
    add_if_bad_float(diagnostics, s.width, path, "width", true);
    add_if_bad_float(diagnostics, s.height, path, "height", true);
    add_if_bad_float(diagnostics, s.min_width, path, "min_width", true);
    add_if_bad_float(diagnostics, s.min_height, path, "min_height", true);
    add_if_bad_float(diagnostics, s.max_width, path, "max_width", true);
    add_if_bad_float(diagnostics, s.max_height, path, "max_height", true);
    if (s.render_bounds) {
        if (!std::isfinite(s.render_bounds->w) || !std::isfinite(s.render_bounds->h)
            || !std::isfinite(s.render_bounds->dx) || !std::isfinite(s.render_bounds->dy)
            || s.render_bounds->w < 0.0f || s.render_bounds->h < 0.0f) {
            diagnostics.push_back(provider_diagnostic(
                ImportDiagnosticSeverity::error,
                "provider-invalid-geometry",
                path,
                "provider DesignIR contains invalid render bounds",
                "render_bounds"));
        }
    }

    const auto& l = node.layout;
    add_if_bad_float(diagnostics, l.gap, path, "gap");
    add_if_bad_float(diagnostics, l.row_gap, path, "row_gap");
    add_if_bad_float(diagnostics, l.column_gap, path, "column_gap");
    add_if_bad_float(diagnostics, l.padding_top, path, "padding_top");
    add_if_bad_float(diagnostics, l.padding_right, path, "padding_right");
    add_if_bad_float(diagnostics, l.padding_bottom, path, "padding_bottom");
    add_if_bad_float(diagnostics, l.padding_left, path, "padding_left");
    add_if_bad_float(diagnostics, l.margin_top, path, "margin_top");
    add_if_bad_float(diagnostics, l.margin_right, path, "margin_right");
    add_if_bad_float(diagnostics, l.margin_bottom, path, "margin_bottom");
    add_if_bad_float(diagnostics, l.margin_left, path, "margin_left");
    add_if_bad_float(diagnostics, l.flex_grow, path, "flex_grow");
    add_if_bad_float(diagnostics, l.flex_shrink, path, "flex_shrink");
    add_if_bad_float(diagnostics, l.aspect_ratio, path, "aspect_ratio", true);

    if (node.stable_anchor_id && !node.stable_anchor_id->empty()) {
        const bool provider_anchor = node.stable_anchor_id->rfind("provider:", 0) == 0;
        auto& anchors = provider_anchor ? provider_anchors : source_anchors;
        if (!anchors.insert(*node.stable_anchor_id).second) {
            diagnostics.push_back(provider_diagnostic(
                provider_anchor || options.reject_duplicate_source_anchors
                    ? ImportDiagnosticSeverity::error
                    : ImportDiagnosticSeverity::warning,
                provider_anchor ? "provider-duplicate-anchor" : "provider-duplicate-source-anchor",
                path,
                provider_anchor
                    ? "provider anchor ids must be unique"
                    : "source anchor id is duplicated in provider DesignIR",
                "stable_anchor_id"));
        }
    }

    for (const auto& [key, value] : node.attributes) {
        if (!is_asset_reference_key(key) || value.empty()) continue;
        if (manifest.resolve(value) == nullptr) {
            diagnostics.push_back(provider_diagnostic(
                ImportDiagnosticSeverity::error,
                "provider-unresolved-asset",
                path,
                "provider DesignIR references an asset id that is missing from IRAssetManifest",
                key));
        }
    }

    for (std::size_t i = 0; i < node.children.size(); ++i)
        validate_node(node.children[i],
                      manifest,
                      options,
                      path + ".children[" + std::to_string(i) + "]",
                      source_anchors,
                      provider_anchors,
                      diagnostics);
}

bool has_error(const std::vector<ImportDiagnostic>& diagnostics) {
    return std::any_of(diagnostics.begin(), diagnostics.end(), [](const ImportDiagnostic& diagnostic) {
        return diagnostic.severity == ImportDiagnosticSeverity::error;
    });
}

ImportDiagnostic provider_attempt_diagnostic(ImportDiagnosticSeverity severity,
                                             std::string code,
                                             std::string message,
                                             std::string_view provider_id) {
    auto diagnostic = provider_diagnostic(severity, std::move(code), "$", std::move(message), "provider");
    if (!provider_id.empty()) diagnostic.anchor_id = std::string(provider_id);
    return diagnostic;
}

std::string default_source_hash(const DesignIR& ir) {
    return "sha256:" + pulp::runtime::sha256_hex(serialize_design_ir(ir));
}

} // namespace

bool NativeDesignProviderRegistry::register_provider(std::shared_ptr<NativeDesignProvider> provider) {
    if (!provider) return false;
    const auto id = provider->provider_id();
    if (id.empty()) return false;
    if (find_provider(id) != nullptr) return false;
    providers_.push_back(std::move(provider));
    return true;
}

NativeDesignProvider* NativeDesignProviderRegistry::find_provider(std::string_view provider_id) const {
    if (provider_id.empty()) return nullptr;
    for (const auto& provider : providers_) {
        if (provider && provider->provider_id() == provider_id)
            return provider.get();
    }
    return nullptr;
}

std::vector<std::string> NativeDesignProviderRegistry::provider_ids() const {
    std::vector<std::string> ids;
    ids.reserve(providers_.size());
    for (const auto& provider : providers_) {
        if (provider) ids.push_back(provider->provider_id());
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

NativeDesignProviderValidationResult validate_native_design_provider_output(
    const NativeDesignProviderOutput& output,
    const NativeDesignProviderValidationOptions& options) {
    NativeDesignProviderValidationResult result;
    result.provenance = output.provenance;
    result.asset_manifest = output.asset_manifest;
    std::vector<ImportDiagnostic> provider_diagnostic_errors;
    result.diagnostics = bounded_provider_diagnostics(output.diagnostics, provider_diagnostic_errors);
    result.diagnostics.insert(result.diagnostics.end(),
                              provider_diagnostic_errors.begin(),
                              provider_diagnostic_errors.end());

    if (!output.ok) {
        result.diagnostics.push_back(provider_diagnostic(
            ImportDiagnosticSeverity::error,
            "provider-output-failed",
            "$",
            output.failure_reason.empty() ? "provider returned failure" : output.failure_reason));
    }
    if (output.canonical_design_ir_json.empty()) {
        result.diagnostics.push_back(provider_diagnostic(
            ImportDiagnosticSeverity::error,
            "provider-empty-design-ir",
            "$",
            "provider did not return canonical DesignIR JSON"));
    }
    if (!options.expected_provider_id.empty()
        && output.provenance.provider_id != options.expected_provider_id) {
        result.diagnostics.push_back(provider_diagnostic(
            ImportDiagnosticSeverity::error,
            "provider-id-mismatch",
            "$",
            "provider provenance id does not match the selected provider"));
    }
    if (output.provenance.provider_id.size() > kMaxProviderProvenanceFieldBytes
        || output.provenance.provider_version.size() > kMaxProviderProvenanceFieldBytes
        || output.provenance.source_design_hash.size() > kMaxProviderProvenanceFieldBytes) {
        result.diagnostics.push_back(provider_diagnostic(
            ImportDiagnosticSeverity::error,
            "provider-provenance-too-large",
            "$",
            "provider provenance exceeds the bounded field size"));
    }
    if (!options.source_design_hash.empty()
        && output.provenance.source_design_hash != options.source_design_hash) {
        result.diagnostics.push_back(provider_diagnostic(
            ImportDiagnosticSeverity::error,
            "provider-source-hash-mismatch",
            "$",
            "provider provenance source hash does not match the selected source"));
    }

    for (const auto& diagnostic : output.diagnostics) {
        if (has_secret_shaped_text(diagnostic.message) || has_secret_shaped_text(diagnostic.path)) {
            result.diagnostics.push_back(provider_diagnostic(
                ImportDiagnosticSeverity::error,
                "provider-diagnostic-secret",
                diagnostic.path.empty() ? "$" : diagnostic.path,
                "provider diagnostic contains secret-shaped text"));
        }
        if (has_absolute_host_path(diagnostic.path)) {
            result.diagnostics.push_back(provider_diagnostic(
                ImportDiagnosticSeverity::error,
                "provider-diagnostic-host-path",
                "$",
                "provider diagnostic contains an unrestricted absolute host path",
                "path"));
        }
    }

    DesignIR parsed;
    if (!output.canonical_design_ir_json.empty()) {
        try {
            parsed = parse_design_ir_json(output.canonical_design_ir_json);
            const IRAssetManifest effective_manifest = output.asset_manifest.assets.empty()
                ? parsed.asset_manifest
                : output.asset_manifest;
            parsed.asset_manifest = effective_manifest;
            result.asset_manifest = effective_manifest;
            result.canonical_design_ir_json = serialize_design_ir(parsed);
            if (options.require_canonical_json
                && result.canonical_design_ir_json != output.canonical_design_ir_json) {
                result.diagnostics.push_back(provider_diagnostic(
                    ImportDiagnosticSeverity::error,
                    "provider-noncanonical-json",
                    "$",
                    "provider DesignIR JSON does not match Pulp canonical serialization"));
            }
        } catch (const std::exception& e) {
            result.diagnostics.push_back(provider_diagnostic(
                ImportDiagnosticSeverity::error,
                "provider-json-parse-failed",
                "$",
                e.what()));
        } catch (...) {
            result.diagnostics.push_back(provider_diagnostic(
                ImportDiagnosticSeverity::error,
                "provider-json-parse-failed",
                "$",
                "unknown DesignIR parse failure"));
        }
    }

    if (!result.canonical_design_ir_json.empty()) {
        if (parsed.version != 1) {
            result.diagnostics.push_back(provider_diagnostic(
                ImportDiagnosticSeverity::error,
                "provider-unsupported-designir-version",
                "$",
                "provider DesignIR version is not supported"));
        }

        std::set<std::string> asset_ids;
        for (const auto& asset : result.asset_manifest.assets) {
            if (asset.asset_id.empty()) {
                result.diagnostics.push_back(provider_diagnostic(
                    ImportDiagnosticSeverity::error,
                    "provider-empty-asset-id",
                    "$.assetManifest",
                    "provider asset manifest contains an empty asset id"));
            } else if (!asset_ids.insert(asset.asset_id).second) {
                result.diagnostics.push_back(provider_diagnostic(
                    ImportDiagnosticSeverity::error,
                    "provider-duplicate-asset-id",
                    "$.assetManifest",
                    "provider asset manifest contains a duplicate asset id"));
            }
            if (asset.local_path && !is_relative_path_contained(*asset.local_path)) {
                result.diagnostics.push_back(provider_diagnostic(
                    ImportDiagnosticSeverity::error,
                    "provider-asset-path-escape",
                    "$.assetManifest",
                    "provider asset manifest path is not bundle-relative and contained"));
            }
            if (asset.content_hash.empty()) {
                result.diagnostics.push_back(provider_diagnostic(
                    ImportDiagnosticSeverity::warning,
                    "provider-asset-hash-missing",
                    "$.assetManifest",
                    "provider asset manifest entry does not include a content hash"));
            }
        }

        std::set<std::string> source_anchors;
        std::set<std::string> provider_anchors;
        validate_node(parsed.root,
                      result.asset_manifest,
                      options,
                      "$.root",
                      source_anchors,
                      provider_anchors,
                      result.diagnostics);
        result.design_ir = std::move(parsed);
    }

    sort_diagnostics(result.diagnostics);
    result.ok = !has_error(result.diagnostics);
    return result;
}

std::unique_ptr<View> build_native_view_tree_with_provider(
    const DesignIR& ir,
    const IRAssetManifest& manifest,
    const NativeMaterializeOptions& materialize_options,
    const NativeDesignProviderMaterializeOptions& provider_options) {
    if (provider_options.attempt_out != nullptr)
        *provider_options.attempt_out = {};

    if (provider_options.mode == NativeDesignProviderMode::baked_cpp_only) {
        return build_native_view_tree(ir, manifest, materialize_options);
    }

    const bool strict = provider_options.mode == NativeDesignProviderMode::provider_strict;
    const auto canonical_source = serialize_design_ir(ir);
    const auto source_hash = provider_options.source_design_hash.empty()
        ? default_source_hash(ir)
        : provider_options.source_design_hash;

    NativeDesignProviderAttempt attempt;
    attempt.strict_mode = strict;
    attempt.provider_id = provider_options.provider_id;
    attempt.source_design_hash = source_hash;

    auto publish_attempt = [&]() {
        sort_diagnostics(attempt.diagnostics);
        if (provider_options.attempt_out != nullptr)
            *provider_options.attempt_out = attempt;
    };

    auto fail_or_fallback = [&](std::string code, std::string message) -> std::unique_ptr<View> {
        auto diagnostic = provider_attempt_diagnostic(
            strict ? ImportDiagnosticSeverity::error : ImportDiagnosticSeverity::warning,
            std::move(code),
            std::move(message),
            provider_options.provider_id);
        attempt.failure_reason = diagnostic.message;
        attempt.diagnostics.push_back(diagnostic);
        if (materialize_options.diagnostics_out != nullptr)
            materialize_options.diagnostics_out->push_back(std::move(diagnostic));
        if (strict) {
            publish_attempt();
            return nullptr;
        }
        attempt.fallback_used = true;
        publish_attempt();
        return build_native_view_tree(ir, manifest, materialize_options);
    };

    if (provider_options.registry == nullptr) {
        return fail_or_fallback("provider-registry-missing",
                                "native design provider mode was requested without a registry");
    }
    if (provider_options.provider_id.empty()) {
        return fail_or_fallback("provider-id-missing",
                                "native design provider mode was requested without a provider id");
    }

    auto* provider = provider_options.registry->find_provider(provider_options.provider_id);
    if (provider == nullptr) {
        return fail_or_fallback("provider-not-found",
                                "native design provider id is not registered");
    }

    attempt.provider_attempted = true;
    attempt.provider_version = provider->provider_version();

    NativeDesignProviderRequest request;
    request.source_ir = &ir;
    request.source_manifest = &manifest;
    request.canonical_source_design_ir_json = canonical_source;
    request.source_design_hash = source_hash;
    request.strict_mode = strict;

    NativeDesignProviderOutput output;
    try {
        output = provider->import_design(request);
    } catch (const std::exception& e) {
        return fail_or_fallback("provider-threw", e.what());
    } catch (...) {
        return fail_or_fallback("provider-threw", "native design provider threw an unknown exception");
    }

    if (output.provenance.provider_id.empty())
        output.provenance.provider_id = provider->provider_id();
    if (output.provenance.provider_version.empty())
        output.provenance.provider_version = provider->provider_version();
    if (output.provenance.source_design_hash.empty())
        output.provenance.source_design_hash = source_hash;
    attempt.provider_version = output.provenance.provider_version;

    auto validation = validate_native_design_provider_output(
        output,
        {.expected_provider_id = provider_options.provider_id,
         .source_design_hash = source_hash,
         .reject_duplicate_source_anchors = provider_options.reject_duplicate_source_anchors});
    attempt.provider_version = validation.provenance.provider_version;
    attempt.diagnostics = validation.diagnostics;
    if (materialize_options.diagnostics_out != nullptr) {
        materialize_options.diagnostics_out->insert(materialize_options.diagnostics_out->end(),
                                                    validation.diagnostics.begin(),
                                                    validation.diagnostics.end());
    }
    if (!validation.ok) {
        attempt.failure_reason = validation.diagnostics.empty()
            ? "provider validation failed"
            : validation.diagnostics.front().message;
        if (strict) {
            publish_attempt();
            return nullptr;
        }
        attempt.fallback_used = true;
        publish_attempt();
        return build_native_view_tree(ir, manifest, materialize_options);
    }

    publish_attempt();
    return build_native_view_tree(validation.design_ir,
                                  validation.asset_manifest,
                                  materialize_options);
}

} // namespace pulp::view
