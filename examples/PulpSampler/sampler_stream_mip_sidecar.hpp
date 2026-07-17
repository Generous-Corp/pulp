#pragma once

#include <pulp/audio/streaming_sample_source_file.hpp>
#include <pulp/runtime/crypto.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace pulp::examples {

// Sidecar hashes detect stale or corrupt trusted local content at admission.
// As with Pulp's base memory-mapped sample reader, mutating or truncating an
// admitted file concurrently with playback is outside the file-owner contract.

enum class SamplerStreamMipSidecarStatus : std::uint8_t {
    Absent,
    Valid,
    Invalid,
};

struct SamplerStreamMipSidecar {
    static constexpr std::uint32_t kMaximumLevels = 2;

    struct Level {
        audio::FileFrameReader reader{};
        double sample_rate = 0.0;
        std::uint32_t octave = 0;
    };

    SamplerStreamMipSidecarStatus status = SamplerStreamMipSidecarStatus::Absent;
    std::array<Level, kMaximumLevels> levels{};
    std::uint32_t level_count = 0;
};

namespace sampler_stream_mip_detail {

constexpr std::array<std::uint8_t, 8> kMagic{
    'P', 'U', 'L', 'P', 'M', 'I', 'P', 0};
constexpr std::uint16_t kVersion = 1;
constexpr std::uint16_t kHeaderBytes = 92;
constexpr std::uint32_t kRecordBytes = 80;
constexpr std::uint32_t kBuilderRevision = 1;
constexpr std::uint64_t kMaximumManifestBytes =
    kHeaderBytes + SamplerStreamMipSidecar::kMaximumLevels * kRecordBytes;

class ByteCursor {
public:
    explicit ByteCursor(const std::vector<std::uint8_t>& bytes) : bytes_(bytes) {}

    bool read_bytes(std::uint8_t* destination, std::size_t count) noexcept {
        if (count > bytes_.size() - position_) return false;
        std::copy_n(bytes_.data() + position_, count, destination);
        position_ += count;
        return true;
    }

    template<typename Integer>
    bool read_le(Integer& value) noexcept {
        static_assert(std::is_unsigned_v<Integer>);
        if (sizeof(Integer) > bytes_.size() - position_) return false;
        value = 0;
        for (std::size_t byte = 0; byte < sizeof(Integer); ++byte) {
            value |= static_cast<Integer>(bytes_[position_ + byte]) << (byte * 8);
        }
        position_ += sizeof(Integer);
        return true;
    }

    bool read_zeroes(std::size_t count) noexcept {
        if (count > bytes_.size() - position_) return false;
        const auto begin = bytes_.begin() + static_cast<std::ptrdiff_t>(position_);
        if (!std::all_of(begin, begin + static_cast<std::ptrdiff_t>(count),
                         [](std::uint8_t byte) { return byte == 0; })) {
            return false;
        }
        position_ += count;
        return true;
    }

    bool at_end() const noexcept { return position_ == bytes_.size(); }

private:
    const std::vector<std::uint8_t>& bytes_;
    std::size_t position_ = 0;
};

struct ManifestLevel {
    std::uint32_t octave = 0;
    std::uint32_t decimation = 0;
    std::uint64_t frames = 0;
    std::uint32_t rate_numerator = 0;
    std::uint32_t rate_denominator = 0;
    std::uint64_t payload_bytes = 0;
    std::array<std::uint8_t, 32> payload_sha256{};
};

inline bool regular_file(const std::filesystem::path& path) noexcept {
    std::error_code error;
    const auto status = std::filesystem::status(path, error);
    return !error && std::filesystem::is_regular_file(status);
}

inline std::string payload_path(std::string_view source_path,
                                const std::array<std::uint8_t, 32>& source_sha,
                                std::uint32_t octave) {
    auto result = std::string(source_path);
    result += ".pulpmip-";
    result += runtime::hex_encode(source_sha.data(), source_sha.size());
    result += ".L";
    if (octave < 10) result += '0';
    result += std::to_string(octave);
    result += ".wav";
    return result;
}

} // namespace sampler_stream_mip_detail

inline bool sampler_stream_mip_sidecar_exists(
    std::string_view source_path) noexcept {
    std::error_code error;
    const auto path = std::filesystem::path(
        std::string(source_path) + ".pulpmip");
    return std::filesystem::exists(path, error) && !error;
}

