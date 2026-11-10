#pragma once

// approach after SimZhou/vscode-audiolens c5c5a24; re-authored

/// @file audio_file_layout.hpp
/// Byte-level layout report for an audio file: every region the container
/// declares, with its offset and size, the fields the specification defines
/// for it, and the specification violations found while walking it.
///
/// The report is a fact sheet, not a verdict. It never decodes samples and
/// reads only what the container structure requires: the envelope, each
/// region header, and the small payloads whose fields it parses. A multi-
/// gigabyte file costs the same handful of reads as a 44-byte one.
///
/// For WAV the verdict, the stream facts (rate, channels, bits, the frames
/// present and the frames declared) and every warning that bears on decoding
/// come from the decoder's own probe: the chunk walk and acceptance policy in
/// wave_layout.hpp, which `inspect_wav` and every Pulp WAV reader apply. The
/// report cannot disagree with the decoder; the parity test holds it to
/// that.

#include <pulp/audio/wav_decoder.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace pulp::audio {

/// Random-access byte source the layout inspector reads through.
///
/// The inspector asks for region headers and bounded payloads at explicit
/// offsets, never for sample data. Implementations serve a byte span or a
/// file; a test can wrap one to count what was read.
class LayoutByteSource {
  public:
    virtual ~LayoutByteSource() = default;

    /// Total size of the underlying bytes.
    [[nodiscard]] virtual std::uint64_t size() const noexcept = 0;

    /// Copy up to `out.size()` bytes starting at `offset` into `out`.
    /// Returns the count copied, which is short only at end of input.
    [[nodiscard]] virtual std::size_t read_at(std::uint64_t offset,
                                              std::span<std::uint8_t> out) noexcept = 0;
};

/// Byte source over an in-memory span. The span must outlive the source.
class SpanByteSource final : public LayoutByteSource {
  public:
    explicit SpanByteSource(std::span<const std::uint8_t> bytes) noexcept : bytes_(bytes) {}

    [[nodiscard]] std::uint64_t size() const noexcept override {
        return bytes_.size();
    }
    [[nodiscard]] std::size_t read_at(std::uint64_t offset,
                                      std::span<std::uint8_t> out) noexcept override;

  private:
    std::span<const std::uint8_t> bytes_;
};

/// Byte source over a file opened for positional reads. `size()` is 0 and
/// `is_open()` false when the file could not be opened.
class FileByteSource final : public LayoutByteSource {
  public:
    explicit FileByteSource(const std::filesystem::path& path);
    ~FileByteSource() override;

    FileByteSource(const FileByteSource&) = delete;
    FileByteSource& operator=(const FileByteSource&) = delete;

    [[nodiscard]] bool is_open() const noexcept {
        return handle_ >= 0;
    }
    [[nodiscard]] std::uint64_t size() const noexcept override {
        return size_;
    }
    [[nodiscard]] std::size_t read_at(std::uint64_t offset,
                                      std::span<std::uint8_t> out) noexcept override;

  private:
    int handle_ = -1;
    std::uint64_t size_ = 0u;
};

/// Container the inspector recognized from the leading bytes.
enum class AudioContainer {
    unknown,
    wav,  ///< RIFF, RF64/BW64, or Sony Wave64 envelope around WAVE data
    aiff, ///< FORM/AIFF
    aifc, ///< FORM/AIFC
    flac, ///< native FLAC stream
    ogg,  ///< Ogg pages (Vorbis, Opus, FLAC-in-Ogg, Speex)
    mp3,  ///< MPEG audio frames, optionally wrapped in ID3/APE tags
    mp4,  ///< ISO base media file (MP4, M4A, 3GP, MOV)
    raw,  ///< headerless PCM described by the caller
};

/// What a region holds.
enum class LayoutRegionKind { envelope, fmt, data, metadata, padding, unknown };

/// WAV header family. Standard files carry exactly one of these and no warning.
enum class WavProfile { pcm44, bwf, floating, extensible, rf64, w64 };

