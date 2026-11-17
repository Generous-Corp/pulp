// Checks the platform-independent helpers behind the Linux backends: the
// AT-SPI UTF-8 offset arithmetic, the XDND reply validation and the
// /proc/meminfo classification. They have no platform dependency, so they run
// everywhere.

#include <catch2/catch_test_macros.hpp>

#include "../core/platform/platform/linux/meminfo_pressure.hpp"
#include "../core/view/platform/linux/atspi_text_offsets.hpp"
#include <pulp/view/platform/atspi_geometry.hpp>

#include <string>

namespace {

namespace text = pulp::view::atspi_text;
namespace atspi = pulp::view::atspi;
namespace env = pulp::platform::linux_env;
using pulp::platform::MemoryPressure;

// "aé😀b": a 1-byte, a 2-byte, a 4-byte and a 1-byte character (8 bytes).
const std::string kMixed = "a\xC3\xA9\xF0\x9F\x98\x80"
                           "b";

TEST_CASE("text counts characters not bytes", "[view][linux][pure]") {
    CHECK(kMixed.size() == 8);
    CHECK(text::character_count(kMixed) == 4);
    CHECK(text::character_count("") == 0);
    CHECK(text::character_count("\xC3\xA9") == 1);
}

TEST_CASE("text slices on character boundaries", "[view][linux][pure]") {
    CHECK(text::substring("\xC3\xA9", 0, 1) == "\xC3\xA9");
    CHECK(text::substring(kMixed, 1, 3) == "\xC3\xA9\xF0\x9F\x98\x80");
    CHECK(text::substring(kMixed, 0, -1) == kMixed);
    CHECK(text::substring(kMixed, 3, 2).empty());
    CHECK(text::substring(kMixed, -4, 99) == kMixed);
}

TEST_CASE("text reads code points", "[view][linux][pure]") {
    CHECK(text::code_point_at(kMixed, 0) == 'a');
    CHECK(text::code_point_at(kMixed, 1) == 0xE9u);
    CHECK(text::code_point_at(kMixed, 2) == 0x1F600u);
    CHECK(text::code_point_at(kMixed, 3) == 'b');
    CHECK(text::code_point_at(kMixed, 4) == 0u);
    CHECK(text::code_point_at(kMixed, -1) == 0u);
}

TEST_CASE("text converts between byte and character offsets", "[view][linux][pure]") {
    CHECK(text::byte_offset_of_character(kMixed, 0) == 0);
    CHECK(text::byte_offset_of_character(kMixed, 2) == 3);
    CHECK(text::byte_offset_of_character(kMixed, 3) == 7);
    CHECK(text::byte_offset_of_character(kMixed, 99) == 8);
    CHECK(text::character_index_of_byte(kMixed, 0) == 0);
    CHECK(text::character_index_of_byte(kMixed, 3) == 2);
    CHECK(text::character_index_of_byte(kMixed, 7) == 3);
    CHECK(text::character_index_of_byte(kMixed, 8) == 4);
    CHECK(text::character_index_of_byte(kMixed, 99) == 4);
}

TEST_CASE("text treats malformed bytes as single characters", "[view][linux][pure]") {
    const std::string stray = "a\x80z";  // lone continuation byte
    const std::string cut = "a\xE2\x82"; // truncated 3-byte sequence
    CHECK(text::character_count(stray) == 3);
    CHECK(text::character_count(cut) == 3);
    CHECK(text::code_point_at(stray, 1) == 0xFFFDu);
}

TEST_CASE("meminfo zero available is critical", "[view][linux][pure]") {
    const env::MeminfoField total{true, 16'000'000};
    CHECK(env::classify_memory_pressure(total, {true, 0}) == MemoryPressure::critical);
    CHECK(env::classify_memory_pressure(total, {true, 500'000}) == MemoryPressure::critical);
    CHECK(env::classify_memory_pressure(total, {true, 2'000'000}) == MemoryPressure::moderate);
    CHECK(env::classify_memory_pressure(total, {true, 8'000'000}) == MemoryPressure::normal);
}

TEST_CASE("meminfo missing fields are no reading", "[view][linux][pure]") {
    const env::MeminfoField total{true, 16'000'000};
    CHECK(env::classify_memory_pressure(total, {}) == MemoryPressure::normal);
    CHECK(env::classify_memory_pressure({}, {true, 0}) == MemoryPressure::normal);
    CHECK(env::classify_memory_pressure({true, 0}, {true, 0}) == MemoryPressure::normal);
}

TEST_CASE("meminfo lines parse with presence", "[view][linux][pure]") {
    const auto zero = env::parse_meminfo_line("MemAvailable:             0 kB", "MemAvailable:");
    CHECK((zero.present && zero.kb == 0));
    const auto value = env::parse_meminfo_line("MemTotal:       16384 kB", "MemTotal:");
    CHECK((value.present && value.kb == 16384));
    CHECK(!env::parse_meminfo_line("MemFree: 5 kB", "MemTotal:").present);
    CHECK(!env::parse_meminfo_line("MemAvailable: junk kB", "MemAvailable:").present);
}

} // namespace

// ── UTF-8 well-formedness ────────────────────────────────────────────────────
// Each ill-formed sequence below must count as one character per byte (so no
// byte is lost or invented) and decode to U+FFFD; each well-formed boundary
// value must count as one character and decode to its code point.

TEST_CASE("text rejects overlong surrogate and out-of-range sequences", "[view][linux][pure]") {
    const std::string bad[] = {
        "\xE0\x80\x80",     // overlong U+0000
        "\xE0\x9F\xBF",     // overlong U+07FF
        "\xC0\x80",         // overlong 2-byte U+0000
        "\xC1\xBF",         // overlong 2-byte U+007F
        "\xED\xA0\x80",     // surrogate U+D800
        "\xED\xBF\xBF",     // surrogate U+DFFF
        "\xF0\x80\x80\x80", // overlong 4-byte U+0000
        "\xF0\x8F\xBF\xBF", // overlong 4-byte U+FFFF
        "\xF4\x90\x80\x80", // above U+10FFFF
        "\xF5\x80\x80\x80", // lead byte beyond F4
    };
    for (const std::string& sequence : bad) {
        INFO("sequence of " << sequence.size() << " bytes starting " << std::hex
                            << static_cast<unsigned>(static_cast<unsigned char>(sequence[0])));
        CHECK(text::character_count(sequence) == static_cast<int>(sequence.size()));
        CHECK(text::code_point_at(sequence, 0) == 0xFFFDu);
    }
}

TEST_CASE("text accepts the boundary values of well-formed UTF-8", "[view][linux][pure]") {
    struct Case {
        std::string bytes;
        uint32_t code_point;
    };
    const Case good[] = {
        {"\xC2\x80", 0x80u},             // first 2-byte value
        {"\xDF\xBF", 0x7FFu},            // last 2-byte value
        {"\xE0\xA0\x80", 0x800u},        // first 3-byte value
        {"\xED\x9F\xBF", 0xD7FFu},       // last before the surrogates
        {"\xEE\x80\x80", 0xE000u},       // first after the surrogates
        {"\xEF\xBF\xBF", 0xFFFFu},       // last 3-byte value
        {"\xF0\x90\x80\x80", 0x10000u},  // first 4-byte value
        {"\xF4\x8F\xBF\xBF", 0x10FFFFu}, // last scalar value
    };
    for (const Case& c : good) {
        INFO("code point " << std::hex << c.code_point);
        CHECK(text::character_count(c.bytes) == 1);
        CHECK(text::code_point_at(c.bytes, 0) == c.code_point);
    }
}

TEST_CASE("text does not let a bad sequence swallow the character after it",
          "[view][linux][pure]") {
    // The surrogate is three single bytes, then a well-formed 'e acute' follows.
    const std::string s = "\xED\xA0\x80\xC3\xA9";
    CHECK(text::character_count(s) == 4);
    CHECK(text::code_point_at(s, 3) == 0xE9u);
}

// ── AT-SPI component geometry ────────────────────────────────────────────────

TEST_CASE("component geometry follows the viewport transform and the screen origin",
          "[view][linux][pure][accessibility]") {
    atspi::WindowGeometry identity;
    // A root that fills the window: window coordinates are the root's.
    CHECK(atspi::root_rect_in(35, 75, 200, 24, atspi::kCoordWindow, identity) ==
          atspi::PixelRect{35, 75, 200, 24});

    // A 400x300 design shown at 2x, centred in an 1000x700 window: the root is
    // 800x600, so the offset is (100, 50).
    atspi::WindowGeometry letterboxed;
    letterboxed.scale = 2.0f;
    letterboxed.offset_x = 100.0f;
    letterboxed.offset_y = 50.0f;
    CHECK(atspi::root_rect_in(35, 75, 200, 24, atspi::kCoordWindow, letterboxed) ==
          atspi::PixelRect{170, 200, 400, 48});

    // The same window at (300, 400) on the desktop.
    letterboxed.screen_origin_known = true;
    letterboxed.screen_x = 300;
    letterboxed.screen_y = 400;
    CHECK(atspi::root_rect_in(35, 75, 200, 24, atspi::kCoordScreen, letterboxed) ==
          atspi::PixelRect{470, 600, 400, 48});
    // Control: ignoring the transform (the defect) would give 335,475.
    CHECK(atspi::root_rect_in(35, 75, 200, 24, atspi::kCoordScreen, letterboxed).x != 335);
}

TEST_CASE("component coordinate spaces need what they rely on",
          "[view][linux][pure][accessibility]") {
    atspi::WindowGeometry g;
    CHECK(atspi::coord_type_available(atspi::kCoordWindow, g));
    CHECK_FALSE(atspi::coord_type_available(atspi::kCoordScreen, g)); // origin not reported yet
    CHECK_FALSE(atspi::coord_type_available(2, g));                   // parent coordinates
    g.screen_origin_known = true;
    CHECK(atspi::coord_type_available(atspi::kCoordScreen, g));
    CHECK_FALSE(atspi::coord_type_available(2, g));
}

TEST_CASE("a non-positive scale is treated as 1", "[view][linux][pure][accessibility]") {
    atspi::WindowGeometry g;
    g.scale = 0.0f;
    CHECK(atspi::root_rect_in(10, 10, 20, 20, atspi::kCoordWindow, g) ==
          atspi::PixelRect{10, 10, 20, 20});
}