inline SamplerStreamMipSidecar load_sampler_stream_mip_sidecar(
    std::string_view source_path,
    const audio::FileFrameReader& source) {
    using namespace sampler_stream_mip_detail;
    SamplerStreamMipSidecar result;
    const auto manifest_path = std::filesystem::path(
        std::string(source_path) + ".pulpmip");
    std::error_code error;
    if (!std::filesystem::exists(manifest_path, error) || error) return result;
    result.status = SamplerStreamMipSidecarStatus::Invalid;
    if (!source.valid || !source.has_content_identity ||
        !regular_file(manifest_path)) {
        return result;
    }

    const auto manifest_size = std::filesystem::file_size(manifest_path, error);
    if (error || manifest_size < kHeaderBytes ||
        manifest_size > kMaximumManifestBytes) {
        return result;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(manifest_size));
    std::ifstream input(manifest_path, std::ios::binary);
    if (!input.read(reinterpret_cast<char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size())) ||
        input.peek() != std::ifstream::traits_type::eof()) {
        return result;
    }

    ByteCursor cursor(bytes);
    std::array<std::uint8_t, 8> magic{};
    std::uint16_t version = 0;
    std::uint16_t header_bytes = 0;
    std::uint32_t level_count = 0;
    std::array<std::uint8_t, 32> source_sha{};
    std::uint64_t source_bytes = 0;
    std::uint32_t source_channels = 0;
    std::uint64_t source_frames = 0;
    std::uint32_t source_rate = 0;
    std::uint32_t builder_revision = 0;
    if (!cursor.read_bytes(magic.data(), magic.size()) || magic != kMagic ||
        !cursor.read_le(version) || version != kVersion ||
        !cursor.read_le(header_bytes) || header_bytes != kHeaderBytes ||
        !cursor.read_le(level_count) || level_count == 0 ||
        level_count > SamplerStreamMipSidecar::kMaximumLevels ||
        manifest_size != kHeaderBytes + level_count * kRecordBytes ||
        !cursor.read_bytes(source_sha.data(), source_sha.size()) ||
        !cursor.read_le(source_bytes) ||
        !cursor.read_le(source_channels) ||
        !cursor.read_le(source_frames) ||
        !cursor.read_le(source_rate) ||
        !cursor.read_le(builder_revision) ||
        builder_revision != kBuilderRevision || !cursor.read_zeroes(16)) {
        return result;
    }
    if (source_sha != source.content_sha256 ||
        source_bytes != source.mapped_byte_size ||
        source_channels != source.channels || source_frames != source.total_frames ||
        source_rate != source.sample_rate) {
        return result;
    }

    std::array<SamplerStreamMipSidecar::Level,
               SamplerStreamMipSidecar::kMaximumLevels> staged_levels{};
    std::uint64_t previous_frames = source.total_frames;
    for (std::uint32_t index = 0; index < level_count; ++index) {
        ManifestLevel level;
        if (!cursor.read_le(level.octave) ||
            !cursor.read_le(level.decimation) ||
            !cursor.read_le(level.frames) ||
            !cursor.read_le(level.rate_numerator) ||
            !cursor.read_le(level.rate_denominator) ||
            !cursor.read_le(level.payload_bytes) ||
            !cursor.read_bytes(level.payload_sha256.data(),
                               level.payload_sha256.size()) ||
            !cursor.read_zeroes(16)) {
            return result;
        }
        const auto octave = index + 1;
        const auto decimation = std::uint32_t{1} << octave;
        const auto expected_frames = (previous_frames + 1) / 2;
        if (level.octave != octave || level.decimation != decimation ||
            level.frames != expected_frames ||
            level.rate_numerator != source.sample_rate ||
            level.rate_denominator != decimation ||
            level.payload_bytes == 0) {
            return result;
        }

        const auto logical_rate =
            static_cast<double>(level.rate_numerator) /
            static_cast<double>(level.rate_denominator);
        const auto encoded_rate = static_cast<std::uint32_t>(
            std::llround(logical_rate));
        if (encoded_rate == 0) return result;

        const auto path = payload_path(source_path, source.content_sha256, octave);
        if (!regular_file(path)) return result;
        auto opened = audio::make_memory_mapped_frame_reader(path, true, true);
        if (!opened.valid || !opened.has_content_identity ||
            opened.mapped_byte_size != level.payload_bytes ||
            opened.content_sha256 != level.payload_sha256 ||
            opened.channels != source.channels ||
            opened.total_frames != level.frames ||
            opened.sample_rate != encoded_rate) {
            return result;
        }
        staged_levels[index] = {
            .reader = std::move(opened),
            .sample_rate = logical_rate,
            .octave = octave,
        };
        previous_frames = level.frames;
    }
    if (!cursor.at_end()) return result;
    result.levels = std::move(staged_levels);
    result.level_count = level_count;
    result.status = SamplerStreamMipSidecarStatus::Valid;
    return result;
}

} // namespace pulp::examples