/// How far the facts can be trusted: `parsed` means the inspector walked the
/// container's own structure; `container_only` means it identified the
/// container and read its first page or frame, and the stream facts are the
/// codec's claim about itself, not a measurement of the whole file.
enum class FactsConfidence { parsed, container_only };

// LayoutWarningKind, the specification violations a walk can prove from
// bytes alone, is declared in wave_layout.hpp with the WAV walk that raises
// most of them.

struct LayoutWarning {
    LayoutWarningKind kind;
    std::uint64_t offset = 0u; ///< region the warning is about
    std::string detail;        ///< human-readable specifics with the numbers involved
};

/// A parsed field value. Strings here are never free text from the file;
/// those live under `LayoutMetadataText` and are off by default.
using LayoutFieldValue = std::variant<std::uint64_t, std::int64_t, double, bool, std::string>;

struct LayoutField {
    std::string name;
    LayoutFieldValue value;
};

/// One contiguous region of the file.
struct LayoutRegion {
    std::uint64_t offset = 0u;
    std::uint64_t declared_size = 0u; ///< size the header claims for the payload
    std::uint64_t actual_size = 0u;   ///< payload bytes actually present in the file
    std::string id;                   ///< chunk id, box type, block type, or page label
    LayoutRegionKind kind = LayoutRegionKind::unknown;
    std::vector<LayoutField> fields; ///< fields the specification defines for this region
    std::string note;                ///< one-line clarification when the id alone misleads
};

/// Free text lifted from a metadata region. Emitted only on request.
struct LayoutMetadataText {
    std::string region_id;
    std::string key;
    std::string text;
};

/// Stream facts the container states about its audio.
struct LayoutStreamFacts {
    std::uint32_t sample_rate = 0u;
    std::uint32_t channels = 0u;
    std::uint32_t bits_per_sample = 0u; ///< container width
    std::uint32_t valid_bits =
        0u; ///< significant bits when the container says so, else bits_per_sample
    /// 0 when the container does not state a count. For WAV, the whole
    /// frames present: what a decode produces.
    std::uint64_t frames = 0u;
    double duration_seconds = 0.0; ///< 0 when frames or rate are unknown
    std::string
        encoding; ///< "pcm", "float", "adpcm", "alaw", "mulaw", "flac", "vorbis", "opus", "mp3", "aac", ...
    std::uint32_t bytes_per_frame = 0u; ///< 0 for compressed streams
    /// WAV only: the frames the header declares, a partial last frame
    /// counted. Larger than `frames` when the file is truncated; 0 for other
    /// containers.
    std::uint64_t declared_frames = 0u;
};

/// Whether the in-memory decoder would accept the file as laid out, and the
/// first reason it would not. This is the same decision `inspect_wav` makes.
struct LayoutDecodeVerdict {
    bool decodable = false;
    std::string reason; ///< empty when decodable
    /// Decodable, but only the whole frames present: the header declares
    /// more (see LayoutStreamFacts::declared_frames).
    bool truncated = false;
};

/// `decode_wav`'s allocation ceilings applied to the frames a decode
/// produces (for WAV, the whole frames present).
struct LayoutDecodeLimitsFact {
    bool exceeds_frames = false;
    bool exceeds_channels = false;
    bool exceeds_output_bytes = false;
    WavDecodeLimits limits;
    std::uint64_t output_bytes = 0u; ///< frames x channels x sizeof(float), 0 on overflow

    [[nodiscard]] bool exceeds() const noexcept {
        return exceeds_frames || exceeds_channels || exceeds_output_bytes;
    }
    /// Names of the exceeded ceilings in check order: max_frames,
    /// max_channels, max_output_bytes.
    [[nodiscard]] std::vector<std::string_view> exceeded_names() const;
    /// "max_frames=N max_channels=N" for each exceeded ceiling; empty when
    /// none is.
    [[nodiscard]] std::string describe() const;
};

