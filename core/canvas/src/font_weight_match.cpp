// font_weight_match.cpp — CSS weight matching over a Skia style set. See
// pulp/canvas/font_weight_match.hpp.
//
// Kept out of the header so css_weight_font_manager.cpp, which is built
// without RTTI, never instantiates library code that can throw. A throw site
// compiled without RTTI emits hidden local copies of the standard exception
// type infos, and the linker binds the whole image's catch clauses to those
// copies, so a std::runtime_error thrown elsewhere stops matching
// catch (const std::exception&).

#include <pulp/canvas/font_weight_match.hpp>

#ifdef PULP_HAS_SKIA

#include <vector>

namespace pulp::canvas {

sk_sp<SkTypeface> css_match_style(SkFontStyleSet& set, const SkFontStyle& style) {
    std::vector<int> weights;
    std::vector<int> indices;
    const int count = set.count();
    weights.reserve(static_cast<size_t>(count > 0 ? count : 0));
    indices.reserve(weights.capacity());
    for (int i = 0; i < count; ++i) {
        SkFontStyle have;
        set.getStyle(i, &have, nullptr);
        if (have.width() == style.width() && have.slant() == style.slant()) {
            weights.push_back(have.weight());
            indices.push_back(i);
        }
    }
    const int weight = css_match_font_weight(weights, style.weight());
    for (size_t k = 0; k < weights.size(); ++k) {
        if (weights[k] == weight)
            return set.createTypeface(indices[k]);
    }
    return nullptr;
}

sk_sp<SkTypeface> css_match_family_style(SkFontMgr& mgr, const char family[],
                                         const SkFontStyle& style) {
    if (family != nullptr && family[0] != '\0') {
        if (sk_sp<SkFontStyleSet> set = mgr.matchFamily(family)) {
            if (sk_sp<SkTypeface> face = css_match_style(*set, style))
                return face;
        }
    }
    return mgr.matchFamilyStyle(family, style);
}

} // namespace pulp::canvas

#endif // PULP_HAS_SKIA
