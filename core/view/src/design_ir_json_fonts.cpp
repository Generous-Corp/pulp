// The fonts a DesignIR ships (`fontFamilyAssets`): one entry per bundled face,
// or per captured @font-face rule with its CSS descriptors.
#include "design_ir_json_primitives.hpp"

#include <choc/text/choc_JSON.h>

namespace pulp::view {

using namespace ir_json;

void parse_ir_font_family_assets(DesignIR& ir, const choc::value::ValueView& root) {
    for (const char* fk : {"fontFamilyAssets", "font_family_assets"}) {
        if (!root.hasObjectMember(fk) || !root[fk].isArray())
            continue;
        const auto arr = root[fk];
        for (uint32_t i = 0; i < arr.size(); ++i) {
            const auto e = arr[static_cast<int>(i)];
            if (!e.isObject())
                continue;
            IRFontAsset fa;
            fa.family = get_string(e, "family");
            fa.style = get_string(e, "style");
            if (e.hasObjectMember("weight"))
                fa.weight = static_cast<int>(e["weight"].getWithDefault<int64_t>(400));
            fa.asset_id = get_string(e, "asset_id");
            if (fa.asset_id.empty())
                fa.asset_id = get_string(e, "assetId");
            fa.resolved_path = get_string(e, "resolvedPath");
            if (fa.resolved_path.empty())
                fa.resolved_path = get_string(e, "resolved_path");
            fa.font_style = get_string(e, "fontStyle");
            if (fa.font_style.empty())
                fa.font_style = get_string(e, "font_style");
            for (const char* wk : {"weightMax", "weight_max"}) {
                if (e.hasObjectMember(wk)) {
                    fa.weight_max = static_cast<int>(e[wk].getWithDefault<int64_t>(0));
                    break;
                }
            }
            fa.unicode_range = get_string(e, "unicodeRange");
            if (fa.unicode_range.empty())
                fa.unicode_range = get_string(e, "unicode_range");
            if (!fa.family.empty())
                ir.font_family_assets.push_back(std::move(fa));
        }
        break;
    }
}

void write_ir_font_family_assets(std::ostringstream& out, bool& first, const DesignIR& ir) {
    if (ir.font_family_assets.empty())
        return;
    write_key(out, first, "fontFamilyAssets");
    out << "[";
    bool ffirst = true;
    for (const auto& fa : ir.font_family_assets) {
        if (!ffirst)
            out << ",";
        ffirst = false;
        out << "{";
        bool mf = true;
        write_string_member(out, mf, "family", fa.family);
        if (!fa.style.empty())
            write_string_member(out, mf, "style", fa.style);
        write_key(out, mf, "weight");
        out << fa.weight;
        if (!fa.asset_id.empty())
            write_string_member(out, mf, "asset_id", fa.asset_id);
        if (!fa.resolved_path.empty())
            write_string_member(out, mf, "resolvedPath", fa.resolved_path);
        // Captured @font-face descriptors. Written only when set, so a
        // document without them serializes exactly as it did before.
        if (!fa.font_style.empty())
            write_string_member(out, mf, "fontStyle", fa.font_style);
        if (fa.weight_max > 0) {
            write_key(out, mf, "weightMax");
            out << fa.weight_max;
        }
        if (!fa.unicode_range.empty())
            write_string_member(out, mf, "unicodeRange", fa.unicode_range);
        out << "}";
    }
    out << "]";
}

} // namespace pulp::view
