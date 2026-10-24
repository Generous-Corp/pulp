// css_weight_font_manager.cpp — a font manager that matches weights the way
// CSS does. See pulp/canvas/font_weight_match.hpp.
//
// Built without RTTI: it subclasses SkFontMgr and SkFontStyleSet, whose type
// info the prebuilt Skia does not export.
// Nothing here may instantiate library code that can throw (containers,
// strings): see font_weight_match.cpp for what that does to exception
// matching across the whole binary.

#include <pulp/canvas/font_weight_match.hpp>

#ifdef PULP_HAS_SKIA

#include "include/core/SkData.h"
#include "include/core/SkFontArguments.h"
#include "include/core/SkStream.h"
#include "include/core/SkString.h"

#include <memory>
#include <utility>

namespace pulp::canvas {

namespace {

class CssWeightFontStyleSet final : public SkFontStyleSet {
  public:
    explicit CssWeightFontStyleSet(sk_sp<SkFontStyleSet> base) : base_(std::move(base)) {}

    int count() override {
        return base_->count();
    }
    void getStyle(int index, SkFontStyle* style, SkString* name) override {
        base_->getStyle(index, style, name);
    }
    sk_sp<SkTypeface> createTypeface(int index) override {
        return base_->createTypeface(index);
    }
    sk_sp<SkTypeface> matchStyle(const SkFontStyle& pattern) override {
        if (sk_sp<SkTypeface> face = css_match_style(*base_, pattern))
            return face;
        return base_->matchStyle(pattern);
    }

  private:
    sk_sp<SkFontStyleSet> base_;
};

class CssWeightFontManager final : public SkFontMgr {
  public:
    explicit CssWeightFontManager(sk_sp<SkFontMgr> base) : base_(std::move(base)) {}

  protected:
    int onCountFamilies() const override {
        return base_->countFamilies();
    }
    void onGetFamilyName(int index, SkString* family) const override {
        base_->getFamilyName(index, family);
    }
    sk_sp<SkFontStyleSet> onCreateStyleSet(int index) const override {
        return wrap(base_->createStyleSet(index));
    }
    sk_sp<SkFontStyleSet> onMatchFamily(const char family[]) const override {
        return wrap(base_->matchFamily(family));
    }
    sk_sp<SkTypeface> onMatchFamilyStyle(const char family[],
                                         const SkFontStyle& style) const override {
        return css_match_family_style(*base_, family, style);
    }
    sk_sp<SkTypeface> onMatchFamilyStyleCharacter(const char family[], const SkFontStyle& style,
                                                  const char* bcp47[], int bcp47_count,
                                                  SkUnichar character) const override {
        return base_->matchFamilyStyleCharacter(family, style, bcp47, bcp47_count, character);
    }
    sk_sp<SkTypeface> onMakeFromData(sk_sp<SkData> data, int ttc_index) const override {
        return base_->makeFromData(std::move(data), ttc_index);
    }
    sk_sp<SkTypeface> onMakeFromStreamIndex(std::unique_ptr<SkStreamAsset> stream,
                                            int ttc_index) const override {
        return base_->makeFromStream(std::move(stream), ttc_index);
    }
    sk_sp<SkTypeface> onMakeFromStreamArgs(std::unique_ptr<SkStreamAsset> stream,
                                           const SkFontArguments& args) const override {
        return base_->makeFromStream(std::move(stream), args);
    }
    sk_sp<SkTypeface> onMakeFromFile(const char path[], int ttc_index) const override {
        return base_->makeFromFile(path, ttc_index);
    }
    sk_sp<SkTypeface> onLegacyMakeTypeface(const char family[], SkFontStyle style) const override {
        return base_->legacyMakeTypeface(family, style);
    }

  private:
    static sk_sp<SkFontStyleSet> wrap(sk_sp<SkFontStyleSet> set) {
        if (!set)
            return nullptr;
        return sk_make_sp<CssWeightFontStyleSet>(std::move(set));
    }

    sk_sp<SkFontMgr> base_;
};

} // namespace

sk_sp<SkFontMgr> make_css_weight_font_manager(sk_sp<SkFontMgr> base) {
    if (!base)
        return nullptr;
    return sk_make_sp<CssWeightFontManager>(std::move(base));
}

} // namespace pulp::canvas

#endif // PULP_HAS_SKIA
