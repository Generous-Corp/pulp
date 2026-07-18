#include "sample_mip_sidecar_internal.hpp"

#include <pulp/audio/audio_file.hpp>
#include <pulp/runtime/crypto.hpp>
#include <pulp/runtime/inter_process_lock.hpp>
#include <pulp/runtime/memory_mapped_file.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <thread>
#include <type_traits>
#include <utility>

namespace pulp::audio::sample_mip_detail {

namespace {

struct ManifestHeader {
    std::uint32_t level_count = 0;
    std::array<std::uint8_t, 32> source_sha256{};
    std::uint64_t source_bytes = 0;
    std::uint32_t source_channels = 0;
    std::uint64_t source_frames = 0;
    std::uint32_t source_rate = 0;
    std::uint32_t builder_revision = 0;
    runtime::FileIdentity source_identity{};
    ManifestNamespace manifest_namespace{};
};

class ByteCursor {
  public:
    explicit ByteCursor(const std::vector<std::uint8_t>& bytes) : bytes_(bytes) {}

    bool read_bytes(std::uint8_t* destination, std::size_t count) noexcept {
        if (count > bytes_.size() - position_)
            return false;
        std::copy_n(bytes_.data() + position_, count, destination);
        position_ += count;
        return true;
    }

    template <typename Integer> bool read_le(Integer& value) noexcept {
        static_assert(std::is_unsigned_v<Integer>);
        if (sizeof(Integer) > bytes_.size() - position_)
            return false;
        value = 0;
        for (std::size_t byte = 0; byte < sizeof(Integer); ++byte)
            value |= static_cast<Integer>(bytes_[position_ + byte]) << (byte * 8);
        position_ += sizeof(Integer);
        return true;
    }

    bool read_zeroes(std::size_t count) noexcept {
        if (count > bytes_.size() - position_)
            return false;
        const auto begin = bytes_.begin() + static_cast<std::ptrdiff_t>(position_);
        if (!std::all_of(begin, begin + static_cast<std::ptrdiff_t>(count),
                         [](std::uint8_t byte) { return byte == 0; }))
            return false;
        position_ += count;
        return true;
    }

    bool at_end() const noexcept {
        return position_ == bytes_.size();
    }

