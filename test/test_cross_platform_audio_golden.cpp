#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/audio_file.hpp>
#include <pulp/signal/biquad.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

std::vector<float> render_reference_chain() {
    pulp::signal::Biquad filter;
    filter.set_coefficients(pulp::signal::BiquadCoefficients{
        .b0 = 0.3125f,
        .b1 = -0.1875f,
        .b2 = 0.0625f,
        .a1 = -0.4375f,
        .a2 = 0.125f,
    });

    std::vector<float> output(64);
    for (std::size_t i = 0; i < output.size(); ++i) {
        const int centered = static_cast<int>((i * 17U) % 31U) - 15;
        const float input = static_cast<float>(centered) * 0.0625f;
        const float filtered = filter.process(input);
        output[i] = filtered * (0.75f + 0.125f * filtered * filtered);
    }
    return output;
}

std::vector<unsigned char> read_bytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    REQUIRE(stream.good());
    return {std::istreambuf_iterator<char>(stream),
            std::istreambuf_iterator<char>()};
}

}  // namespace

TEST_CASE("Cross-platform audio golden is byte-exact",
          "[audio][golden][cross-platform][determinism]") {
    const auto fixture = std::filesystem::path(PULP_SOURCE_DIR) /
                         "test/fixtures/audio/cross_platform_signal_chain.wav";
    const auto generated = std::filesystem::temp_directory_path() /
                           "pulp-cross-platform-signal-chain.wav";

    pulp::audio::AudioFileData audio;
    audio.sample_rate = 48000;
    audio.channels = {render_reference_chain()};
    REQUIRE(pulp::audio::write_wav_file(
        generated.string(), audio, pulp::audio::WavBitDepth::Float32));

    if (const char* regenerate = std::getenv("PULP_REGENERATE_AUDIO_GOLDEN");
        regenerate && std::string(regenerate) == "1") {
        std::filesystem::create_directories(fixture.parent_path());
        std::filesystem::copy_file(
            generated, fixture, std::filesystem::copy_options::overwrite_existing);
    }

    const auto expected = read_bytes(fixture);
    const auto actual = read_bytes(generated);
    std::error_code ignored;
    std::filesystem::remove(generated, ignored);

    REQUIRE(actual == expected);
}
