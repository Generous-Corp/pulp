#pragma once

// Shared by the decoder corpus replay's per-format files: where the corpus
// lives, how its files are listed and read, a canonical PCM WAV builder, and
// the oracle failure formatter.

#include "../fuzz/audio_decoder_fuzz_oracle.hpp"
#include "fixture_root.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace pulp_test::audio_decoder_corpus {

namespace fs = std::filesystem;
using pulp_fuzz::audio_decoder::OracleFailure;

inline fs::path corpus_dir() {
    return pulp_test::fixture_root() / "test" / "fixtures" / "fuzz" / "audio-decoder";
}

inline std::string lower_extension(const fs::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

// Every file in the corpus directory except its README, sorted so a failure
// report is stable from run to run.
inline std::vector<fs::path> corpus_files() {
    std::vector<fs::path> files;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(corpus_dir(), ec)) {
        if (!entry.is_regular_file())
            continue;
        const auto name = entry.path().filename().string();
        if (name == "README.md" || name.empty() || name.front() == '.')
            continue;
        files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    return files;
}

inline std::vector<uint8_t> read_bytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.good());
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(input), {});
}

// A canonical little-endian PCM WAV at 44.1 kHz: `codes` are interleaved
// integer samples at the given depth (8-bit written unsigned, as the format
// requires). `declared_data_size` lets the data chunk claim more than the
// file holds, for header-only fixtures; by default it is the real size.
inline std::vector<uint8_t>
make_pcm_wav(uint16_t channels, uint16_t bits, const std::vector<int32_t>& codes,
             std::optional<uint32_t> declared_data_size = std::nullopt) {
    const uint16_t bytes_per_sample = bits / 8;
    const uint32_t actual_size = static_cast<uint32_t>(codes.size()) * bytes_per_sample;
    const uint32_t data_size = declared_data_size.value_or(actual_size);
    std::vector<uint8_t> out;
    auto put32 = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i)
            out.push_back(static_cast<uint8_t>(v >> (8 * i)));
    };
    auto put16 = [&](uint16_t v) {
        out.push_back(static_cast<uint8_t>(v));
        out.push_back(static_cast<uint8_t>(v >> 8));
    };
    auto tag = [&](const char* t) { out.insert(out.end(), t, t + 4); };
    const uint32_t pad = actual_size & 1u; // RIFF chunks are word aligned
    tag("RIFF");
    put32(36u + data_size + pad);
    tag("WAVE");
    tag("fmt ");
    put32(16u);
    put16(1u); // PCM
    put16(channels);
    put32(44100u);
    put32(44100u * bytes_per_sample * channels);
    put16(static_cast<uint16_t>(bytes_per_sample * channels));
    put16(bits);
    tag("data");
    put32(data_size);
    for (int32_t code : codes) {
        const uint32_t raw =
            bits == 8 ? static_cast<uint32_t>(code + 128) : static_cast<uint32_t>(code);
        for (uint16_t b = 0; b < bytes_per_sample; ++b)
            out.push_back(static_cast<uint8_t>(raw >> (8 * b)));
    }
    if (pad)
        out.push_back(0u);
    return out;
}

inline std::string write_file(const fs::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary);
    REQUIRE(output.good());
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    return path.string();
}

inline std::string describe(const OracleFailure& failure) {
    return fs::path(failure.path).filename().string() + ": " + failure.field + ": " +
           failure.detail;
}

} // namespace pulp_test::audio_decoder_corpus