/// Description of a headerless PCM payload. Every field is required; the
/// inspector never guesses any of them from content.
struct RawPcmSpec {
    enum class Format { s8, u8, s16, s24, s32, f32, f64 };
    enum class Endian { little, big };
    Format format = Format::s16;
    Endian endian = Endian::little;
    std::uint32_t channels = 0u;
    std::uint32_t sample_rate = 0u;
    std::uint64_t offset = 0u; ///< byte offset where sample data begins
};

/// Options for a layout inspection.
struct LayoutInspectOptions {
    /// Lift free text (bext strings, LIST-INFO, iXML, Vorbis comments) into
    /// `metadata_text`. Off by default so the report can travel.
    bool include_metadata_text = false;
    /// Ceilings the decode-limits fact is evaluated against.
    WavDecodeLimits decode_limits{};
    /// Headerless PCM description; when set the bytes are treated as raw PCM.
    std::optional<RawPcmSpec> raw;
};

/// The layout report.
struct AudioFileLayoutReport {
    AudioContainer container = AudioContainer::unknown;
    std::string
        container_detail; ///< envelope or brand: "RIFF", "RF64", "W64", "FORM", "fLaC", "OggS", "ftyp:M4A ", ...
    std::uint64_t file_size = 0u;
    std::uint64_t bytes_read = 0u; ///< bytes the inspector requested from the source
    FactsConfidence facts_confidence = FactsConfidence::parsed;
    std::optional<WavProfile> profile;
    std::vector<LayoutRegion> regions;
    bool regions_truncated = false; ///< the walk stopped at its region cap
    std::vector<LayoutWarning> warnings;
    std::optional<LayoutStreamFacts> stream;
    std::optional<LayoutDecodeVerdict> decode;
    std::optional<LayoutDecodeLimitsFact> decode_limits;
    std::vector<LayoutMetadataText> metadata_text;
    /// Raw mode only: bytes after the last whole frame.
    std::optional<std::uint64_t> truncated_tail_bytes;
    /// Set when the inspector could not read the source at all.
    std::string error;

    /// True when no warning was recorded.
    [[nodiscard]] bool is_standard() const noexcept {
        return warnings.empty();
    }
    /// Regions of kind `metadata`, which is what `metadata.present` lists.
    [[nodiscard]] std::vector<const LayoutRegion*> metadata_regions() const;
};

/// Inspect bytes through a source. Reads are bounded to headers and small
/// payloads regardless of `source.size()`.
[[nodiscard]] AudioFileLayoutReport
inspect_audio_file_layout(LayoutByteSource& source, const LayoutInspectOptions& options = {});

/// Inspect a complete in-memory file.
[[nodiscard]] AudioFileLayoutReport
inspect_audio_file_layout(std::span<const std::uint8_t> bytes,
                          const LayoutInspectOptions& options = {});

/// Inspect a file on disk with positional reads. A file that cannot be
/// opened yields a report whose `error` names the failure.
[[nodiscard]] AudioFileLayoutReport
inspect_audio_file_layout(const std::filesystem::path& path,
                          const LayoutInspectOptions& options = {});

struct DecodeLimits;
/// The format registry's decode ceilings as the inspector's option type.
[[nodiscard]] WavDecodeLimits as_wav_decode_limits(const DecodeLimits& limits) noexcept;

/// Names for the enumerations, as they appear in JSON.
[[nodiscard]] const char* audio_container_name(AudioContainer container) noexcept;
[[nodiscard]] const char* layout_region_kind_name(LayoutRegionKind kind) noexcept;
[[nodiscard]] const char* wav_profile_name(WavProfile profile) noexcept;
[[nodiscard]] const char* facts_confidence_name(FactsConfidence confidence) noexcept;
[[nodiscard]] const char* layout_warning_kind_name(LayoutWarningKind kind) noexcept;
[[nodiscard]] const char* raw_pcm_format_name(RawPcmSpec::Format format) noexcept;
[[nodiscard]] std::optional<RawPcmSpec::Format>
parse_raw_pcm_format(std::string_view name) noexcept;
[[nodiscard]] std::uint32_t raw_pcm_bytes_per_sample(RawPcmSpec::Format format) noexcept;

} // namespace pulp::audio
