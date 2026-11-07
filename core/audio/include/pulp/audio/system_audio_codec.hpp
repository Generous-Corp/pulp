#pragma once

/// @file system_audio_codec.hpp
/// Write AAC, ALAC and PCM audio files through the operating system's codecs.
///
/// On Apple platforms this uses ExtAudioFile from AudioToolbox. On Windows it
/// uses Media Foundation's sink writer, which encodes AAC into an MPEG-4
/// file. Other platforms have no system codec and every write returns false.
/// No third-party encoder is linked, so these paths add no licensing terms.
///
/// FormatRegistry registers a writer built on this for the extensions no
/// other writer claims first (.m4a, .aac and .alac on Apple, .m4a on
/// Windows, plus .caf on Apple). An installed optional encoder such as FDK
/// AAC keeps priority for its extensions.
///
/// Threading: writing encodes, allocates and performs file I/O, so it belongs
/// on a control or background thread, never the audio callback.

#include <pulp/audio/audio_file.hpp>

#include <cstdint>
#include <string>

namespace pulp::audio {

/// File container to write.
enum class SystemAudioContainer {
    M4A,  ///< MPEG-4 audio (.m4a). Holds AAC or ALAC.
    ADTS, ///< Raw AAC stream with ADTS framing (.aac). Apple only.
    CAF,  ///< Core Audio Format (.caf). Holds AAC, ALAC or PCM. Apple only.
    AIFF, ///< AIFF (.aiff). Big-endian integer PCM only. Apple only.
};

/// Encoding inside the container.
enum class SystemAudioEncoding {
    AAC,       ///< AAC-LC, lossy.
    ALAC,      ///< Apple Lossless.
    LinearPCM, ///< Uncompressed PCM.
};

/// What to write. Unsupported combinations make the write return false.
struct SystemAudioWriteOptions {
    SystemAudioContainer container = SystemAudioContainer::M4A;
    SystemAudioEncoding encoding = SystemAudioEncoding::AAC;
    /// Source bit depth for ALAC (16, 20, 24 or 32) and integer PCM (16, 24
    /// or 32). Ignored for AAC.
    std::uint32_t bit_depth = 16;
    /// Write 32-bit float PCM instead of integer. CAF only.
    bool float_pcm = false;
    /// AAC target bitrate in bits per second; zero keeps the codec's default.
    /// The codec may round it to a rate it supports for the sample rate and
    /// channel count.
    std::uint32_t aac_bitrate = 0;
};

/// True when this platform's codecs can write `encoding` into `container`.
bool system_audio_can_write(SystemAudioContainer container, SystemAudioEncoding encoding);

/// Encodes `data` to `path`, replacing any existing file. Returns false when
/// the combination is unsupported, the data is empty or ragged, the sample
/// rate is zero, or the system codec refuses the format.
bool write_system_audio_file(const std::string& path, const AudioFileData& data,
                             const SystemAudioWriteOptions& options);

} // namespace pulp::audio
