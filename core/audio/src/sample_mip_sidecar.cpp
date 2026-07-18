#include "sample_mip_sidecar_internal.hpp"
#include <pulp/audio/sample_mip_sidecar.hpp>

#include <pulp/audio/audio_file.hpp>
#include <pulp/audio/sample_mip_builder.hpp>
#include <pulp/runtime/crypto.hpp>
#include <pulp/runtime/inter_process_lock.hpp>
#include <pulp/runtime/memory_mapped_file.hpp>
#include <pulp/runtime/scope_guard.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace pulp::audio {

namespace {

std::atomic<detail::SampleMipBuildFaultForTesting> sample_mip_build_fault_for_testing{};

} // namespace

void detail::set_sample_mip_build_fault_for_testing(SampleMipBuildFaultForTesting fault) noexcept {
    sample_mip_build_fault_for_testing.store(fault, std::memory_order_release);
}

namespace {

std::filesystem::path coordination_source_path(std::string_view source_path) {
    std::error_code error;
    auto normalized =
        std::filesystem::weakly_canonical(std::filesystem::path(std::string(source_path)), error);
    if (error) {
        error.clear();
        normalized =
            std::filesystem::absolute(std::filesystem::path(std::string(source_path)), error);
    }
    return error ? std::filesystem::path{} : normalized.lexically_normal();
}

bool source_filesystem_is_case_insensitive(const std::filesystem::path& source_path) {
    auto alternate_name = source_path.filename().string();
    const auto letter = std::find_if(alternate_name.begin(), alternate_name.end(),
                                     [](unsigned char value) { return std::isalpha(value) != 0; });
    if (letter == alternate_name.end())
        return false;
    *letter = std::islower(static_cast<unsigned char>(*letter))
                  ? static_cast<char>(std::toupper(static_cast<unsigned char>(*letter)))
                  : static_cast<char>(std::tolower(static_cast<unsigned char>(*letter)));
    std::error_code error;
    return std::filesystem::equivalent(source_path, source_path.parent_path() / alternate_name,
                                       error) &&
           !error;
}

} // namespace

