// pulp_add_binary_data(... COMPRESS): the embedded zlib stream inflates through
// pulp::runtime::zlib_decompress to the source file byte for byte, and is
// smaller than the file.

#include <catch2/catch_test_macros.hpp>

#include <pulp/runtime/zip.hpp>

#include "pulp-test-embed-compress-data_data.hpp"

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

TEST_CASE("a COMPRESS embed inflates to its source file", "[runtime][embed][compression]") {
    std::ifstream in(PULP_TEST_EMBED_COMPRESS_SOURCE, std::ios::binary);
    REQUIRE(in);
    const std::vector<char> source{std::istreambuf_iterator<char>(in),
                                   std::istreambuf_iterator<char>()};
    REQUIRE(embed_compress_fixture::css_colors_js_size == source.size());
    CHECK(embed_compress_fixture::css_colors_js_zlib_size < source.size() / 2);

    const auto inflated = pulp::runtime::zlib_decompress(
        embed_compress_fixture::css_colors_js_zlib, embed_compress_fixture::css_colors_js_zlib_size,
        embed_compress_fixture::css_colors_js_size);
    REQUIRE(inflated);
    REQUIRE(inflated->size() == source.size());
    CHECK(std::equal(inflated->begin(), inflated->end(),
                     reinterpret_cast<const std::uint8_t*>(source.data())));
}
