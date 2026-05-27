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

ScreenshotCliOptions parse_args(std::initializer_list<const char*> args) {
    std::vector<std::string> storage;
    storage.reserve(args.size() + 1);
    storage.emplace_back("pulp-screenshot");
    for (const char* arg : args) storage.emplace_back(arg);

    std::vector<char*> argv;
    argv.reserve(storage.size());
    for (auto& arg : storage) argv.push_back(arg.data());

    return parse_options(static_cast<int>(argv.size()), argv.data());
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

TEST_CASE("pulp-screenshot runtime trace script records live native bounds",
          "[tools][screenshot][coverage]") {
    const std::string script = runtime_trace_script();
    REQUIRE(script.find("native_bounds_count") != std::string::npos);
    REQUIRE(script.find("native_bounds") != std::string::npos);
    REQUIRE(script.find("getLayoutRect") != std::string::npos);
    REQUIRE(script.find("bounds_source") != std::string::npos);
    REQUIRE(script.find("reference_frame") != std::string::npos);
    REQUIRE(script.find("root-view-css-points") != std::string::npos);
}

TEST_CASE("pulp-screenshot option parser preserves documented defaults",
          "[tools][screenshot][coverage]") {
    auto options = parse_args({});

    REQUIRE(options.script_path.empty());
    REQUIRE(options.output_path == "screenshot.png");
    REQUIRE(options.width == 400);
    REQUIRE(options.height == 300);
    REQUIRE(options.backend_was_defaulted);
    REQUIRE_FALSE(options.output_base64);
    REQUIRE_FALSE(options.demo);
    REQUIRE_FALSE(options.help);
#ifdef PULP_HAS_SKIA
    REQUIRE(options.backend_name == "skia");
#else
    REQUIRE(options.backend_name == "coregraphics");
#endif
    REQUIRE(normalize_backend(options));
    REQUIRE_FALSE(options.backend_name.empty());
}

TEST_CASE("pulp-screenshot option parser accepts explicit render settings",
          "[tools][screenshot][coverage]") {
    auto options = parse_args({
        "--script", "ui.js",
        "--output", "render.png",
        "--width", "1280",
        "--height", "720",
        "--scale", "1.5",
        "--theme", "pro_audio",
        "--backend", "coregraphics",
        "--runtime-trace", "trace.json",
        "--base64"
    });

    REQUIRE(options.script_path == "ui.js");
    REQUIRE(options.output_path == "render.png");
    REQUIRE(options.width == 1280);
    REQUIRE(options.height == 720);
    REQUIRE(options.scale == 1.5f);
    REQUIRE(options.theme_name == "pro_audio");
    REQUIRE(options.backend_name == "coregraphics");
    REQUIRE(options.runtime_trace_path == "trace.json");
    REQUIRE_FALSE(options.backend_was_defaulted);
    REQUIRE(options.output_base64);
    REQUIRE(normalize_backend(options));
    REQUIRE(options.backend_name == "coregraphics");
}

TEST_CASE("pulp-screenshot option parser handles aliases and last value wins",
          "[tools][screenshot][coverage]") {
    auto options = parse_args({
        "--demo",
        "--script", "first.js",
        "--script", "second.js",
        "--width", "100",
        "--width", "640",
        "--height", "50",
        "--height", "480",
        "--scale", "1",
        "--scale", "2.25",
        "--theme", "light",
        "--backend", "cg"
    });

    REQUIRE(options.demo);
    REQUIRE(options.script_path == "second.js");
    REQUIRE(options.width == 640);
    REQUIRE(options.height == 480);
    REQUIRE(options.scale == 2.25f);
    REQUIRE(options.theme_name == "light");
    REQUIRE(options.backend_name == "cg");
    REQUIRE_FALSE(options.backend_was_defaulted);
    REQUIRE_FALSE(options.output_base64);
    REQUIRE_FALSE(options.help);
    REQUIRE(normalize_backend(options));
    REQUIRE(options.backend_name == "cg");
}

TEST_CASE("pulp-screenshot help option short-circuits parsing entirely",
          "[tools][screenshot][coverage]") {
    // After #2956's pre-scan fix, `--help` anywhere in argv short-circuits
    // BEFORE the option loop runs — including BEFORE flags that come
    // earlier in argv. This is the documented help-path contract: any
    // command containing `--help` prints usage and exits cleanly, even
    // when the rest of the command line is garbage. Callers that need
    // their other flags applied must omit `--help`.
    ScreenshotCliOptions options;
    REQUIRE_NOTHROW(options = parse_args({
        "--script", "before-help.js",
        "--help",
        "--width", "not-a-number",
        "--height", "also-bad"
    }));

    REQUIRE(options.help);
    // Pre-help flags are intentionally NOT applied — help short-circuits
    // the whole option loop. Defaults remain.
    REQUIRE(options.script_path.empty());
    REQUIRE(options.width == 400);
    REQUIRE(options.height == 300);
    REQUIRE(options.backend_was_defaulted);
}

// Regression: #2956 / Codex comment 3304939247 — the previous fix
// (#2957) set `options.help = true` and continued the parse loop, so
// malformed numeric options ANYWHERE in argv (including BEFORE `--help`)
// still ran std::stoi / std::stof and threw, surfacing an error instead
// of clean help output. This pins help-path robustness in the order
// the user is most likely to hit it: a typo first, `--help` later.
TEST_CASE("pulp-screenshot --help short-circuits malformed args before it (#2956)",
          "[tools][screenshot][coverage][issue-2956]") {
    // Malformed --width / --height BEFORE --help must not blow up.
    ScreenshotCliOptions options;
    REQUIRE_NOTHROW(options = parse_args({
        "--width", "not-a-number",
        "--height", "also-bad",
        "--scale", "definitely-not-a-float",
        "--help",
    }));
    REQUIRE(options.help);
    // The values keep their defaults — the option loop never ran.
    REQUIRE(options.width == 400);
    REQUIRE(options.height == 300);
    REQUIRE(options.scale == 2.0f);

    // -h short flag also short-circuits.
    REQUIRE_NOTHROW(options = parse_args({"--width", "BAD", "-h"}));
    REQUIRE(options.help);

    // Help anywhere in the middle of malformed args still works.
    REQUIRE_NOTHROW(options = parse_args({
        "--width", "x",
        "--help",
        "--height", "y",
    }));
    REQUIRE(options.help);
}

TEST_CASE("pulp-screenshot backend normalization rejects unavailable explicit Skia",
          "[tools][screenshot][coverage]") {
    auto explicit_skia = parse_args({"--demo", "--backend", "skia"});
    REQUIRE(explicit_skia.backend_name == "skia");
    REQUIRE_FALSE(explicit_skia.backend_was_defaulted);
#ifdef PULP_HAS_SKIA
    REQUIRE(normalize_backend(explicit_skia));
    REQUIRE(explicit_skia.backend_name == "skia");
#else
    REQUIRE_FALSE(normalize_backend(explicit_skia));
    REQUIRE(explicit_skia.backend_name == "skia");
#endif

    auto default_backend = parse_args({"--demo"});
    REQUIRE(default_backend.backend_was_defaulted);
    REQUIRE(normalize_backend(default_backend));
#ifdef PULP_HAS_SKIA
    REQUIRE(default_backend.backend_name == "skia");
#else
    REQUIRE(default_backend.backend_name == "coregraphics");
#endif
}