std::string
detail::sample_mip_coordination_key_for_manifest_path(std::string_view normalized_manifest_path,
                                                      bool case_insensitive) {
    auto identity = std::string(normalized_manifest_path);
    if (case_insensitive) {
        std::transform(identity.begin(), identity.end(), identity.begin(),
                       [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    }
    const auto digest =
        runtime::sha256(reinterpret_cast<const std::uint8_t*>(identity.data()), identity.size());
    return runtime::hex_encode(digest.data(), 16);
}

std::string detail::sample_mip_coordination_key(std::string_view source_path,
                                                const runtime::FileIdentity&) {
    const auto source = coordination_source_path(source_path);
    if (source.empty())
        return {};
    auto manifest_path = source;
    manifest_path += ".pulpmip";
    return detail::sample_mip_coordination_key_for_manifest_path(
        manifest_path.generic_string(), source_filesystem_is_case_insensitive(source));
}

namespace sample_mip_detail {

std::string sample_mip_build_lock_name(std::string_view source_path,
                                       const runtime::FileIdentity& source_identity) {
    return "sampler_mip_build_" + detail::sample_mip_coordination_key(source_path, source_identity);
}

std::string sample_mip_publication_lock_name(std::string_view source_path,
                                             const runtime::FileIdentity& source_identity) {
    return "sampler_mip_publish_" +
           detail::sample_mip_coordination_key(source_path, source_identity);
}

} // namespace sample_mip_detail

namespace {

using namespace sample_mip_detail;

struct TemporaryDirectory {
    std::filesystem::path path;
    runtime::FileIdentity identity;
#ifdef _WIN32
    void* handle = nullptr;
#endif

    explicit TemporaryDirectory(std::filesystem::path created_path)
        : path(std::move(created_path)) {
#ifdef _WIN32
        HANDLE opened =
            CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL | DELETE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (opened != INVALID_HANDLE_VALUE) {
            BY_HANDLE_FILE_INFORMATION information{};
            DWORD filesystem_flags = 0;
            const bool information_read = GetFileInformationByHandle(opened, &information) != 0;
            const bool safe_directory =
                information_read &&
                (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
                (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
            if (safe_directory &&
                GetVolumeInformationByHandleW(opened, nullptr, 0, nullptr, nullptr,
                                              &filesystem_flags, nullptr, 0) != 0 &&
                (filesystem_flags & FILE_PERSISTENT_ACLS) != 0 &&
                temporary_directory_has_private_security(opened)) {
                handle = opened;
                identity = {.volume = information.dwVolumeSerialNumber,
                            .file = (static_cast<std::uint64_t>(information.nFileIndexHigh) << 32) |
                                    information.nFileIndexLow,
                            .generation = 0,
                            .valid = true};
            } else {
                if (safe_directory) {
                    FILE_DISPOSITION_INFO disposition{TRUE};
                    SetFileInformationByHandle(opened, FileDispositionInfo, &disposition,
                                               sizeof(disposition));
                }
                CloseHandle(opened);
            }
        } else {
            RemoveDirectoryW(path.c_str());
        }
#else
        identity = temporary_directory_identity(path);
#endif
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    TemporaryDirectory(TemporaryDirectory&&) = delete;
    TemporaryDirectory& operator=(TemporaryDirectory&&) = delete;

    bool valid() const noexcept {
        if (path.empty() || !identity.valid)
            return false;
#ifdef _WIN32
        if (handle == nullptr)
            return false;
#endif
        const auto current = temporary_directory_identity(path);
        return current.valid && current.volume == identity.volume && current.file == identity.file;
    }

    ~TemporaryDirectory() {
#ifdef _WIN32
        if (valid()) {
            std::error_code error;
            for (std::filesystem::directory_iterator iterator(path, error), end;
                 !error && iterator != end; iterator.increment(error)) {
                std::error_code status_error;
                const auto status = std::filesystem::symlink_status(iterator->path(), status_error);
                if (status_error)
                    continue;
                if (std::filesystem::is_regular_file(status)) {
                    std::error_code remove_error;
                    std::filesystem::remove(iterator->path(), remove_error);
                }
            }
        }
        if (handle != nullptr) {
            FILE_DISPOSITION_INFO disposition{TRUE};
            SetFileInformationByHandle(static_cast<HANDLE>(handle), FileDispositionInfo,
                                       &disposition, sizeof(disposition));
            CloseHandle(static_cast<HANDLE>(handle));
            handle = nullptr;
        }
#else
        if (!valid())
            return;
        std::error_code error;
        std::filesystem::remove_all(path, error);
#endif
    }
};

struct PublishedPayloadRollback {
    std::array<std::filesystem::path, SampleMipSidecar::kMaximumLevels> paths;
    std::array<bool, SampleMipSidecar::kMaximumLevels> remove_on_failure{};
    std::size_t count = 0;
    bool committed = false;
    ~PublishedPayloadRollback() {
        if (committed)
            return;
        for (std::size_t index = 0; index < count; ++index) {
            if (!remove_on_failure[index])
                continue;
            std::error_code error;
            std::filesystem::remove(paths[index], error);
        }
    }

    void prepare(const std::filesystem::path& path) {
        paths[count] = path;
        remove_on_failure[count] = true;
        ++count;
    }

    void retain_last_on_failure() noexcept {
        remove_on_failure[count - 1] = false;
    }

    void discard_last() noexcept {
        --count;
    }
};

} // namespace

using namespace sample_mip_detail;

SampleMipBuildResult build_sample_mip_sidecar(std::string_view source_path,
                                              const SampleMipBuildOptions& options) {
    SampleMipBuildResult result;
    result.manifest_path = std::string(source_path) + ".pulpmip";
    if (options.level_count == 0 || options.level_count > SampleMipSidecar::kMaximumLevels) {
        result.error = "level count must be between 1 and 2";
        return result;
    }
    const auto test_fault = sample_mip_build_fault_for_testing.load(std::memory_order_acquire);
    std::shared_ptr<MemoryMappedAudioReader> source_identity;
    auto source = make_memory_mapped_frame_reader(source_path, true, true,
                                                  options.maximum_source_bytes, &source_identity);
    if (!source.valid || !source.has_content_identity || !source_identity) {
        result.error = "source must be a readable seekable audio file";
        return result;
    }
    const auto opened_source_identity = source_identity->opened_file_identity();
    if (!opened_source_identity.valid) {
        result.error = "failed to capture the source file identity";
        return result;
    }
    const auto normalized_source = normalized_source_path(source_path);
    const auto normalized_string = normalized_source.generic_string();
    if (normalized_source.empty() ||
        runtime::file_identity(normalized_string) != opened_source_identity) {
        result.error = "source path changed while opening the mip input";
        return result;
    }
    const auto publication_source =
        canonical_parent_path(std::filesystem::path(std::string(source_path))) /
        std::filesystem::path(std::string(source_path)).filename();
    if (publication_source.empty()) {
        result.error = "failed to resolve the mip publication directory";
        return result;
    }
    result.manifest_path = publication_source.generic_string() + ".pulpmip";
    runtime::InterProcessLock build_lock(
        sample_mip_build_lock_name(source_path, opened_source_identity));
    const auto lock_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!build_lock.try_lock()) {
        if (std::chrono::steady_clock::now() >= lock_deadline) {
            result.error = "timed out waiting for another mip builder";
            return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const auto source_unchanged = [&] {
        return source_identity->path_refers_to_open_file(source_path) &&
               source_identity->opened_file_identity() == opened_source_identity;
    };
    if (!source_unchanged() ||
        runtime::file_identity(normalized_string) != opened_source_identity) {
        result.error = "source path changed before mip construction";
        return result;
    }
    std::optional<ManifestNamespace> reusable_namespace;
    const auto prior_sidecar = load_sample_mip_sidecar_from_manifest(
        source_path, result.manifest_path, source, opened_source_identity);
    if (prior_sidecar.status == SampleMipSidecarStatus::Valid) {
        reusable_namespace =
            read_manifest_namespace(result.manifest_path, source, opened_source_identity);
    }
    const auto manifest_namespace =
        reusable_namespace.value_or(default_manifest_namespace(source_path));
    if (source.total_frames > std::numeric_limits<std::uint64_t>::max() / source.channels ||
        source.total_frames * source.channels > options.maximum_source_bytes / sizeof(float)) {
        result.error = "decoded source exceeds the configured byte limit";
        return result;
    }
    AudioFileData decoded;
    decoded.sample_rate = source.sample_rate;
    decoded.channels.resize(source.channels);
    std::vector<float*> decoded_channels(source.channels);
    for (std::uint32_t channel = 0; channel < source.channels; ++channel) {
        decoded.channels[channel].resize(static_cast<std::size_t>(source.total_frames));
        decoded_channels[channel] = decoded.channels[channel].data();
    }
    BufferView<float> decoded_view(decoded_channels.data(), source.channels,
                                   static_cast<std::size_t>(source.total_frames));
    if (source.reader(0, decoded_view, source.total_frames) != source.total_frames) {
        result.error = "failed to decode the mapped source identity";
        return result;
    }
    const auto coefficients = design_sample_mip_decimator();
    if (coefficients.empty() || (coefficients.size() & 1u) == 0u) {
        result.error = "failed to design the mip decimator";
        return result;
    }

    const auto source_file = normalized_source;
    TemporaryDirectory temporary{unique_temporary_directory(source_file.parent_path().empty()
                                                                ? std::filesystem::path{"."}
                                                                : source_file.parent_path())};
    if (!temporary.valid()) {
        result.error = "failed to create a private temporary directory";
        return result;
    }
    PublishedPayloadRollback published_payloads;
    std::vector<std::string> payload_paths;
    std::vector<ManifestLevel> manifest_levels;
    std::uint64_t output_bytes = 0;
    auto previous = std::move(decoded);
    for (std::uint32_t index = 0; index < options.level_count; ++index) {
        if (!temporary.valid()) {
            result.error = "private mip staging directory changed identity";
            break;
        }
        const auto octave = index + 1;
        const auto frames = (previous.num_frames() + 1) / 2;
        if (frames == 0 ||
            previous.num_channels() > std::numeric_limits<std::uint64_t>::max() / frames) {
            result.error = "mip dimensions overflow";
            break;
        }
        const auto samples = frames * previous.num_channels();
        if (samples > std::numeric_limits<std::uint64_t>::max() / sizeof(float) ||
            samples * sizeof(float) > options.maximum_output_bytes - output_bytes) {
            result.error = "mip payloads exceed the configured byte limit";
            break;
        }
        AudioFileData next;
        next.sample_rate = static_cast<std::uint32_t>(
            std::llround(static_cast<double>(source.sample_rate) /
                         static_cast<double>(std::uint32_t{1} << octave)));
        if (next.sample_rate == 0) {
            result.error = "mip sample rate rounds to zero";
            break;
        }
        next.channels.resize(previous.channels.size());
        for (std::size_t channel = 0; channel < previous.channels.size(); ++channel) {
            next.channels[channel].resize(static_cast<std::size_t>(frames));
            decimate_sample_mip_2x(previous.channels[channel].data(), previous.num_frames(),
                                   next.channels[channel].data(), frames, coefficients);
        }
        const auto temporary_payload =
            temporary.path / ("level-" + std::to_string(octave) + ".wav");
        if (!write_wav_file(temporary_payload.string(), next, WavBitDepth::Float32)) {
            result.error = "failed to write a temporary mip payload";
            break;
        }
        if (!temporary.valid()) {
            result.error = "private mip staging directory changed identity";
            break;
        }
        if (!sync_file_for_publication(temporary_payload)) {
            result.error = "failed to durably write a temporary mip payload";
            break;
        }
        std::error_code payload_size_error;
        const auto temporary_payload_bytes =
            std::filesystem::file_size(temporary_payload, payload_size_error);
        if (payload_size_error || !bounded_payload_file(temporary_payload, temporary_payload_bytes,
                                                        frames, source.channels)) {
            result.error = "temporary mip payload exceeded its physical bound";
            break;
        }
        if (temporary_payload_bytes > options.maximum_output_bytes - output_bytes) {
            result.error = "mip payloads exceed the configured byte limit";
            break;
        }
        output_bytes += temporary_payload_bytes;
        auto payload = make_memory_mapped_frame_reader(temporary_payload.string(), true, true,
                                                       temporary_payload_bytes);
        if (!payload.valid || !payload.has_content_identity) {
            result.error = "failed to verify a temporary mip payload";
            break;
        }
        const auto final_path = sample_mip_payload_path_for_namespace(
            source_path, manifest_namespace, source.content_sha256, payload.content_sha256, octave);
        const auto payload_bytes = payload.mapped_byte_size;
        const auto payload_sha256 = payload.content_sha256;
        payload = {};
        auto policy_target =
            source_identity->prepare_access_policy_target(temporary_payload.string());
        if (!policy_target || !policy_target.sync()) {
            result.error = "failed to apply the source access policy to a mip payload";
            break;
        }
        if (!source_unchanged()) {
            result.error = "source pathname changed during mip construction";
            break;
        }
        if (!temporary.valid()) {
            result.error = "private mip staging directory changed identity";
            break;
        }
        published_payloads.prepare(final_path);
        const auto publication = publish_no_replace(temporary_payload, final_path);
        if (publication == PublishResult::AlreadyExists) {
            if (!bounded_payload_file(final_path, payload_bytes, frames, source.channels)) {
                published_payloads.discard_last();
                result.error = "hash-addressed mip payload conflicts with existing file";
                break;
            }
            std::shared_ptr<MemoryMappedAudioReader> retained_existing;
            auto existing = make_memory_mapped_frame_reader(final_path, true, true, payload_bytes,
                                                            &retained_existing);
            if (!existing.valid || existing.mapped_byte_size != payload_bytes ||
                existing.content_sha256 != payload_sha256) {
                published_payloads.discard_last();
                result.error = "hash-addressed mip payload conflicts with existing file";
                break;
            }
            retained_existing.reset();
            if (!replace_by_rename(temporary_payload, final_path)) {
                published_payloads.discard_last();
                result.error = "failed to replace the mip payload access policy";
                break;
            }
            published_payloads.retain_last_on_failure();
            if (!policy_target.finalize_after_move() || !policy_target.sync()) {
                result.error = "failed to finalize the mip payload access policy";
                break;
            }
        } else if (publication == PublishResult::Published) {
            if (test_fault == detail::SampleMipBuildFaultForTesting::PayloadPublicationException) {
                throw std::runtime_error("injected mip payload publication exception");
            }
            if (!policy_target.finalize_after_move() || !policy_target.sync()) {
                result.error = "failed to finalize the mip payload access policy";
                break;
            }
        } else {
            published_payloads.discard_last();
            result.error = "failed to publish a mip payload";
            break;
        }
        payload_paths.push_back(final_path);
        manifest_levels.push_back({.octave = octave,
                                   .decimation = std::uint32_t{1} << octave,
                                   .frames = frames,
                                   .rate_numerator = source.sample_rate,
                                   .rate_denominator = std::uint32_t{1} << octave,
                                   .payload_bytes = payload_bytes,
                                   .payload_sha256 = payload_sha256});
        previous = std::move(next);
    }

    if (!result.error.empty())
        return result;
    if (!sync_parent_directory(source_file)) {
        result.error = "failed to durably publish mip payloads";
        return result;
    }

    const auto manifest_file = std::filesystem::path(result.manifest_path);
    TemporaryDirectory manifest_temporary{
        unique_temporary_directory(canonical_parent_path(manifest_file))};
    if (!manifest_temporary.valid()) {
        result.error = "failed to create a private manifest staging directory";
        return result;
    }
    const auto manifest_temp = manifest_temporary.path / "manifest.pulpmip";
    if (!write_manifest(manifest_temp, source, opened_source_identity, manifest_namespace,
                        manifest_levels)) {
        result.error = "failed to finish the mip manifest";
        return result;
    }
    if (!manifest_temporary.valid()) {
        result.error = "private manifest staging directory changed identity";
        return result;
    }
    if (!sync_file_for_publication(manifest_temp)) {
        result.error = "failed to durably write the mip manifest";
        return result;
    }
    const auto staged = load_sample_mip_sidecar_from_manifest(source_path, manifest_temp, source,
                                                              opened_source_identity);
    if (staged.status != SampleMipSidecarStatus::Valid ||
        staged.level_count != options.level_count) {
        result.error = "staged mip manifest failed self-verification";
        return result;
    }
    auto manifest_policy = source_identity->prepare_access_policy_target(manifest_temp.string());
    if (!manifest_policy || !manifest_policy.sync()) {
        result.error = "failed to apply the source access policy to the mip manifest";
        return result;
    }
    runtime::InterProcessLock publication_lock(
        sample_mip_publication_lock_name(source_path, opened_source_identity));
    const auto publication_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!publication_lock.try_lock()) {
        if (std::chrono::steady_clock::now() >= publication_deadline) {
            result.error = "timed out waiting to publish the mip sidecar";
            return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto previous_manifest = manifest_temporary.path / "previous.pulpmip";
    runtime::AccessPolicyTarget previous_policy;
    bool previous_saved = false;
    if (regular_file(result.manifest_path)) {
        runtime::MemoryMappedFile opened_previous;
        if (!opened_previous.open_no_follow(result.manifest_path, runtime::MapMode::ReadOnly,
                                            static_cast<std::size_t>(kMaximumManifestBytes))) {
            result.error = "failed to preserve the previous mip manifest";
            return result;
        }
        const auto previous_identity = opened_previous.opened_file_identity();
        std::vector<std::uint8_t> previous_bytes(opened_previous.data(),
                                                 opened_previous.data() + opened_previous.size());
        if (!previous_identity.valid ||
            opened_previous.opened_file_identity() != previous_identity ||
            !opened_previous.path_refers_to_open_file(result.manifest_path)) {
            result.error = "previous mip manifest changed while preserving it";
            return result;
        }
        std::ofstream previous_output(previous_manifest, std::ios::binary | std::ios::trunc);
        previous_output.write(reinterpret_cast<const char*>(previous_bytes.data()),
                              static_cast<std::streamsize>(previous_bytes.size()));
        previous_output.flush();
        if (!previous_output.good()) {
            result.error = "failed to preserve the previous mip manifest";
            return result;
        }
        previous_output.close();
        if (previous_output.fail()) {
            result.error = "failed to preserve the previous mip manifest";
            return result;
        }
        previous_policy = source_identity->prepare_access_policy_target(previous_manifest.string());
        if (!previous_policy || !previous_policy.sync()) {
            result.error = "failed to preserve the previous manifest policy";
            return result;
        }
        previous_saved = true;
    }
    const auto rollback_manifest = [&]() {
        if (previous_saved)
            return replace_by_rename(previous_manifest, manifest_file) &&
                   previous_policy.finalize_after_move() && previous_policy.sync() &&
                   sync_parent_directory(manifest_file);
        std::error_code remove_error;
        const bool removed = std::filesystem::remove(manifest_file, remove_error);
        std::error_code exists_error;
        const bool still_exists = std::filesystem::exists(manifest_file, exists_error);
        return !remove_error && !exists_error && (removed || !still_exists) &&
               sync_parent_directory(manifest_file);
    };
    if (!source_unchanged()) {
        result.error = "source pathname changed before mip publication";
        return result;
    }
    if (!manifest_temporary.valid()) {
        result.error = "private manifest staging directory changed identity";
        return result;
    }
    if (!replace_by_rename(manifest_temp, manifest_file)) {
        std::error_code error;
        std::filesystem::remove(manifest_temp, error);
        result.error = "failed to publish the mip manifest";
        return result;
    }
    auto manifest_rollback_guard = runtime::make_scope_guard([&] {
        if (!rollback_manifest())
            published_payloads.committed = true;
    });
    if (!sync_parent_directory(manifest_file)) {
        const bool rolled_back = rollback_manifest();
        manifest_rollback_guard.dismiss();
        if (!rolled_back)
            published_payloads.committed = true;
        result.error = "failed to durably publish the mip manifest";
        if (!rolled_back)
            result.error += " and failed to restore the previous manifest";
        return result;
    }
    if (test_fault == detail::SampleMipBuildFaultForTesting::ManifestPolicyFinalization ||
        !manifest_policy.finalize_after_move() || !manifest_policy.sync()) {
        const bool rolled_back = rollback_manifest();
        manifest_rollback_guard.dismiss();
        if (!rolled_back)
            published_payloads.committed = true;
        result.error = "failed to finalize the mip manifest access policy";
        if (!rolled_back)
            result.error += " and failed to restore the previous manifest";
        return result;
    }
    if (test_fault ==
            detail::SampleMipBuildFaultForTesting::SourceChangedAfterManifestPublication ||
        !source_unchanged()) {
        const bool rolled_back = rollback_manifest();
        manifest_rollback_guard.dismiss();
        if (!rolled_back)
            published_payloads.committed = true;
        result.error = "source pathname changed during mip publication";
        if (!rolled_back)
            result.error += " and failed to restore the previous manifest";
        return result;
    }
    if (test_fault ==
        detail::SampleMipBuildFaultForTesting::PublishedManifestVerificationException) {
        throw std::runtime_error("injected mip verification exception");
    }
    auto verified =
        test_fault == detail::SampleMipBuildFaultForTesting::PublishedManifestVerification
            ? SampleMipSidecar{.status = SampleMipSidecarStatus::Invalid}
            : load_sample_mip_sidecar_from_manifest(source_path, result.manifest_path, source,
                                                    opened_source_identity);
    if (verified.status != SampleMipSidecarStatus::Valid ||
        verified.level_count != options.level_count) {
        const bool rolled_back = rollback_manifest();
        manifest_rollback_guard.dismiss();
        if (!rolled_back)
            published_payloads.committed = true;
        result.error = "published mip sidecar failed self-verification";
        if (!rolled_back)
            result.error += " and failed to restore the previous manifest";
        return result;
    }
    manifest_rollback_guard.dismiss();
    published_payloads.committed = true;
    try {
        if (test_fault ==
            detail::SampleMipBuildFaultForTesting::PostCommitGarbageCollectionException) {
            throw std::runtime_error("injected post-commit garbage-collection exception");
        }
        std::unordered_set<std::string> keep;
        for (const auto& payload : payload_paths)
            keep.insert(std::filesystem::path(payload).filename().string());
        const auto prefix =
            manifest_namespace == default_manifest_namespace(source_path)
                ? sample_mip_namespace_prefix(manifest_namespace)
                : sample_mip_payload_prefix(manifest_namespace, source.content_sha256);
        std::error_code scan_error;
        const auto parent = source_file.parent_path().empty() ? std::filesystem::path{"."}
                                                              : source_file.parent_path();
        for (std::filesystem::directory_iterator iterator(parent, scan_error), end;
             !scan_error && iterator != end; iterator.increment(scan_error)) {
            const auto& candidate = iterator->path();
            const auto name = candidate.filename().string();
            if (!name.starts_with(prefix) || candidate.extension() != ".wav" || keep.contains(name))
                continue;
            std::error_code status_error;
            if (!std::filesystem::is_regular_file(
                    std::filesystem::symlink_status(candidate, status_error)) ||
                status_error)
                continue;
            std::error_code remove_error;
            std::filesystem::remove(candidate, remove_error);
        }
    } catch (...) {
        // Publication is already committed. Garbage collection is retried by later builds.
    }
    result.payload_paths = std::move(payload_paths);
    result.ok = true;
    return result;
}

} // namespace pulp::audio
