#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace pulp::import_design {

/// Diagnostic shape only. Every recognized HTML shape uses the same Chromium
/// evaluator; this enum explains the decision without selecting a different
/// parser or asking the user for a fragile flag.
enum class HtmlExportShape {
    not_html,
    claude_project_bundle,
    claude_design_component,
    claude_standalone_bundle,
    generic_html,
};

struct HtmlIntakeDecision {
    HtmlExportShape shape = HtmlExportShape::not_html;
    bool use_browser = false;
    std::string reason;
};

/// Case-normalized direct-file HTML suffix check shared by source inference
/// and the content-aware intake classifier.
bool looks_like_html_path(const std::filesystem::path& input_path);

HtmlIntakeDecision classify_html_intake(
    const std::filesystem::path& input_path,
    std::string_view content);

const char* html_export_shape_name(HtmlExportShape shape);

}  // namespace pulp::import_design
