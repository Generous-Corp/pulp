// The JS preludes ship compressed. Each one must inflate to exactly the bytes
// of its source file, and a damaged stream must fail loudly rather than hand
// the engine truncated script.

#include <catch2/catch_test_macros.hpp>

#include <web_compat_preludes_gen.hpp>

#include <pulp/runtime/crypto.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace preludes = pulp::view::preludes;

namespace {

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

} // namespace

TEST_CASE("every embedded JS prelude inflates to its source file", "[view][preludes]") {
    const std::filesystem::path js_dir = std::filesystem::path(PULP_SOURCE_DIR) / "core/view/js";
    REQUIRE(preludes::kAllCount > 20);

    std::size_t source_total = 0;
    std::size_t deflated_total = 0;
    std::set<std::string> names;
    for (std::size_t i = 0; i < preludes::kAllCount; ++i) {
        const auto& prelude = *preludes::kAll[i];
        INFO(prelude.file_name);
        CHECK(names.insert(prelude.file_name).second);

        const std::string expected = read_file(js_dir / prelude.file_name);
        REQUIRE_FALSE(expected.empty());
        const std::string text = preludes::source(prelude);
        CHECK(text.size() == prelude.source_size);
        CHECK(text == expected);
        CHECK(pulp::runtime::sha256_hex(text) == prelude.sha256);

        source_total += prelude.source_size;
        deflated_total += prelude.deflated_size;
    }
    // The point of compressing: the preludes cost under a third of their text.
    CHECK(deflated_total * 3 < source_total);
}

TEST_CASE("a damaged embedded prelude throws instead of evaluating", "[view][preludes]") {
    const auto& original = preludes::css_colors;
    std::vector<unsigned char> bytes(original.deflated, original.deflated + original.deflated_size);

    SECTION("a flipped byte inside the stream") {
        bytes[bytes.size() / 2] ^= 0x40;
    }
    SECTION("a truncated stream") {
        bytes.resize(bytes.size() - 8);
    }

    preludes::EmbeddedPrelude damaged = original;
    damaged.deflated = bytes.data();
    damaged.deflated_size = bytes.size();
    CHECK_THROWS_AS(preludes::source(damaged), std::runtime_error);

    preludes::EmbeddedPrelude wrong_size = original;
    wrong_size.source_size = original.source_size + 1;
    CHECK_THROWS_AS(preludes::source(wrong_size), std::runtime_error);
}
