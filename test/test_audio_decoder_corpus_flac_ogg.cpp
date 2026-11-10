// The decoder corpus replay's FLAC and Ogg cases: declared lengths the stream
// does not deliver and comment headers with impossible lengths. The
// cross-format replay is in test_audio_decoder_corpus.cpp.
// approach after SimZhou/vscode-audiolens c5c5a24; re-authored

#include "fuzz/audio_decoder_fuzz_oracle.hpp"
#include "support/audio_decoder_corpus.hpp"
#include "support/audio_decoder_corpus_expectations.hpp"
#include "support/fixture_root.hpp"
#include "support/unique_temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>
#include <pulp/audio/audio_file.hpp>
#include <pulp/audio/audio_reader.hpp>
#include <pulp/audio/format_registry.hpp>
#include <pulp/audio/wav_decoder.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using namespace pulp_test::audio_decoder_corpus;
using pulp_fuzz::audio_decoder::OracleFailure;

TEST_CASE("a declared length the stream does not deliver is refused, an undeclared one decoded",
          "[fuzz][audio]") {
    // Pinned by name so the contract survives changes to the generic oracle
    // case: the FLAC's STREAMINFO declares 800 frames the stream does not
    // hold, so read refuses while read_info still reports the declaration;
    // the two Ogg files declare no length (0), so read_info reports 0 and a
    // decode, if the decoder ends cleanly, is accepted under the limits.
    auto& registry = pulp::audio::FormatRegistry::instance();
    const auto limits = pulp_fuzz::audio_decoder::fuzz_limits();

    {
        const auto path = corpus_dir() / "flac-truncated-midframe.flac";
        REQUIRE(fs::exists(path));
        const auto info = registry.read_info(path.string());
        REQUIRE(info.has_value());
        CHECK(info->num_frames == 800u);
        CHECK_FALSE(registry.read(path.string(), limits).has_value());
    }

    // Both Ogg files declare no length (0), and stb_vorbis reports neither a
    // truncated page nor a bad page CRC as a decoder error: it treats both as
    // the end of the stream. So both decode what the stream held, and the
    // oracle checks rate and channels against the header.
    struct OggExpectation {
        const char* name;
        bool decodes;
    };
    for (const auto& expected : {OggExpectation{"ogg-truncated-page.ogg", true},
                                 OggExpectation{"ogg-page-bad-crc.ogg", true}}) {
        const auto path = corpus_dir() / expected.name;
        INFO(expected.name);
        REQUIRE(fs::exists(path));
        const auto info = registry.read_info(path.string());
        REQUIRE(info.has_value());
        CHECK(info->num_frames == 0u);
        const auto data = registry.read(path.string(), limits);
        CHECK(data.has_value() == expected.decodes);
        if (data) {
            CHECK(data->sample_rate == info->sample_rate);
            CHECK(data->num_channels() == info->num_channels);
        }
    }
}

TEST_CASE("a comment header with an impossible string length is refused without allocating",
          "[fuzz][audio][ogg]") {
    // libFuzzer reproducers: a negative vendor-string length, which once
    // became a negative-sized setup allocation, and a comment-string length
    // near INT_MAX, where len + 1 wrapped negative, slipped past the arena
    // check and the byte loop wrote past the arena.
    auto& registry = pulp::audio::FormatRegistry::instance();
    for (const char* name : {"ogg-vendor-length-negative.ogg", "ogg-comment-length-int-max.ogg"}) {
        const auto path = corpus_dir() / name;
        INFO(name);
        REQUIRE(fs::exists(path));
        const auto started = std::chrono::steady_clock::now();
        CHECK_FALSE(registry.read_info(path.string()).has_value());
        CHECK_FALSE(
            registry.read(path.string(), pulp_fuzz::audio_decoder::fuzz_limits()).has_value());
        CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(1));
    }
}
