// The header is included first on purpose: it must compile on its own.
#include <pulp/vellum/about_notices.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string_view>

namespace {

constexpr std::string_view kThreeNotices = R"json({
  "schema": "pulp.third-party-notices.v1",
  "product": "Example Synth",
  "target": "ExampleSynth",
  "format": "CLAP",
  "notices": [
    {"name": "Alpha", "version": "1.0", "license": "MIT",
     "license_text": "alpha license body"},
    {"name": "Beta", "version": "2.5.1", "license": "BSD-3-Clause",
     "license_text": "beta license body"},
    {"name": "Gamma", "version": "0.9", "license": "Apache-2.0",
     "license_text": "gamma license body"}
  ]
})json";

pulp::format::ThirdPartyNotices three() {
    return pulp::format::parse_third_party_notices(kThreeNotices);
}

} // namespace

TEST_CASE("About model lists every notice in manifest order", "[pulp-vellum-about]") {
    const auto model = pulp::vellum::make_about_model(three());
    REQUIRE(model.product == "Example Synth");
    REQUIRE(model.rows.size() == 3);
    CHECK(model.rows[0].name == "Alpha");
    CHECK(model.rows[0].version == "1.0");
    CHECK(model.rows[0].license == "MIT");
    CHECK(model.rows[1].name == "Beta");
    CHECK(model.rows[1].version == "2.5.1");
    CHECK(model.rows[1].license == "BSD-3-Clause");
    CHECK(model.rows[2].name == "Gamma");
    CHECK(model.rows[2].license == "Apache-2.0");
    CHECK(model.empty_text.empty());
    for (const auto& row : model.rows) {
        CHECK_FALSE(row.expanded);
        CHECK(row.license_text.empty());
    }
}

TEST_CASE("Expanding a row exposes only its license text", "[pulp-vellum-about]") {
    pulp::vellum::AboutNotices view(three());
    REQUIRE(view.model().rows.size() == 3);
    CHECK_FALSE(view.expanded_row().has_value());
    CHECK(view.shown_license_text().empty());

    view.set_expanded(1);
    REQUIRE(view.expanded_row().has_value());
    CHECK(*view.expanded_row() == 1);
    CHECK(view.shown_license_text() == "beta license body");
    CHECK(view.model().rows[1].expanded);
    CHECK(view.model().rows[1].license_text == "beta license body");
    CHECK(view.model().rows[0].license_text.empty());
    CHECK(view.model().rows[2].license_text.empty());

    view.set_expanded(std::nullopt);
    CHECK_FALSE(view.expanded_row().has_value());
    CHECK(view.shown_license_text().empty());
    CHECK_FALSE(view.model().rows[1].expanded);
    CHECK(view.model().rows[1].license_text.empty());
}

TEST_CASE("Expanding one row collapses the previous one", "[pulp-vellum-about]") {
    pulp::vellum::AboutNotices view(three());
    view.set_expanded(0);
    view.set_expanded(2);
    CHECK(*view.expanded_row() == 2);
    CHECK(view.shown_license_text() == "gamma license body");
    CHECK_FALSE(view.model().rows[0].expanded);
}

TEST_CASE("Toggle and an out-of-range row", "[pulp-vellum-about]") {
    pulp::vellum::AboutNotices view(three());
    view.toggle(1);
    CHECK(*view.expanded_row() == 1);
    view.toggle(1);
    CHECK_FALSE(view.expanded_row().has_value());

    view.set_expanded(0);
    view.set_expanded(7);
    CHECK_FALSE(view.expanded_row().has_value());
    CHECK(view.shown_license_text().empty());
}

TEST_CASE("Clicking a row toggles it without destroying the row", "[pulp-vellum-about]") {
    pulp::vellum::AboutNotices view(three());
    auto* row = view.row_view(1);
    REQUIRE(row != nullptr);
    REQUIRE(row->on_click);
    const auto before = row->child_count();

    row->on_click();
    CHECK(view.row_view(1) == row);
    CHECK(*view.expanded_row() == 1);
    CHECK(row->child_count() == before + 1);

    row->on_click();
    CHECK(view.row_view(1) == row);
    CHECK_FALSE(view.expanded_row().has_value());
    CHECK(row->child_count() == before);
}

TEST_CASE("No notices shows the recorded reason", "[pulp-vellum-about]") {
    pulp::format::ThirdPartyNotices none;
    none.product = "Example Synth";
    none.reason = "ThirdPartyNotices.json is not in this bundle";

    const auto model = pulp::vellum::make_about_model(none);
    CHECK(model.rows.empty());
    CHECK(model.empty_text == "ThirdPartyNotices.json is not in this bundle");

    pulp::vellum::AboutNotices view(none);
    CHECK(view.model().rows.empty());
    CHECK(view.model().empty_text == none.reason);
    CHECK(view.row_view(0) == nullptr);
    CHECK(view.shown_license_text().empty());
}

TEST_CASE("No notices and no reason shows a neutral line", "[pulp-vellum-about]") {
    const auto model = pulp::vellum::make_about_model(pulp::format::ThirdPartyNotices{});
    CHECK(model.rows.empty());
    CHECK_FALSE(model.empty_text.empty());
}