  private:
    const std::vector<std::uint8_t>& bytes_;
    std::size_t position_ = 0;
};

std::optional<ManifestHeader>
decode_manifest_header(ByteCursor& cursor, std::size_t manifest_size, const FileFrameReader& source,
                       const runtime::FileIdentity& source_identity) noexcept {
    std::array<std::uint8_t, 8> magic{};
    std::uint16_t version = 0;
    std::uint16_t header_bytes = 0;
    ManifestHeader header;
    if (!cursor.read_bytes(magic.data(), magic.size()) || magic != kMagic ||
        !cursor.read_le(version) || version != kVersion || !cursor.read_le(header_bytes) ||
        header_bytes != kHeaderBytes || !cursor.read_le(header.level_count) ||
        header.level_count == 0 || header.level_count > SampleMipSidecar::kMaximumLevels ||
        manifest_size != kHeaderBytes + header.level_count * kRecordBytes ||
        !cursor.read_bytes(header.source_sha256.data(), header.source_sha256.size()) ||
        !cursor.read_le(header.source_bytes) || !cursor.read_le(header.source_channels) ||
        !cursor.read_le(header.source_frames) || !cursor.read_le(header.source_rate) ||
        !cursor.read_le(header.builder_revision) || header.builder_revision != kBuilderRevision ||
        !cursor.read_le(header.source_identity.volume) ||
        !cursor.read_le(header.source_identity.file) ||
        !cursor.read_le(header.source_identity.generation) ||
        !cursor.read_bytes(header.manifest_namespace.data(), header.manifest_namespace.size())) {
        return std::nullopt;
    }
    header.source_identity.valid = true;
    if (header.source_sha256 != source.content_sha256 ||
        header.source_bytes != source.mapped_byte_size ||
        header.source_channels != source.channels || header.source_frames != source.total_frames ||
        header.source_rate != source.sample_rate || !source_identity.valid ||
        header.source_identity != source_identity) {
        return std::nullopt;
    }
    return header;
}

} // namespace

bool regular_file(const std::filesystem::path& path) noexcept {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    return !error && std::filesystem::is_regular_file(status);
}

std::filesystem::path normalized_source_path(std::string_view source_path) {
    std::error_code error;
    auto normalized =
        std::filesystem::weakly_canonical(std::filesystem::path(std::string(source_path)), error);
    if (error)
        return {};
    return normalized.lexically_normal();
}

std::filesystem::path canonical_parent_path(const std::filesystem::path& path) {
    std::error_code error;
    auto parent = path.parent_path();
    if (parent.empty())
        parent = ".";
    auto canonical = std::filesystem::weakly_canonical(parent, error);
    return error ? std::filesystem::path{} : canonical.lexically_normal();
}

ManifestNamespace default_manifest_namespace(std::string_view source_path) {
    const auto source = std::filesystem::path(std::string(source_path));
    const auto parent = canonical_parent_path(source);
    if (parent.empty() || source.filename().empty())
        return {};
    const auto spelling = (parent / source.filename()).lexically_normal().generic_string();
    const auto digest =
        runtime::sha256(reinterpret_cast<const std::uint8_t*>(spelling.data()), spelling.size());
    ManifestNamespace result{};
    std::copy_n(digest.begin(), result.size(), result.begin());
    return result;
}
template <typename Integer> void append_le(std::vector<std::uint8_t>& bytes, Integer value) {
    static_assert(std::is_unsigned_v<Integer>);
    for (std::size_t byte = 0; byte < sizeof(Integer); ++byte)
        bytes.push_back(static_cast<std::uint8_t>(value >> (byte * 8)));
}

void append_bytes(std::vector<std::uint8_t>& bytes, const std::uint8_t* data, std::size_t count) {
    bytes.insert(bytes.end(), data, data + count);
}

bool write_manifest(const std::filesystem::path& path, const FileFrameReader& source,
                    const runtime::FileIdentity& source_identity,
                    const ManifestNamespace& manifest_namespace,
                    const std::vector<ManifestLevel>& levels) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(kHeaderBytes + levels.size() * kRecordBytes);
    append_bytes(bytes, kMagic.data(), kMagic.size());
    append_le(bytes, kVersion);
    append_le(bytes, kHeaderBytes);
    append_le(bytes, static_cast<std::uint32_t>(levels.size()));
    append_bytes(bytes, source.content_sha256.data(), source.content_sha256.size());
    append_le(bytes, source.mapped_byte_size);
    append_le(bytes, source.channels);
    append_le(bytes, source.total_frames);
    append_le(bytes, source.sample_rate);
    append_le(bytes, kBuilderRevision);
    append_le(bytes, source_identity.volume);
    append_le(bytes, source_identity.file);
    append_le(bytes, source_identity.generation);
    append_bytes(bytes, manifest_namespace.data(), manifest_namespace.size());
    for (const auto& level : levels) {
        append_le(bytes, level.octave);
        append_le(bytes, level.decimation);
        append_le(bytes, level.frames);
        append_le(bytes, level.rate_numerator);
        append_le(bytes, level.rate_denominator);
        append_le(bytes, level.payload_bytes);
        append_bytes(bytes, level.payload_sha256.data(), level.payload_sha256.size());
        bytes.insert(bytes.end(), 16, 0);
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    output.flush();
    if (!output.good())
        return false;
    output.close();
    return !output.fail();
}

bool bounded_payload_file(const std::filesystem::path& path, std::uint64_t expected_bytes,
                          std::uint64_t frames, std::uint32_t channels) noexcept {
    if (!regular_file(path) || channels == 0 ||
        frames > std::numeric_limits<std::uint64_t>::max() / channels)
        return false;
    const auto samples = frames * channels;
    if (samples >
        (std::numeric_limits<std::uint64_t>::max() - kMaximumWavContainerOverhead) / sizeof(float))
        return false;
    const auto maximum_bytes = samples * sizeof(float) + kMaximumWavContainerOverhead;
    if (expected_bytes == 0 || expected_bytes > maximum_bytes)
        return false;
    std::error_code error;
    return std::filesystem::file_size(path, error) == expected_bytes && !error;
}

std::string sample_mip_payload_prefix(const ManifestNamespace& manifest_namespace,
                                      const std::array<std::uint8_t, 32>& source_sha256) {
    return ".pulp-mip-" + runtime::hex_encode(manifest_namespace.data(), 12) + "-" +
           runtime::hex_encode(source_sha256.data(), 8) + "-";
}

std::string sample_mip_namespace_prefix(const ManifestNamespace& manifest_namespace) {
    return ".pulp-mip-" + runtime::hex_encode(manifest_namespace.data(), 12) + "-";
}

std::string sample_mip_payload_path_for_namespace(
    std::string_view source_path, const ManifestNamespace& manifest_namespace,
    const std::array<std::uint8_t, 32>& source_sha256,
    const std::array<std::uint8_t, 32>& payload_sha256, std::uint32_t octave) {
    const auto source = normalized_source_path(source_path);
    if (source.empty())
        return {};
    auto name = sample_mip_payload_prefix(manifest_namespace, source_sha256) + "L";
    if (octave < 10)
        name += '0';
    name += std::to_string(octave);
    name += "-";
    name += runtime::hex_encode(payload_sha256.data(), 16);
    name += ".wav";
    return (source.parent_path() / name).string();
}

std::optional<ManifestNamespace>
read_manifest_namespace(const std::filesystem::path& path, const FileFrameReader& source,
                        const runtime::FileIdentity& source_identity) {
    runtime::MemoryMappedFile manifest;
    if (!manifest.open(path.string(), runtime::MapMode::ReadOnly,
                       static_cast<std::size_t>(kMaximumManifestBytes)) ||
        manifest.size() < kHeaderBytes || manifest.size() > kMaximumManifestBytes) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> bytes(manifest.data(), manifest.data() + manifest.size());
    ByteCursor cursor(bytes);
    const auto header = decode_manifest_header(cursor, manifest.size(), source, source_identity);
    return header ? std::optional{header->manifest_namespace} : std::nullopt;
}

} // namespace pulp::audio::sample_mip_detail

