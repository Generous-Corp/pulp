#pragma once

// approach after SimZhou/vscode-audiolens c5c5a24; re-authored

// Internal seams shared by the per-container layout walkers. Each walker
// lives in its own translation unit and receives a Builder, which owns the
// report under construction and the bounded reader over the byte source.

#include <pulp/audio/audio_file_layout.hpp>

#include <cstddef>
#include <cstdint>
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
    void present(const std::string& id, std::uint64_t offset, std::uint64_t size);
    /// Record free text; a no-op unless the options ask for metadata text.
    void text(const std::string& region_id, std::string key, std::string value);
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
/// True when every byte of `id` is printable ASCII (after escaping, every
/// fourcc() result passes; use raw_id_printable on the bytes themselves).
bool printable_id(std::string_view id) noexcept;
/// True when the raw bytes of an id are all printable ASCII.
bool raw_id_printable(const std::uint8_t* p, std::size_t count) noexcept;
/// Replace control bytes (other than tab, newline, return) with `\xNN`
/// escapes. Bytes above 0x7f pass through so UTF-8 text survives.
std::string escape_control(std::string text);
std::string hex_bytes(const std::uint8_t* p, std::size_t count);
/// Bytes as text with trailing NULs and spaces removed and interior control
/// bytes escaped.
std::string trimmed_text(const std::uint8_t* p, std::size_t count);
/// UTF-16 (with or without BOM) to UTF-8, stopping at the first NUL unit.
std::string utf16_to_utf8(const std::uint8_t* p, std::size_t count, bool big_endian);

// Per-container walkers. Each fills regions, warnings, facts, and metadata.
void inspect_wav_layout(Builder& b);
void inspect_aiff_layout(Builder& b);
void inspect_flac_layout(Builder& b);
void inspect_ogg_layout(Builder& b);
void inspect_mp3_layout(Builder& b);
void inspect_mp4_layout(Builder& b);
void inspect_raw(Builder& b, const RawPcmSpec& spec);

} // namespace pulp::audio::layout_detail
