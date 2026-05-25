#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#define main pulp_screenshot_main_for_test
#include "../tools/screenshot/pulp_screenshot.cpp"
#undef main

namespace {

std::filesystem::path temp_file_path(const char* name) {
    return std::filesystem::temp_directory_path() / ("pulp-screenshot-cli-" + std::string(name));
}

void write_bytes(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary);
    REQUIRE(out.good());
    out << bytes;
}

}  // namespace

TEST_CASE("pulp-screenshot base64 encoder handles RFC 4648 padding cases",
          "[tools][screenshot][coverage]") {
    REQUIRE(base64_encode({}).empty());
    REQUIRE(base64_encode({'f'}) == "Zg==");
    REQUIRE(base64_encode({'f', 'o'}) == "Zm8=");
    REQUIRE(base64_encode({'f', 'o', 'o'}) == "Zm9v");
    REQUIRE(base64_encode({'f', 'o', 'o', 'b'}) == "Zm9vYg==");
    REQUIRE(base64_encode({'f', 'o', 'o', 'b', 'a'}) == "Zm9vYmE=");
    REQUIRE(base64_encode({'f', 'o', 'o', 'b', 'a', 'r'}) == "Zm9vYmFy");

    auto one_byte = base64_encode({0xff});
    REQUIRE(one_byte == "/w==");
    REQUIRE(one_byte.size() == 4);
    REQUIRE(one_byte[2] == '=');
    REQUIRE(one_byte[3] == '=');

    auto two_bytes = base64_encode({0xff, 0xee});
    REQUIRE(two_bytes == "/+4=");
    REQUIRE(two_bytes.size() == 4);
    REQUIRE(two_bytes[3] == '=');
}

TEST_CASE("pulp-screenshot base64 encoder preserves binary PNG-like bytes",
          "[tools][screenshot][coverage]") {
    std::vector<uint8_t> png_header = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    auto encoded = base64_encode(png_header);
    REQUIRE(encoded == "iVBORw0KGgo=");
    REQUIRE(encoded.size() == 12);
    REQUIRE(encoded.find('\n') == std::string::npos);
    REQUIRE(encoded.find('\r') == std::string::npos);
    REQUIRE(encoded.back() == '=');

    std::vector<uint8_t> all_low_bytes = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05};
    REQUIRE(base64_encode(all_low_bytes) == "AAECAwQF");

    std::vector<uint8_t> mixed = {'P', 'u', 'l', 'p', 0x00, 0xff, 0x10};
    auto mixed_encoded = base64_encode(mixed);
    REQUIRE(mixed_encoded == "UHVscAD/EA==");
    REQUIRE(mixed_encoded.size() % 4 == 0);
}

TEST_CASE("pulp-screenshot base64 encoder emits stable 4-character blocks",
          "[tools][screenshot][coverage]") {
    for (std::size_t size = 1; size <= 12; ++size) {
        std::vector<uint8_t> bytes(size);
        for (std::size_t i = 0; i < size; ++i)
            bytes[i] = static_cast<uint8_t>(i * 17 + 3);

        auto encoded = base64_encode(bytes);
        REQUIRE(encoded.size() == ((size + 2) / 3) * 4);
        REQUIRE(encoded.size() % 4 == 0);

        if (size % 3 == 0) {
            REQUIRE(encoded.find('=') == std::string::npos);
        } else if (size % 3 == 1) {
            REQUIRE(encoded.substr(encoded.size() - 2) == "==");
        } else {
            REQUIRE(encoded.back() == '=');
        }
    }
}

TEST_CASE("pulp-screenshot read_file preserves script bytes and fails closed",
          "[tools][screenshot][coverage]") {
    auto path = temp_file_path("script.js");
    std::filesystem::remove(path);
    write_bytes(path, "createLabel('title', 'Hello');\n");

    auto body = read_file(path.string());
    REQUIRE(body == "createLabel('title', 'Hello');\n");
    REQUIRE(body.size() == 31);
    REQUIRE(body.back() == '\n');

    write_bytes(path, std::string("nul\0inside", 10));
    auto binary_body = read_file(path.string());
    REQUIRE(binary_body.size() == 10);
    REQUIRE(binary_body[0] == 'n');
    REQUIRE(binary_body[3] == '\0');
    REQUIRE(binary_body.substr(4) == "inside");

    std::filesystem::remove(path);
    REQUIRE(read_file(path.string()).empty());
    REQUIRE(read_file((path.parent_path() / "missing.js").string()).empty());
}
