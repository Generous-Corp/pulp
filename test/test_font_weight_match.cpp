// CSS font-weight matching (pulp/canvas/font_weight_match.hpp): the spec's
// search order as a table, and the faces it picks from real platform families
// through every path that picks one (the resolver, the paragraph collection's
// font manager, and resolved_face_identity()).

#include <catch2/catch_test_macros.hpp>

#include <pulp/canvas/font_weight_match.hpp>

#include <array>
#include <string>
#include <vector>

#ifdef PULP_HAS_SKIA
#include <pulp/canvas/bundled_fonts.hpp>
#include <pulp/canvas/font_resolver.hpp>
#include <pulp/canvas/text_font_context.hpp>

#include "include/core/SkFontMgr.h"
#include "include/core/SkFontStyle.h"
#include "include/core/SkString.h"
#include "include/core/SkTypeface.h"
#include "modules/skparagraph/include/FontCollection.h"
#endif

using pulp::canvas::css_match_font_weight;

namespace {

struct Row {
    int desired;
    int expected;
};

void check_table(const std::vector<int>& available, std::initializer_list<Row> rows) {
    for (const Row& row : rows) {
        INFO("available weights: " << available.size() << " faces, desired " << row.desired);
        CHECK(css_match_font_weight(available, row.desired) == row.expected);
    }
}

} // namespace

TEST_CASE("css weight matching follows the spec order for every hundred",
          "[canvas][font][weight-match]") {
    // A family with no 400, 600 or 800, so every rule branch has to search.
    check_table({100, 300, 500, 700, 900}, {
                                               {100, 100},
                                               {200, 100},
                                               {300, 300}, // below 400: lighter first
                                               {400, 500}, // 400..500: up to 500 first
                                               {500, 500},
                                               {600, 700},
                                               {700, 700},
                                               {800, 900},
                                               {900, 900}, // above 500: heavier first
                                           });
    // Nothing on the preferred side: each rule falls back to the other side.
    check_table({100, 200}, {{300, 200}, {400, 200}, {600, 200}, {900, 200}});
    check_table({800, 900}, {{100, 800}, {300, 800}, {450, 800}, {600, 800}});
}

TEST_CASE("css weight matching for 400 and 500 searches up to 500 before going lighter",
          "[canvas][font][weight-match]") {
    // 450 takes 500 when present, then anything lighter, and only then heavier.
    check_table({300, 500, 700}, {{450, 500}});
    check_table({300, 700}, {{450, 300}, {400, 300}, {500, 300}});
    check_table({700}, {{400, 700}, {500, 700}});
}

TEST_CASE("css weight matching prefers the heavier face above 500 where nearest would not",
          "[canvas][font][weight-match]") {
    // The Helvetica Neue shape: Medium and Bold, no 600. Nearest is a tie at
    // 100 either way (and CoreText's trait scale breaks it toward Medium);
    // CSS takes the heavier face.
    check_table({100, 200, 300, 400, 500, 700}, {{600, 700}, {550, 700}, {650, 700}});
    // Below 400 the direction flips: 350 takes 300, not 400.
    check_table({300, 400}, {{350, 300}});
}

TEST_CASE("css weight matching returns an exact weight and refuses an empty family",
          "[canvas][font][weight-match]") {
    for (int w = 100; w <= 900; w += 100)
        check_table({100, 200, 300, 400, 500, 600, 700, 800, 900}, {{w, w}});
    CHECK(css_match_font_weight(std::vector<int>{}, 400) == -1);
}

#if defined(PULP_HAS_SKIA) && defined(__APPLE__)

namespace {

std::string postscript_name(const sk_sp<SkTypeface>& face) {
    if (!face)
        return {};
    SkString name;
    face->getPostScriptName(&name);
    return std::string(name.c_str(), name.size());
}

SkFontStyle upright(int weight) {
    return SkFontStyle(weight, SkFontStyle::kNormal_Width, SkFontStyle::kUpright_Slant);
}

// The face every path that picks one reports for `family` at `weight`.
struct Picks {
    std::string matcher;
    std::string paragraph;
    std::string identity;
};

Picks picks_for(const char* family, int weight) {
    auto ctx = pulp::canvas::TextFontContext::shared();
    auto mgr = ctx->font_manager();
    REQUIRE(mgr);
    Picks out;
    out.matcher =
        postscript_name(pulp::canvas::css_match_family_style(*mgr, family, upright(weight)));
    auto collection = ctx->font_collection();
    REQUIRE(collection);
    const auto faces = collection->findTypefaces({SkString(family)}, upright(weight));
    out.paragraph = faces.empty() ? std::string{} : postscript_name(faces.front());
    out.identity = pulp::canvas::resolved_face_identity(std::string("\"") + family + "\"",
                                                        static_cast<float>(weight),
                                                        pulp::canvas::FontSlant::Normal);
    return out;
}

bool family_installed(const char* family, const char* face) {
    auto mgr = pulp::canvas::TextFontContext::shared()->font_manager();
    if (!mgr || !pulp::canvas::platform_font_db_usable())
        return false;
    auto set = mgr->matchFamily(family);
    if (!set)
        return false;
    for (int i = 0; i < set->count(); ++i) {
        if (postscript_name(set->createTypeface(i)) == face)
            return true;
    }
    return false;
}

} // namespace

TEST_CASE("Helvetica Neue 600 resolves to Bold on every path, as Chromium draws it",
          "[canvas][font][weight-match]") {
    if (!family_installed("Helvetica Neue", "HelveticaNeue-Bold") ||
        !family_installed("Helvetica Neue", "HelveticaNeue-Medium"))
        SKIP("Helvetica Neue Medium and Bold are not installed");
    const Picks picks = picks_for("Helvetica Neue", 600);
    CHECK(picks.matcher == "HelveticaNeue-Bold");
    CHECK(picks.paragraph == "HelveticaNeue-Bold");
    CHECK(picks.identity == "HelveticaNeue-Bold");
}

TEST_CASE("A family that ships the requested weight still gets that exact face",
          "[canvas][font][weight-match]") {
    if (!family_installed("Helvetica Neue", "HelveticaNeue-Medium"))
        SKIP("Helvetica Neue Medium is not installed");
    for (const auto& [weight, face] :
         std::array<std::pair<int, const char*>, 3>{{{500, "HelveticaNeue-Medium"},
                                                     {700, "HelveticaNeue-Bold"},
                                                     {400, "HelveticaNeue"}}}) {
        INFO("weight " << weight);
        const Picks picks = picks_for("Helvetica Neue", weight);
        CHECK(picks.matcher == face);
        CHECK(picks.paragraph == face);
        CHECK(picks.identity == face);
    }
}

TEST_CASE("Avenir Next between its weights resolves the CSS way on every path",
          "[canvas][font][weight-match]") {
    // Avenir Next ships Medium (500), Demi Bold (600) and Bold (700). The
    // platform's own matches disagree with each other here: its family match
    // and its style-set match pick different faces for 550 and 650.
    if (!family_installed("Avenir Next", "AvenirNext-DemiBold"))
        SKIP("Avenir Next is not installed");
    for (const auto& [weight, face] : std::array<std::pair<int, const char*>, 2>{
             {{550, "AvenirNext-DemiBold"}, {650, "AvenirNext-Bold"}}}) {
        INFO("weight " << weight);
        const Picks picks = picks_for("Avenir Next", weight);
        CHECK(picks.matcher == face);
        CHECK(picks.paragraph == face);
        CHECK(picks.identity == face);
    }
}

#endif // PULP_HAS_SKIA && __APPLE__
