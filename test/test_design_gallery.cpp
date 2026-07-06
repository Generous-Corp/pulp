// Tests for the design-system gallery card model + review-artifact emitters.

#include <pulp/design/design_gallery.hpp>

#include <catch2/catch_test_macros.hpp>

#include <choc/text/choc_JSON.h>

using namespace pulp::design;

namespace {

std::string png_for(const GalleryCard& c) { return c.file + ".png"; }
std::string no_png(const GalleryCard&) { return {}; }

}  // namespace

TEST_CASE("parse_gallery_card reads group, viewport, and starting point", "[gallery]") {
    auto card = parse_gallery_card("// @dsCard group=knobs viewport=120x140\n// @startingPoint\n",
                                   "widgets/knob.js");
    REQUIRE(card.has_value());
    CHECK(card->file == "widgets/knob.js");
    CHECK(card->group == "knobs");
    CHECK(card->width == 120);
    CHECK(card->height == 140);
    CHECK(card->starting_point);
}

TEST_CASE("parse_gallery_card defaults group and starting point", "[gallery]") {
    auto card = parse_gallery_card("// @dsCard viewport=64x64\nconst x = 1;\n", "misc/x.js");
    REQUIRE(card.has_value());
    CHECK(card->group == std::string(kGalleryUngrouped));
    CHECK_FALSE(card->starting_point);
    CHECK(card->width == 64);
}

TEST_CASE("parse_gallery_card rejects untagged or viewport-less files", "[gallery]") {
    CHECK_FALSE(parse_gallery_card("const x = 1;\n", "plain.js").has_value());
    // @dsCard present but no viewport -> not a valid card.
    CHECK_FALSE(parse_gallery_card("// @dsCard group=foo\n", "foo.js").has_value());
    // Malformed viewport dimensions.
    CHECK_FALSE(parse_gallery_card("// @dsCard viewport=0x40\n", "z.js").has_value());
    CHECK_FALSE(parse_gallery_card("// @dsCard viewport=widexTall\n", "z.js").has_value());
}

TEST_CASE("parse_gallery_card does not read a viewport= from later code", "[gallery]") {
    // The tag line has no viewport; a `viewport=` further down must not leak in.
    auto card = parse_gallery_card("// @dsCard group=g\nconst css = 'viewport=800x600';\n", "a.js");
    CHECK_FALSE(card.has_value());
}

TEST_CASE("parse_gallery_card honors the tag only as a magic comment", "[gallery]") {
    // A @dsCard inside a string literal / real code must NOT opt the file in.
    CHECK_FALSE(parse_gallery_card("const s = \"@dsCard viewport=120x120\";\n", "a.js").has_value());
    // A block-comment continuation line (` * @dsCard`) is a valid magic comment.
    auto block = parse_gallery_card("/*\n * @dsCard group=g viewport=80x80\n */\n", "b.js");
    REQUIRE(block.has_value());
    CHECK(block->width == 80);
}

TEST_CASE("parse_gallery_card counts @startingPoint only on a comment line", "[gallery]") {
    // On a comment line -> seed.
    auto seed = parse_gallery_card("// @dsCard viewport=64x64\n// @startingPoint\n", "a.js");
    REQUIRE(seed.has_value());
    CHECK(seed->starting_point);
    // Mentioned in code, not a comment -> not a seed.
    auto no_seed = parse_gallery_card("// @dsCard viewport=64x64\nx = '@startingPoint';\n", "b.js");
    REQUIRE(no_seed.has_value());
    CHECK_FALSE(no_seed->starting_point);
}

TEST_CASE("parse_gallery_card matches keys only on word boundaries", "[gallery]") {
    // `notviewport=` must not satisfy the required viewport.
    CHECK_FALSE(parse_gallery_card("// @dsCard notviewport=120x120\n", "a.js").has_value());
    // A real viewport alongside a decoy prefix token still parses.
    auto card = parse_gallery_card("// @dsCard xgroup=bad viewport=32x48\n", "b.js");
    REQUIRE(card.has_value());
    CHECK(card->width == 32);
    CHECK(card->group == std::string(kGalleryUngrouped));  // xgroup= is not group=
}

