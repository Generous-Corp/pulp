#pragma once

// A WaveByteCursor over an AudioByteSource, for the WAV readers that pull
// from a file or a caller's stream rather than a span. Positional reads seek
// only when the request is not where the source already is, so the
// decoder's sequential reads stay sequential on the source.

#include "wav_frame_reader.hpp"

#include <pulp/audio/audio_reader.hpp>
#include <pulp/audio/wave_layout.hpp>

#include <memory>
#include <utility>

namespace pulp::audio::detail {

inline WaveByteCursor cursor_over(AudioByteSource& source) noexcept {
    WaveByteCursor cursor;
    cursor.context = &source;
    cursor.size = source.size();
    cursor.read_at = [](const WaveByteCursor& self, std::uint64_t offset, std::uint8_t* out,
                        std::size_t count) noexcept -> std::size_t {
        auto& byte_source = *static_cast<AudioByteSource*>(self.context);
        if (byte_source.position() != offset && !byte_source.seek(offset))
            return 0u;
        return byte_source.read(out, count);
    };
    return cursor;
}

// One WAV opened from a byte source: the source, the cursor over it and the
// probe, the sequence every source-backed WAV reader starts with. The cursor
// points into the source, which the unique_ptr keeps at a stable address, so
// the object may move.
struct WavSource {
    std::unique_ptr<AudioByteSource> source;
    WaveByteCursor cursor;
    WavProbe probe;

    // Takes the source and probes it; false when it is absent or refused.
    bool open(std::unique_ptr<AudioByteSource> byte_source) {
        source = std::move(byte_source);
        if (!source)
            return false;
        cursor = cursor_over(*source);
        probe = probe_wav(cursor);
        return probe.accepted();
    }
};

} // namespace pulp::audio::detail
