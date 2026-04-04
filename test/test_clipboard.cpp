#include <catch2/catch_test_macros.hpp>
#include <pulp/platform/clipboard.hpp>

using namespace pulp::platform;

TEST_CASE("Clipboard text round-trip", "[platform][clipboard]") {
    bool ok = Clipboard::set_text("pulp test clipboard");
    REQUIRE(ok);

    REQUIRE(Clipboard::has_text());

    auto text = Clipboard::get_text();
    REQUIRE(text.has_value());
    REQUIRE(text.value() == "pulp test clipboard");
}

TEST_CASE("Clipboard binary data round-trip", "[platform][clipboard]") {
    std::vector<uint8_t> data = {0x50, 0x55, 0x4C, 0x50}; // "PULP"
    bool ok = Clipboard::set_data("com.pulp.test", data);
    REQUIRE(ok);

    auto result = Clipboard::get_data("com.pulp.test");
    REQUIRE(result.has_value());
    REQUIRE(result->size() == 4);
    REQUIRE((*result)[0] == 0x50);
    REQUIRE((*result)[3] == 0x50);
}

TEST_CASE("Clipboard missing data returns nullopt", "[platform][clipboard]") {
    auto result = Clipboard::get_data("com.pulp.nonexistent.type");
    REQUIRE_FALSE(result.has_value());
}

#ifdef __APPLE__
TEST_CASE("Clipboard text reflects pasteboard contents, not stale fallback", "[platform][clipboard]") {
    REQUIRE(Clipboard::set_text("pulp stale text"));
    REQUIRE(Clipboard::set_data("com.pulp.test.binary", {0x01, 0x02, 0x03}));

    REQUIRE_FALSE(Clipboard::has_text());
    REQUIRE_FALSE(Clipboard::get_text().has_value());
}

TEST_CASE("Clipboard data reflects pasteboard contents, not stale fallback", "[platform][clipboard]") {
    REQUIRE(Clipboard::set_data("com.pulp.test.binary", {0xAA, 0xBB}));
    REQUIRE(Clipboard::set_text("plain text only"));

    REQUIRE_FALSE(Clipboard::get_data("com.pulp.test.binary").has_value());
}
#endif