TEST_CASE("gallery_content_hash is deterministic and content-sensitive", "[gallery]") {
    auto a = gallery_content_hash("hello world");
    CHECK(a == gallery_content_hash("hello world"));
    CHECK(a != gallery_content_hash("hello worlds"));
    CHECK(a.size() == 16);  // 64-bit -> 16 hex chars
    // Empty input still yields the FNV offset basis, not an empty string.
    CHECK(gallery_content_hash("").size() == 16);
}

TEST_CASE("sort_cards orders by group then file", "[gallery]") {
    std::vector<GalleryCard> cards = {
        {"z.js", "beta", 1, 1, false, ""},
        {"a.js", "beta", 1, 1, false, ""},
        {"m.js", "alpha", 1, 1, false, ""},
    };
    sort_cards(cards);
    CHECK(cards[0].group == "alpha");
    CHECK(cards[1].file == "a.js");
    CHECK(cards[2].file == "z.js");
}

TEST_CASE("gallery_manifest_json groups cards deterministically", "[gallery]") {
    std::vector<GalleryCard> cards = {
        {"b.js", "knobs", 100, 100, false, "hashb"},
        {"a.js", "knobs", 120, 90, true, "hasha"},
        {"c.js", "faders", 40, 200, false, "hashc"},
    };
    std::string json = gallery_manifest_json(cards, png_for);
    auto v = choc::json::parse(json);
    CHECK(v["format_version"].getString() == std::string(kGalleryFormatVersion));
    CHECK(v["total"].getWithDefault<int>(0) == 3);

    auto groups = v["groups"];
    REQUIRE(groups.size() == 2);
    // Groups are sorted: faders before knobs.
    CHECK(groups[0]["name"].getString() == "faders");
    CHECK(groups[0]["count"].getWithDefault<int>(0) == 1);
    CHECK(groups[1]["name"].getString() == "knobs");
    CHECK(groups[1]["count"].getWithDefault<int>(0) == 2);

    // Within knobs, cards are file-sorted; fields + derived png are present.
    auto knobs = groups[1]["cards"];
    CHECK(knobs[0]["file"].getString() == "a.js");
    CHECK(knobs[0]["viewport"].getString() == "120x90");
    CHECK(knobs[0]["starting_point"].getWithDefault<bool>(false) == true);
    CHECK(knobs[0]["content_hash"].getString() == "hasha");
    CHECK(knobs[0]["png"].getString() == "a.js.png");
}

TEST_CASE("gallery_manifest_json is byte-stable regardless of input order", "[gallery]") {
    std::vector<GalleryCard> a = {
        {"a.js", "g", 10, 10, false, "h1"},
        {"b.js", "g", 10, 10, false, "h2"},
    };
    std::vector<GalleryCard> b = {a[1], a[0]};
    CHECK(gallery_manifest_json(a, png_for) == gallery_manifest_json(b, png_for));
}

TEST_CASE("gallery_html renders images, placeholders, and escapes", "[gallery]") {
    std::vector<GalleryCard> cards = {
        {"panel.js", "main", 320, 240, true, "h"},
    };
    std::string html = gallery_html(cards, png_for);
    CHECK(html.find("<img src=\"panel.js.png\"") != std::string::npos);
    CHECK(html.find("width=\"320\"") != std::string::npos);
    CHECK(html.find("start") != std::string::npos);  // starting-point badge

    // A card with no rendered PNG shows a placeholder, never a broken <img>.
    std::string missing = gallery_html(cards, no_png);
    CHECK(missing.find("not rendered") != std::string::npos);
    CHECK(missing.find("<img") == std::string::npos);

    // Group/file names are HTML-escaped.
    std::vector<GalleryCard> nasty = {{"a<b>.js", "g&o", 10, 10, false, "h"}};
    std::string esc = gallery_html(nasty, no_png);
    CHECK(esc.find("g&amp;o") != std::string::npos);
    CHECK(esc.find("a&lt;b&gt;.js") != std::string::npos);
    CHECK(esc.find("a<b>.js") == std::string::npos);
}

TEST_CASE("gallery_html handles an empty gallery", "[gallery]") {
    std::string html = gallery_html({}, png_for);
    CHECK(html.find("No <code>@dsCard</code>") != std::string::npos);
}
