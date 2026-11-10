// CHOC's WAV reader, as Pulp's configure-time patches leave it. Pulp's own
// readers decode WAV through dr_wav, but CHOC's WAV header installs with the
// SDK, so its patched reader is what a downstream caller of CHOC gets. These
// cases hold two of the patches (tools/cmake/PulpPatchChoc.cmake) to their
// behaviour on the decoder corpus seeds that motivated them.

#include "support/fixture_root.hpp"

#include <catch2/catch_test_macros.hpp>

#include <choc/audio/choc_AudioFileFormat_WAV.h>
#include <choc/audio/choc_SampleBuffers.h>
#include <pulp/audio/wav_decoder.hpp>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <vector>

namespace {

namespace fs = std::filesystem;

fs::path seed(const char* name) {
    return pulp_test::fixture_root() / "test" / "fixtures" / "fuzz" / "audio-decoder" / name;
}

std::unique_ptr<choc::audio::AudioFileReader> choc_reader(const fs::path& path) {
    choc::audio::WAVAudioFileFormat<false> format;
    try {
        return format.createReader(
            std::make_shared<std::ifstream>(path, std::ios::binary | std::ios::in));
    } catch (...) {
        return nullptr; // a format exception is CHOC's refusal
    }
}

} // namespace

TEST_CASE("CHOC's patched WAV reader keeps the extensible container width", "[audio][choc][wav]") {
    // 24 valid bits in a 32-bit container: unpatched CHOC reads it as packed
    // 24-bit at a 4-byte stride, off by a byte and 256 times too loud.
    const auto path = seed("wav-extensible-pcm24in32-stereo.wav");
    REQUIRE(fs::exists(path));
    auto reader = choc_reader(path);
    REQUIRE(reader);
    const auto props = reader->getProperties();
    CHECK(props.bitDepth == choc::audio::BitDepth::int32);
    REQUIRE(props.numChannels == 2u);

    std::ifstream in(path, std::ios::binary);
    const std::vector<std::uint8_t> bytes(std::istreambuf_iterator<char>(in), {});
    const auto expected = pulp::audio::decode_wav(bytes);
    REQUIRE(expected);
    REQUIRE(props.numFrames == expected->num_frames());

    choc::buffer::ChannelArrayBuffer<float> buffer(
        2u, static_cast<choc::buffer::FrameCount>(props.numFrames));
    REQUIRE(reader->readFrames(0, buffer.getView()));
    // CHOC scales by 2^31 - 1 and Pulp by 2^31: the two agree to well
    // inside 1e-6 at any amplitude, and a misread differs by orders more.
    double worst = 0.0;
    for (std::uint32_t ch = 0; ch < 2u; ++ch)
        for (std::uint64_t i = 0; i < props.numFrames; ++i)
            worst = std::max(worst, std::abs(static_cast<double>(buffer.getSample(ch, i)) -
                                             expected->channels[ch][i]));
    CHECK(worst < 1e-6);
}

TEST_CASE("CHOC's patched WAV reader refuses a block align smaller than its frame",
          "[audio][choc][wav]") {
    // Two 8-bit channels with nBlockAlign 1: unpatched CHOC steps one byte per
    // frame while copying two samples and reads past its raw block buffer.
    const auto path = seed("wav-block-align-smaller-than-frame.wav");
    REQUIRE(fs::exists(path));
    CHECK_FALSE(choc_reader(path));
    // The control: a well-formed seed opens through the same path.
    CHECK(choc_reader(seed("wav-pcm8-stereo.wav")));
}
