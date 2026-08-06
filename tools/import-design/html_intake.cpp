#include "html_intake.hpp"

#include <algorithm>
#include <cctype>

namespace pulp::import_design {

namespace {

std::string lowercase(std::string_view value) {
    std::string out(value);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return out;
}

bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

bool begins_with_html_markup(std::string_view value) {
    constexpr std::string_view kUtf8Bom{"\xEF\xBB\xBF", 3};
    if (value.starts_with(kUtf8Bom)) value.remove_prefix(kUtf8Bom.size());
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return false;
    value.remove_prefix(first);
    return value.starts_with("<!doctype html") ||
           value.starts_with("<html") ||
           value.starts_with("<head") ||
           value.starts_with("<body") ||
           value.starts_with("<script") ||
           value.starts_with("<main") ||
           value.starts_with("<div") ||
           value.starts_with("<svg");
}

}  // namespace

bool looks_like_html_path(const std::filesystem::path& input_path) {
    const auto filename = lowercase(input_path.filename().string());
    const auto extension = lowercase(input_path.extension().string());
    return extension == ".html" || extension == ".htm" ||
           contains(filename, ".dc.html") ||
           contains(filename, ".dc.htm");
}

HtmlIntakeDecision classify_html_intake(
    const std::filesystem::path& input_path,
    std::string_view content) {
    // The decisive signals all live near HTML tag/script declarations. Cap the
    // lowercase copy so a multi-megabyte bundled application does not get
    // duplicated merely to choose the evaluator.
    constexpr std::size_t kFingerprintBytes = 512 * 1024;
    const auto prefix = content.substr(0, std::min(content.size(), kFingerprintBytes));
    const auto lower = lowercase(prefix);
    const bool html_extension = looks_like_html_path(input_path);
    const bool html_markup = begins_with_html_markup(lower);
    if (!html_extension && !html_markup)
        return {};

    const bool design_component =
        contains(lower, "type=\"text/x-dc\"") ||
        contains(lower, "type='text/x-dc'");
    const bool bundler =
        contains(lower, "__bundler/manifest") ||
        contains(lower, "__bundler/template") ||
        contains(lower, "__bundler/ext_resources");

    if (design_component && bundler) {
        return {HtmlExportShape::claude_project_bundle, true,
                "Claude project bundle containing a design component"};
    }
    if (design_component) {
        return {HtmlExportShape::claude_design_component, true,
                "Claude design-component HTML with local/runtime dependencies"};
    }
    if (bundler) {
        return {HtmlExportShape::claude_standalone_bundle, true,
                "Claude standalone/bundled HTML application"};
    }
    return {HtmlExportShape::generic_html, true,
            "runnable HTML document"};
}

const char* html_export_shape_name(HtmlExportShape shape) {
    switch (shape) {
        case HtmlExportShape::not_html: return "not-html";
        case HtmlExportShape::claude_project_bundle:
            return "claude-project-bundle";
        case HtmlExportShape::claude_design_component:
            return "claude-design-component";
        case HtmlExportShape::claude_standalone_bundle:
            return "claude-standalone-bundle";
        case HtmlExportShape::generic_html:
            return "generic-html";
    }
    return "not-html";
}

}  // namespace pulp::import_design
