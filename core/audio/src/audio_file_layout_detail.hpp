#pragma once

// approach after SimZhou/vscode-audiolens c5c5a24; re-authored

// Internal seams shared by the per-container layout walkers. Each walker
// lives in its own translation unit and receives a Builder, which owns the
// report under construction and the bounded reader over the byte source.

#include <pulp/audio/audio_file_layout.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::audio::layout_detail {

/// Largest payload any walker reads for field parsing or text extraction.
/// Anything bigger is read up to this cap and noted as truncated.
inline constexpr std::uint64_t kMaxPayloadRead = 64u * 1024u;

/// Region count at which the walk stops and the report says so.
inline constexpr std::size_t kMaxRegions = 4096u;

/// Bounded reads over a LayoutByteSource with a running byte count.
class Reader {
  public:
    explicit Reader(LayoutByteSource& source) noexcept;

    [[nodiscard]] std::uint64_t size() const noexcept {
        return size_;
    }
    [[nodiscard]] std::uint64_t bytes_read() const noexcept {
        return bytes_read_;
    }

    /// Read up to `length` bytes at `offset`; shorter at end of input.
    [[nodiscard]] std::vector<std::uint8_t> read(std::uint64_t offset, std::uint64_t length);

    /// Read a payload that declares `declared` bytes, never more than `cap`.
    [[nodiscard]] std::vector<std::uint8_t> read_capped(std::uint64_t offset,
                                                        std::uint64_t declared, std::uint64_t cap);

    /// Fill `out` exactly, or return false without partial results.
    [[nodiscard]] bool read_exact(std::uint64_t offset, std::span<std::uint8_t> out);

  private:
    LayoutByteSource& source_;
    std::uint64_t size_ = 0u;
    std::uint64_t bytes_read_ = 0u;
};

/// Report under construction plus the helpers every walker uses.
struct Builder {
    Builder(LayoutByteSource& source, const LayoutInspectOptions& opts) noexcept
        : reader(source), options(opts) {}

    Reader reader;
    const LayoutInspectOptions& options;
    AudioFileLayoutReport report;

    /// Append a region whose payload of `declared` bytes starts `header_size`
    /// bytes after `offset`; `actual_size` is the payload present in the
    /// file. Past the cap the report is marked truncated and a scratch
    /// region is returned so callers need no branch.
    LayoutRegion& add_region(std::uint64_t offset, std::uint64_t header_size,
                             std::uint64_t declared, std::string id, LayoutRegionKind kind);
    void warn(LayoutWarningKind kind, std::uint64_t offset, std::string detail);
    /// Record free text. This is the one sanitising boundary for file text:
    /// a no-op unless the options ask for metadata text, and control bytes
    /// are escaped here for every caller.
    void text(const std::string& region_id, std::string key, std::string value);
    /// Read a text payload (bounded by `cap`) and record it, performing the
    /// read only when the options ask for metadata text.
    void read_text(const std::string& region_id, std::string key, std::uint64_t payload_offset,
                   std::uint64_t declared, std::uint64_t cap = kMaxPayloadRead);
    /// Evaluate the decode ceilings for a declared stream shape.
    void set_decode_limits(std::uint64_t frames, std::uint32_t channels);

  private:
    LayoutRegion overflow_;
};

// Little- and big-endian field readers over raw bytes.
std::uint16_t read_u16le(const std::uint8_t* p) noexcept;
std::uint32_t read_u32le(const std::uint8_t* p) noexcept;
std::uint64_t read_u64le(const std::uint8_t* p) noexcept;
std::uint16_t read_u16be(const std::uint8_t* p) noexcept;
std::uint32_t read_u24be(const std::uint8_t* p) noexcept;
std::uint32_t read_u32be(const std::uint8_t* p) noexcept;
std::uint64_t read_u64be(const std::uint8_t* p) noexcept;

/// Four bytes as text. Bytes outside printable ASCII are written as `\xNN`
/// escapes so an id can never carry a NUL or a control byte into the report.
std::string fourcc(const std::uint8_t* p);
/// True when the raw bytes of an id are all printable ASCII.
bool raw_id_printable(const std::uint8_t* p, std::size_t count) noexcept;
/// Replace control bytes (other than tab, newline, return) with `\xNN`
/// escapes. Bytes above 0x7f pass through so UTF-8 text survives.
std::string escape_control(std::string text);
/// Bytes as text with trailing NULs and spaces removed. Not sanitised: the
/// result must go through Builder::text.
std::string trimmed_text(const std::uint8_t* p, std::size_t count);
/// UTF-16 (with or without BOM) to UTF-8, stopping at the first NUL unit.
std::string utf16_to_utf8(const std::uint8_t* p, std::size_t count, bool big_endian);

// ── IFF-style chunk walk (RIFF/WAVE, Wave64, FORM/AIFF) ─────────────────────

/// Envelope facts the shared chunk loop needs.
struct IffEnvelope {
    std::uint64_t header_size = 12u;      ///< bytes before the first chunk
    std::uint64_t chunk_header_size = 8u; ///< id plus size field
    std::uint64_t alignment = 2u;         ///< chunk start alignment (Wave64 uses 8)
    std::uint64_t declared_end = 0u;      ///< envelope end the header claims
    std::uint64_t walk_end = 0u;          ///< first byte the loop may not visit
    bool pad_bytes = true; ///< odd chunks carry a pad byte (RIFF, AIFF); Wave64 aligns instead
    std::string data_id = "data"; ///< the chunk whose overrun is truncated_data
};

/// One chunk as the loop sees it. `read_header` fills id, printable,
/// declared and sentinel; `resolve` may rewrite declared and set stop_after.
struct IffChunk {
    std::uint64_t offset = 0u;
    std::uint64_t payload_offset = 0u;
    std::string id;
    bool printable = true;
    std::uint64_t declared = 0u;
    bool sentinel = false;
    bool stop_after = false;
    bool past_eof = false;
    std::uint64_t chunk_end = 0u;
};

struct IffHooks {
    /// Read the chunk header at `offset`; nullopt ends the walk.
    std::function<std::optional<IffChunk>(std::uint64_t offset)> read_header;
    /// Region kind for a printable id.
    std::function<LayoutRegionKind(std::string_view id)> classify;
    /// Runs before the region is created; may rewrite `declared` (RF64
    /// sentinel sizes) or stop the walk after this chunk. Optional.
    std::function<void(IffChunk&)> resolve;
    /// Runs once the region exists and the overrun and pad checks are done.
    std::function<void(const IffChunk&, LayoutRegion&)> on_chunk;
    /// A condition that stops the strict decoder. Optional.
    std::function<void(std::string)> fault;
};

/// Walk chunks from `start` to `env.walk_end`, creating a region per chunk,
/// warning on overruns and missing pad bytes, and appending a trailing-bytes
/// region when the envelope ends before the file does.
void walk_iff_chunks(Builder& b, const IffEnvelope& env, std::uint64_t start,
                     const IffHooks& hooks);

// Per-container walkers. Each fills regions, warnings, facts, and metadata.
void inspect_wav_layout(Builder& b);
void inspect_aiff_layout(Builder& b);
void inspect_flac_layout(Builder& b);
void inspect_ogg_layout(Builder& b);
void inspect_mp3_layout(Builder& b);
void inspect_mp4_layout(Builder& b);
void inspect_raw(Builder& b, const RawPcmSpec& spec);

} // namespace pulp::audio::layout_detail
