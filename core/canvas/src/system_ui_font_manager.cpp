// system_ui_font_manager.cpp — the system UI font for SkParagraph's font
// collection. See system_ui_typeface.hpp.
//
// Built without RTTI: it subclasses SkFontMgr, whose type info the prebuilt
// Skia does not export.

#include "system_ui_typeface.hpp"

#if defined(__APPLE__) && defined(PULP_HAS_SKIA)

#include "include/core/SkFontMgr.h"
#include "include/core/SkStream.h"
#include "include/core/SkString.h"

#include <strings.h>
#include <utility>

namespace pulp::canvas::detail {

namespace {

// The system UI font's styles at each hundred, upright and italic. They are
// what the family enumerates; a match is made at the exact requested style.
SkFontStyle system_ui_style_at(int index) {
    const int weight = 100 * (index % 9 + 1);
    return SkFontStyle(weight, SkFontStyle::kNormal_Width,
                       index < 9 ? SkFontStyle::kUpright_Slant : SkFontStyle::kItalic_Slant);
}

class SystemUiFontStyleSet final : public SkFontStyleSet {
  public:
    int count() override {
        return 18;
    }
    void getStyle(int index, SkFontStyle* style, SkString* name) override {
        if (style)
            *style = system_ui_style_at(index);
        if (name)
            name->reset();
    }
    sk_sp<SkTypeface> createTypeface(int index) override {
        return system_ui_typeface(system_ui_style_at(index));
    }
    sk_sp<SkTypeface> matchStyle(const SkFontStyle& pattern) override {
        // Other widths fall through to the platform manager, which maps them.
        if (pattern.width() != SkFontStyle::kNormal_Width)
            return nullptr;
        return system_ui_typeface(pattern);
    }
};

bool names_system_ui_family(const char family[]) {
    return family && strcasecmp(family, kSystemUiFamily) == 0;
}

// A face registered under the system UI font's name keeps precedence, as it
// does in the resolver.
class SystemUiFontManager final : public SkFontMgr {
  public:
    explicit SystemUiFontManager(sk_sp<SkFontMgr> registered)
        : registered_(std::move(registered)) {}

  protected:
    int onCountFamilies() const override {
        return 1;
    }
    void onGetFamilyName(int index, SkString* family) const override {
        if (family)
            family->set(index == 0 ? kSystemUiFamily : "");
    }
    sk_sp<SkFontStyleSet> onCreateStyleSet(int index) const override {
        return index == 0 ? sk_make_sp<SystemUiFontStyleSet>() : nullptr;
    }
    sk_sp<SkFontStyleSet> onMatchFamily(const char family[]) const override {
        if (!names_system_ui_family(family) || registers(family))
            return nullptr;
        return sk_make_sp<SystemUiFontStyleSet>();
    }
    sk_sp<SkTypeface> onMatchFamilyStyle(const char family[],
                                         const SkFontStyle& style) const override {
        if (!names_system_ui_family(family) || registers(family) ||
            style.width() != SkFontStyle::kNormal_Width)
            return nullptr;
        return system_ui_typeface(style);
    }
    sk_sp<SkTypeface> onMatchFamilyStyleCharacter(const char[], const SkFontStyle&, const char*[],
                                                  int, SkUnichar) const override {
        return nullptr;
    }
    sk_sp<SkTypeface> onMakeFromData(sk_sp<SkData>, int) const override {
        return nullptr;
    }
    sk_sp<SkTypeface> onMakeFromStreamIndex(std::unique_ptr<SkStreamAsset>, int) const override {
        return nullptr;
    }
    sk_sp<SkTypeface> onMakeFromStreamArgs(std::unique_ptr<SkStreamAsset>,
                                           const SkFontArguments&) const override {
        return nullptr;
    }
    sk_sp<SkTypeface> onMakeFromFile(const char[], int) const override {
        return nullptr;
    }
    sk_sp<SkTypeface> onLegacyMakeTypeface(const char[], SkFontStyle) const override {
        return nullptr;
    }

  private:
    bool registers(const char family[]) const {
        const sk_sp<SkFontStyleSet> set = registered_ ? registered_->matchFamily(family) : nullptr;
        return set && set->count() > 0;
    }

    sk_sp<SkFontMgr> registered_;
};

} // namespace

sk_sp<SkFontMgr> make_system_ui_font_manager(sk_sp<SkFontMgr> registered) {
    return sk_make_sp<SystemUiFontManager>(std::move(registered));
}

} // namespace pulp::canvas::detail

#endif
