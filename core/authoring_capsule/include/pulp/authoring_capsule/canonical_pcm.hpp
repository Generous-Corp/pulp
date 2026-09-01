#pragma once

/// @file canonical_pcm.hpp
/// The one portable audio representation a capsule carries.
///
/// A capsule that travelled with the author's original 96 kHz FLAC and nothing
/// else would only play back where that decoder exists. So the capsule embeds
/// a canonical rendition — a self-describing member of interleaved
/// little-endian float32 with a fixed header — and treats the author's
/// original as an optional source role. Machine B needs the canonical member
/// bytes and nothing more.
///
/// Canonical member byte layout (frozen — every audio member's identity is
/// the SHA-256 of exactly these bytes):
///
///   [0  .. 8)   magic, the ASCII bytes "pulp.pcm"
///   [8  .. 12)  uint32 LE decoder_version
///   [12 .. 16)  uint32 LE channels, 1 or 2
///   [16 .. 20)  uint32 LE sample_rate, in [8000, 192000]
///   [20 .. 28)  uint64 LE frame_count
///   [28 .. )    frame_count * channels interleaved little-endian IEEE-754
///               binary32 samples, finite only, exactly to the end
///
/// The header travels inside the hashed bytes, which buys three properties at
/// once: the member's plain SHA-256 is its identity, so an audio row obeys
/// the same uniform "row digest = digest of member bytes" rule as every other
/// row with no side channel; the member decodes from its bytes alone, so the
/// decoder-version guard is enforced by the bytes rather than by metadata a
/// consumer might drop; and two renditions differing only in declared rate
/// have different identities, because the rate is inside the hash.

#include <pulp/authoring_capsule/status.hpp>
#include <pulp/runtime/result.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace pulp::authoring_capsule {

inline constexpr std::uint32_t kMinCanonicalSampleRate = 8000;
inline constexpr std::uint32_t kMaxCanonicalSampleRate = 192000;
inline constexpr std::uint32_t kMaxCanonicalChannels = 2;

/// The decoder's contract version, stored in every canonical member's header.
/// A decode that is deterministic today must stay comparable tomorrow: a
/// change to the decode arithmetic is a change to every digest it produced,
/// so it must advance this number.
inline constexpr std::uint32_t kCanonicalPcmDecoderVersion = 1;

/// First eight bytes of every canonical member.
inline constexpr char kCanonicalPcmMagic[8] = {'p', 'u', 'l', 'p', '.', 'p', 'c', 'm'};

/// Fixed header size; sample data begins here.
inline constexpr std::size_t kCanonicalPcmHeaderBytes = 28;

struct CanonicalPcm {
    std::uint32_t channels = 0;
    std::uint32_t sample_rate = 0;
    std::uint64_t frame_count = 0;
    /// Interleaved, `frame_count * channels` samples.
    std::vector<float> samples;
};

/// Decode a source audio file into the canonical form. Admitted formats are
/// exactly the ones that decode deterministically — identical bytes in,
/// identical samples out, on every ISA:
///
///  - WAV: RIFF/WAVE PCM16/PCM24/PCM32 and float32, including
///    WAVE_FORMAT_EXTENSIBLE wrapping those;
///  - AIFF and AIFF-C: big-endian PCM at 9..32 declared bits (decoded at the
///    byte-container depth, since AIFF left-justifies), AIFF-C `NONE`/`twos`,
///    16-bit `sowt`, and `fl32` float32;
///  - FLAC: dr_flac's bit-exact integer decode, with this module's own
///    power-of-two integer-to-float scaling.
///
/// Anything else — MP3, OGG, AAC, CAF, an unknown container, a codec inside a
/// supported container — returns `decode_unsupported` naming what was found:
/// an approximation the user did not ask for is worse than a refusal they can
/// act on.
runtime::Result<CanonicalPcm, CapsuleError> decode_to_canonical(std::span<const std::uint8_t> source);

/// Serialize to the exact member bytes a capsule stores: the fixed header
/// documented at the top of this file, then the interleaved little-endian
/// binary32 samples. `canonical_pcm_digest()` is the SHA-256 of exactly this
/// buffer. The caller validates first; the header carries the fields as
/// declared.
std::vector<std::uint8_t> to_canonical_bytes(const CanonicalPcm& pcm);

/// Parse a canonical member from its bytes alone; no external metadata
/// exists. The header is self-checking: a wrong magic, a `decoder_version`
/// this build does not implement, a channel count outside [1, 2], or a
/// payload whose length disagrees with the declared geometry is refused.
/// There is no separate media struct: a successful parse returns the header's
/// fields in the `CanonicalPcm` itself, and the decoder version is not
/// surfaced because success proves it equals `kCanonicalPcmDecoderVersion`.
runtime::Result<CanonicalPcm, CapsuleError> from_canonical_bytes(std::span<const std::uint8_t> bytes);

/// SHA-256, bare lowercase hex, over `to_canonical_bytes(pcm)`. This is the
/// identity a `files[]` row carries for an audio member — and because it is
/// the digest of the member bytes themselves, it is the same digest the
/// exporter's uniform member hashing and the importer's extraction check
/// compute, with no audio-specific exception.
std::string canonical_pcm_digest(const CanonicalPcm& pcm);

/// Reject NaN, infinity, and any frame count that disagrees with the buffer
/// length. A capsule that admitted a NaN would hand machine B silence or a
/// blown output the author never heard.
runtime::Result<void, CapsuleError> validate_canonical(const CanonicalPcm& pcm);

}  // namespace pulp::authoring_capsule
