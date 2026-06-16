// test_mmap_reader_ranged.cpp — MemoryMappedAudioReader does true ranged reads
// (seek-based, no whole-file decode) for WAV, with EOF zero-fill.

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/audio_file.hpp>
#include <pulp/audio/mmap_reader.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

using pulp::audio::AudioFileData;
using pulp::audio::MemoryMappedAudioReader;
using pulp::audio::write_wav_file;

namespace {

float expected_sample(std::uint64_t frame, std::uint32_t ch) {
    return (static_cast<float>(frame % 991) / 991.0f) * 0.5f - 0.25f +
           static_cast<float>(ch) * 0.1f;
}

std::string temp_wav(const char* name) {
    const char* tmp = std::getenv("TMPDIR");
    return std::string(tmp ? tmp : "/tmp") + "/" + name;
}

bool approx(float a, float b) { return std::fabs(a - b) < 3e-4f; }  // 16-bit PCM tolerance

}  // namespace

TEST_CASE("MemoryMappedAudioReader ranged read of a WAV", "[audio][mmap][ranged]") {
    const std::uint32_t channels = 2;
    const std::uint64_t total = 16000;
    const std::string path = temp_wav("pulp_mmap_ranged_test.wav");

    AudioFileData data;
    data.sample_rate = 48000;
    data.channels.resize(channels);
    for (std::uint32_t ch = 0; ch < channels; ++ch) {
        data.channels[ch].resize(total);
        for (std::uint64_t f = 0; f < total; ++f)
            data.channels[ch][f] = expected_sample(f, ch);
    }
    REQUIRE(write_wav_file(path, data));

    MemoryMappedAudioReader r;
    REQUIRE(r.open(path));
    REQUIRE(r.info().num_frames == total);
    REQUIRE(r.info().num_channels == channels);
    REQUIRE(r.supports_ranged_read());  // WAV → true ranged (no whole-file decode)

    auto read_at = [&](std::uint64_t start, std::uint64_t n) {
        std::vector<float> l(n, 999.0f), rch(n, 999.0f);
        float* dst[2] = {l.data(), rch.data()};
        REQUIRE(r.read_frames(dst, 2, start, n));
        return std::pair{l, rch};
    };

    SECTION("read from the start") {
        auto [l, rr] = read_at(0, 512);
        for (std::uint64_t i = 0; i < 512; ++i) {
            REQUIRE(approx(l[i], expected_sample(i, 0)));
            REQUIRE(approx(rr[i], expected_sample(i, 1)));
        }
    }

    SECTION("seek into the middle") {
        const std::uint64_t start = 9001;  // arbitrary, non-block-aligned
        auto [l, rr] = read_at(start, 700);
        for (std::uint64_t i = 0; i < 700; ++i) {
            REQUIRE(approx(l[i], expected_sample(start + i, 0)));
            REQUIRE(approx(rr[i], expected_sample(start + i, 1)));
        }
    }

    SECTION("read across EOF zero-fills the tail") {
        const std::uint64_t start = total - 100;
        auto [l, rr] = read_at(start, 400);  // 100 real + 300 past EOF
        for (std::uint64_t i = 0; i < 100; ++i)
            REQUIRE(approx(l[i], expected_sample(start + i, 0)));
        for (std::uint64_t i = 100; i < 400; ++i) {
            REQUIRE(l[i] == 0.0f);
            REQUIRE(rr[i] == 0.0f);
        }
    }

    SECTION("chunked full read reconstructs the file in order") {
        const std::uint64_t block = 333;
        std::uint64_t pos = 0;
        while (pos < total) {
            const std::uint64_t n = std::min<std::uint64_t>(block, total - pos);
            auto [l, rr] = read_at(pos, n);
            for (std::uint64_t i = 0; i < n; ++i) {
                REQUIRE(approx(l[i], expected_sample(pos + i, 0)));
                REQUIRE(approx(rr[i], expected_sample(pos + i, 1)));
            }
            pos += n;
        }
    }

    r.close();
    std::remove(path.c_str());
}
