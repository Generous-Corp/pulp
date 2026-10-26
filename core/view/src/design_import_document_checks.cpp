#include "design_import_document_checks.hpp"

#include <pulp/canvas/image_probe.hpp>
#include <pulp/runtime/log.hpp>
#include <pulp/view/design_import.hpp>

#include <algorithm>
#include <cctype>
#include <string>

namespace pulp::view {

namespace {

ImportDiagnostic warning(std::string code, std::string_view path, std::string message,
                         const IRNode& node, ImportDiagnosticKind kind) {
    ImportDiagnostic out;
    out.severity = ImportDiagnosticSeverity::warning;
    out.kind = kind;
    out.code = std::move(code);
    out.path = std::string(path);
    out.message = std::move(message);
    if (node.stable_anchor_id && !node.stable_anchor_id->empty())
        out.anchor_id = *node.stable_anchor_id;
    return out;
}

std::string lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool names_svg(const IRAssetRef& asset, std::string_view file_uri) {
    if (lower(asset.mime).find("svg") != std::string::npos)
        return true;
    const auto uri = lower(file_uri);
    return uri.size() >= 4 && (uri.ends_with(".svg") || uri.ends_with(".svgz"));
}

} // namespace

void append_document_version_diagnostic(const DesignIR& ir, std::vector<ImportDiagnostic>& out) {
    if (ir.version <= kDesignIrNewestKnownVersion)
        return;
    runtime::log_warn(
        "design-import: unsupported DesignIR version {} (this build knows versions up to {}); "
        "its fields are read as version {} defines them",
        ir.version, kDesignIrNewestKnownVersion, kDesignIrNewestKnownVersion);
    out.push_back(warning("document-version-unsupported", "$",
                          "unsupported DesignIR version " + std::to_string(ir.version), ir.root,
                          ImportDiagnosticKind::unknown));
}

void append_unknown_node_type_diagnostic(const IRNode& node, std::string_view path,
                                         std::vector<ImportDiagnostic>& out) {
    runtime::log_warn("design-import: unknown node type '{}' at {}", node.type, path);
    out.push_back(warning("node-type-unknown", path, "unknown node type '" + node.type + "'", node,
                          ImportDiagnosticKind::unsupported_node));
}

void append_duplicate_anchor_diagnostics(const IRNode& root, std::vector<ImportDiagnostic>& out) {
    for (const auto& duplicate : find_duplicate_anchors(root)) {
        runtime::log_warn("design-import: duplicate stable anchor '{}' at {} (first at {}); "
                          "bindings and re-import tweaks reach only the first",
                          duplicate.anchor, duplicate.path, duplicate.first_path);
        ImportDiagnostic diagnostic;
        diagnostic.severity = ImportDiagnosticSeverity::warning;
        diagnostic.kind = ImportDiagnosticKind::unknown;
        diagnostic.code = "identity-duplicate";
        diagnostic.path = duplicate.path;
        diagnostic.message = "duplicate stable anchor '" + duplicate.anchor + "' (first at " +
                             duplicate.first_path + ")";
        diagnostic.anchor_id = duplicate.anchor;
        out.push_back(std::move(diagnostic));
    }
}

bool append_undecodable_image_diagnostic(const IRNode& node, std::string_view path,
                                         const IRAssetRef& asset, std::string_view file_uri,
                                         std::vector<ImportDiagnostic>& out) {
    constexpr std::string_view kFileScheme = "file://";
    if (!file_uri.starts_with(kFileScheme) || names_svg(asset, file_uri))
        return false;
    const std::string file(file_uri.substr(kFileScheme.size()));
    if (canvas::probe_image_file(file) != canvas::ImageFileProbe::undecodable)
        return false;
    runtime::log_warn(
        "design-import: could not decode image asset '{}' ({}); the node at {} renders blank",
        asset.asset_id, file, path);
    out.push_back(warning("asset-undecodable", path,
                          "could not decode image asset '" + asset.asset_id + "'", node,
                          ImportDiagnosticKind::unresolved_asset));
    return true;
}

} // namespace pulp::view
