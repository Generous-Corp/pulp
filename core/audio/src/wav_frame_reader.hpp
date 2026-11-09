#pragma once

// The one WAV decode path. Every reader (decode_wav, the format registry,
// AudioReader, the memory-mapped reader) probes a file with probe_wav, which
// walks it once, interprets its fmt chunk and applies the acceptance policy
// in wave_layout.hpp, and then reads frames through WavFrameReader, which
// feeds dr_wav a canonical header followed by the file's own data payload.
// Implemented in wav_decoder.cpp so the portable web lanes carry it too.

#include <pulp/audio/audio_file.hpp>
#include <pulp/audio/wave_layout.hpp>

#include <cstdint>
#include <memory>
#include <optional>

namespace pulp::audio::detail {

struct WavProbe {
    WaveLayout layout;
    std::optional<WavFormatFacts> facts; ///< set when the decoder can read the fmt chunk
    WaveDecision decision;

    [[nodiscard]] bool accepted() const noexcept {
        return decision.accepted();
    }
};

/// Walk, interpret and decide. Reads headers only.
[[nodiscard]] WavProbe probe_wav(const WaveByteCursor& cursor);

/// The AudioFileInfo every WAV reader reports for an accepted probe.
[[nodiscard]] AudioFileInfo wav_file_info(const WavProbe& probe);

/// dr_wav over [canonical header][the probe's recoverable data bytes].
class WavFrameReader {
  public:
    WavFrameReader();
    ~WavFrameReader();
    WavFrameReader(const WavFrameReader&) = delete;
    WavFrameReader& operator=(const WavFrameReader&) = delete;

    /// Opens an accepted probe. The cursor, and whatever it reads, must
    /// outlive the reader. False when the probe was refused or the decoder
    /// does not count the frames the probe decided on.
    [[nodiscard]] bool open(const WaveByteCursor& cursor, const WavProbe& probe);

    [[nodiscard]] std::uint32_t channels() const noexcept;
    [[nodiscard]] std::uint64_t frames() const noexcept;

    /// Decodes up to `count` frames from `frame` into `interleaved`
    /// (count x channels floats). Returns the frames decoded, short only on
    /// a decoder failure or at the end of the stream.
    std::uint64_t read_interleaved(std::uint64_t frame, std::uint64_t count, float* interleaved);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pulp::audio::detail
