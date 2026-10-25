// generate_pulp_js for a design that ships captured @font-face faces: each
// family's unicode-range segments are registered under the same private
// aliases the native materializer uses, and every font stack is rewritten to
// name them, so generated ui.js shapes text with the page's own fonts.
#include "design_import_native_common.hpp"

#include <pulp/view/design_codegen.hpp>

#include <algorithm>
#include <cctype>
#include <map>
#include <string>

namespace pulp::view {

namespace {

std::string lowered(std::string value) {
    for (auto& c : value)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

} // namespace

bool shipped_font_is_aliased(const std::string& family,
                             const std::vector<ShippedFontSegment>& segments) {
    const auto key = lowered(family);
    return std::any_of(segments.begin(), segments.end(),
                       [&](const ShippedFontSegment& s) { return s.family_key == key; });
}

std::string generate_pulp_js(const DesignIR& ir, const CodeGenOptions& opts) {
    const auto segments = plan_shipped_font_segments(ir, ir.asset_manifest, {}, true);
    if (segments.empty())
        return generate_pulp_js_document(ir, opts, segments);
    // The plan's face pointers index `ir`; the copy only has its stacks
    // rewritten, so its font_family_assets match entry for entry.
    DesignIR rewritten = ir;
    std::map<std::string, std::string> stacks;
    for (const auto& segment : segments) {
        auto& stack = stacks[segment.family_key];
        if (!stack.empty())
            stack += ", ";
        stack += "\"" + segment.alias + "\"";
    }
    rewrite_shipped_font_stacks(rewritten.root, stacks);
    return generate_pulp_js_document(rewritten, opts, segments);
}

} // namespace pulp::view
