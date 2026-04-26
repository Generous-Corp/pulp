#include <catch2/catch_test_macros.hpp>
#include <pulp/audio/format_registry.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace pulp::audio {
std::unique_ptr<FormatReader> create_ogg_reader();
}

namespace {

namespace fs = std::filesystem;

uint64_t current_process_id() {
#ifdef _WIN32
    return static_cast<uint64_t>(_getpid());
#else
    return static_cast<uint64_t>(getpid());
#endif
}

struct TempDir {
    fs::path path;

    TempDir() {
        static std::atomic<uint64_t> next_id{0};
        auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        auto unique_id = next_id.fetch_add(1, std::memory_order_relaxed);
        path = fs::temp_directory_path()
             / ("pulp-ogg-reader-" + std::to_string(current_process_id()) + "-"
                + std::to_string(stamp) + "-" + std::to_string(unique_id));
        fs::create_directories(path);
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void write_binary(const fs::path& path, std::string_view bytes) {
    std::ofstream output(path, std::ios::binary);
    REQUIRE(output.good());
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(output.good());
}

} // namespace

TEST_CASE("OggReader reports supported extensions and format name",
          "[audio][ogg][issue-640]") {
    auto reader = pulp::audio::create_ogg_reader();
    REQUIRE(reader != nullptr);

    REQUIRE(reader->format_name() == "OGG Vorbis");
    REQUIRE(reader->supports_extension(".ogg"));
    REQUIRE(reader->supports_extension(".oga"));
    REQUIRE_FALSE(reader->supports_extension(".wav"));
    REQUIRE_FALSE(reader->supports_extension(".OGG"));
}

TEST_CASE("OggReader rejects missing and malformed files",
          "[audio][ogg][issue-640]") {
    auto reader = pulp::audio::create_ogg_reader();
    TempDir temp;

    auto missing = (temp.path / "missing.ogg").string();
    REQUIRE_FALSE(reader->read_info(missing).has_value());
    REQUIRE_FALSE(reader->read(missing).has_value());

    auto malformed = temp.path / "malformed.oga";
    write_binary(malformed, "OggS_not_a_valid_vorbis_stream");

    REQUIRE_FALSE(reader->read_info(malformed.string()).has_value());
    REQUIRE_FALSE(reader->read(malformed.string()).has_value());
}
