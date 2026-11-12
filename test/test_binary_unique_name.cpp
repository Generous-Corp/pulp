// binary_unique_name: the token is this image's load address, so the name is
// stable within a process and carries the base name it was given.

#include <pulp/platform/binary_unique_name.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

using pulp::platform::binary_image_token;
using pulp::platform::binary_unique_name;

TEST_CASE("a binary-unique name is the base plus this image's token", "[platform][unique-name]") {
    const std::string token = binary_image_token();
    REQUIRE_FALSE(token.empty());
#if !defined(__EMSCRIPTEN__)
    REQUIRE(token != "0");  // the platform could say where this image is loaded
#endif
    REQUIRE(token == binary_image_token());
    REQUIRE(token.find_first_not_of("0123456789abcdef") == std::string::npos);
    REQUIRE(binary_unique_name("PulpWidget") == "PulpWidget_" + token);
    REQUIRE(binary_unique_name("") == "_" + token);
}
