#pragma once

/// @file canonical_pcm.hpp
/// The one portable audio representation a capsule carries.
///
/// A capsule that travelled with the author's original AIFF or 96 kHz FLAC and
/// nothing else would only play back where that decoder exists. So the capsule
/// embeds a canonical rendition — interleaved little-endian float32, one or two
/// channels, an integer source rate in [8000, 192000], finite samples, exact
/// frame count — and treats the author's original as an optional source role.
/// Machine B needs the canonical bytes and nothing more.

#include <pulp/authoring_capsule/status.hpp>
#include <pulp/runtime/result.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace pulp::authoring_capsule {

inline constexpr std::uint32_t kMinCanonicalSampleRate = 8000;
inline constexpr std::uint32_t kMaxCanonicalSampleRate = 192000;
inline constexpr std::uint32_t kMaxCanonicalChannels = 2;

/// The decoder's contract version. It appears in receipts because a decode
/// that is deterministic today must stay comparable tomorrow: a change here is
/// a change to every digest it produced.
inline constexpr std::uint32_t kCanonicalPcmDecoderVersion = 1;

struct CanonicalPcm {
    std::uint32_t channels = 0;
    std::uint32_t sample_rate = 0;
    std::uint64_t frame_count = 0;
    /// Interleaved, `frame_count * channels` samples.
    std::vector<float> samples;
};

/// Media metadata that is hashed alongside the sample bytes, so two renditions
/// that differ only in declared rate cannot share an identity.
struct CanonicalPcmMedia {
    std::uint32_t channels = 0;
    std::uint32_t sample_rate = 0;
    std::uint64_t frame_count = 0;
    std::uint32_t decoder_version = kCanonicalPcmDecoderVersion;
};

/// Decode WAV PCM16, PCM24, PCM32, or float32 into the canonical form. Any
/// other codec or channel layout returns `decode_unsupported` naming what was
/// found: an approximation the user did not ask for is worse than a refusal
/// they can act on.
runtime::Result<CanonicalPcm, CapsuleError> decode_to_canonical(std::span<const std::uint8_t> wav);

/// Serialize to the exact bytes a capsule stores: little-endian float32,
/// interleaved, no header. The header lives in the manifest, where it is
/// covered by the closure.
std::vector<std::uint8_t> to_canonical_bytes(const CanonicalPcm& pcm);

runtime::Result<CanonicalPcm, CapsuleError>
from_canonical_bytes(std::span<const std::uint8_t> bytes, const CanonicalPcmMedia& media);

/// Digest over the canonical sample bytes plus the media metadata. This is the
/// identity a `files[]` row carries for an audio member.
std::string canonical_pcm_digest(const CanonicalPcm& pcm);

/// Reject NaN, infinity, and any frame count that disagrees with the buffer
/// length. A capsule that admitted a NaN would hand machine B silence or a
/// blown output the author never heard.
runtime::Result<void, CapsuleError> validate_canonical(const CanonicalPcm& pcm);

}  // namespace pulp::authoring_capsule