namespace pulp::audio {

using namespace sample_mip_detail;

bool sample_mip_sidecar_exists(std::string_view source_path) noexcept {
    std::error_code error;
    const auto path = std::filesystem::path(std::string(source_path) + ".pulpmip");
    return std::filesystem::exists(path, error) && !error;
}

std::string sample_mip_payload_path(std::string_view source_path,
                                    const std::array<std::uint8_t, 32>& source_sha256,
                                    const std::array<std::uint8_t, 32>& payload_sha256,
                                    std::uint32_t octave) {
    return sample_mip_payload_path_for_namespace(source_path,
                                                 default_manifest_namespace(source_path),
                                                 source_sha256, payload_sha256, octave);
}

} // namespace pulp::audio

namespace pulp::audio::sample_mip_detail {

SampleMipSidecar load_sample_mip_sidecar_from_manifest(
    std::string_view source_path, const std::filesystem::path& manifest_path,
    const FileFrameReader& source, const runtime::FileIdentity& retained_identity) {
    SampleMipSidecar result;
    std::error_code error;
    if (!std::filesystem::exists(manifest_path, error) || error)
        return result;
    result.status = SampleMipSidecarStatus::Invalid;
    if (!source.valid || !source.has_content_identity || !regular_file(manifest_path))
        return result;

    runtime::MemoryMappedFile manifest;
    if (!manifest.open(manifest_path.string(), runtime::MapMode::ReadOnly,
                       static_cast<std::size_t>(kMaximumManifestBytes)))
        return result;
    const auto manifest_size = manifest.size();
    if (manifest_size < kHeaderBytes || manifest_size > kMaximumManifestBytes)
        return result;
    std::vector<std::uint8_t> bytes(manifest.data(), manifest.data() + manifest.size());

    ByteCursor cursor(bytes);
    const auto header = decode_manifest_header(cursor, manifest_size, source, retained_identity);
    if (!header)
        return result;

    std::array<SampleMipSidecar::Level, SampleMipSidecar::kMaximumLevels> staged{};
    std::uint64_t previous_frames = source.total_frames;
    for (std::uint32_t index = 0; index < header->level_count; ++index) {
        ManifestLevel level;
        if (!cursor.read_le(level.octave) || !cursor.read_le(level.decimation) ||
            !cursor.read_le(level.frames) || !cursor.read_le(level.rate_numerator) ||
            !cursor.read_le(level.rate_denominator) || !cursor.read_le(level.payload_bytes) ||
            !cursor.read_bytes(level.payload_sha256.data(), level.payload_sha256.size()) ||
            !cursor.read_zeroes(16))
            return result;
        const auto octave = index + 1;
        const auto decimation = std::uint32_t{1} << octave;
        const auto expected_frames = (previous_frames + 1) / 2;
        if (level.octave != octave || level.decimation != decimation ||
            level.frames != expected_frames || level.rate_numerator != source.sample_rate ||
            level.rate_denominator != decimation || level.payload_bytes == 0)
            return result;
        const auto logical_rate =
            static_cast<double>(level.rate_numerator) / static_cast<double>(level.rate_denominator);
        const auto encoded_rate = static_cast<std::uint32_t>(std::llround(logical_rate));
        if (encoded_rate == 0)
            return result;
        const auto path = sample_mip_payload_path_for_namespace(
            source_path, header->manifest_namespace, source.content_sha256, level.payload_sha256,
            octave);
        if (!bounded_payload_file(path, level.payload_bytes, level.frames, source.channels))
            return result;
        auto opened = make_memory_mapped_frame_reader(path, true, true, level.payload_bytes);
        if (!opened.valid || !opened.has_content_identity ||
            opened.mapped_byte_size != level.payload_bytes ||
            opened.content_sha256 != level.payload_sha256 || opened.channels != source.channels ||
            opened.total_frames != level.frames || opened.sample_rate != encoded_rate)
            return result;
        staged[index] = {
            .reader = std::move(opened), .sample_rate = logical_rate, .octave = octave};
        previous_frames = level.frames;
    }
    if (!cursor.at_end())
        return result;
    result.levels = std::move(staged);
    result.level_count = header->level_count;
    result.status = SampleMipSidecarStatus::Valid;
    return result;
}

} // namespace pulp::audio::sample_mip_detail

namespace pulp::audio {

using namespace sample_mip_detail;

SampleMipSidecar
load_sample_mip_sidecar(std::string_view source_path, const FileFrameReader& source,
                        const std::shared_ptr<MemoryMappedAudioReader>& retained_source) {
    if (!retained_source)
        return {.status = SampleMipSidecarStatus::Invalid};
    const auto retained_identity = retained_source->opened_file_identity();
    if (!retained_identity.valid)
        return {.status = SampleMipSidecarStatus::Invalid};
    runtime::InterProcessLock load_lock(
        sample_mip_publication_lock_name(source_path, retained_identity));
    const auto lock_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!load_lock.try_lock_shared()) {
        if (std::chrono::steady_clock::now() >= lock_deadline)
            return {.status = SampleMipSidecarStatus::Invalid};
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return load_sample_mip_sidecar_from_manifest(
        source_path, std::filesystem::path(std::string(source_path) + ".pulpmip"), source,
        retained_identity);
}

} // namespace pulp::audio
