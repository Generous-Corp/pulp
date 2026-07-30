#include <pulp/tools/timeline/agent.hpp>

#include "timeline_agent_internal.hpp"

#include <pulp/dawproject/dawproject_export.hpp>
#include <pulp/interchange/export_plan.hpp>
#include <pulp/runtime/crypto.hpp>
#include <pulp/runtime/url.hpp>
#include <pulp/smf/interchange.hpp>
#include <pulp/timeline/asset_path.hpp>
#include <pulp/timeline/dawproject_import.hpp>
#include <pulp/timeline/schema_json.hpp>
#include <pulp/timeline/serialize.hpp>
#include <pulp/timeline/smf.hpp>

#include <miniz.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#elif !defined(__APPLE__)
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace pulp::tools::timeline {
namespace {

namespace fs = std::filesystem;
using pulp::interchange::Concept;
using pulp::interchange::Format;

std::optional<Format> parse_format(std::string_view value) noexcept {
    if (value == "smf")
        return Format::Smf;
    if (value == "dawproject")
        return Format::DawProject;
    return std::nullopt;
}

std::optional<std::vector<std::uint8_t>> read_file_bounded(const fs::path& path,
                                                           std::uint64_t maximum_bytes) {
    std::error_code error;
    const auto size = fs::file_size(path, error);
    if (error || size > maximum_bytes || size > std::numeric_limits<std::size_t>::max() ||
        size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max()))
        return std::nullopt;
    try {
        std::ifstream stream(path, std::ios::binary);
        if (!stream)
            return std::nullopt;
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        if (!bytes.empty() && !stream.read(reinterpret_cast<char*>(bytes.data()),
                                           static_cast<std::streamsize>(bytes.size())))
            return std::nullopt;
        if (stream.peek() != std::ifstream::traits_type::eof())
            return std::nullopt;
        return bytes;
    } catch (...) {
        return std::nullopt;
    }
}

bool publish_path_no_replace(const fs::path& source, const fs::path& destination) noexcept {
#ifdef _WIN32
    return ::MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH) != 0;
#elif defined(__APPLE__)
    return ::renamex_np(source.c_str(), destination.c_str(), RENAME_EXCL) == 0;
#elif defined(__linux__)
    return ::syscall(SYS_renameat2, AT_FDCWD, source.c_str(), AT_FDCWD, destination.c_str(),
                     RENAME_NOREPLACE) == 0;
#else
    // std::filesystem has no atomic no-replace directory rename. An
    // exists-then-rename fallback races another publisher and can overwrite
    // its result, so unsupported platforms fail closed.
    (void)source;
    (void)destination;
    return false;
#endif
}

class DirectoryPublisher {
  public:
    static std::optional<DirectoryPublisher> create(const fs::path& destination) {
        if (destination.empty())
            return std::nullopt;
        std::error_code error;
        const auto status = fs::symlink_status(destination, error);
        if ((!error && status.type() != fs::file_type::not_found) ||
            (error && error != std::errc::no_such_file_or_directory))
            return std::nullopt;
        error.clear();
        fs::path parent = destination.parent_path();
        if (parent.empty())
            parent = fs::current_path(error);
        if (error || !fs::is_directory(parent, error) || error)
            return std::nullopt;

        static std::atomic<std::uint64_t> counter{0};
        const auto seed =
            static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        for (std::uint64_t attempt = 0; attempt < 64; ++attempt) {
            const auto suffix = seed ^ (++counter) ^ attempt;
            fs::path staging = parent / ("." + destination.filename().string() + ".pulp-staging-" +
                                         std::to_string(suffix));
            if (fs::create_directory(staging, error))
                return DirectoryPublisher(destination, std::move(staging));
            if (error != std::errc::file_exists)
                return std::nullopt;
            error.clear();
        }
        return std::nullopt;
    }

    DirectoryPublisher(DirectoryPublisher&& other) noexcept
        : destination_(std::move(other.destination_)), staging_(std::move(other.staging_)),
          committed_(other.committed_) {
        other.committed_ = true;
    }
    DirectoryPublisher& operator=(DirectoryPublisher&&) = delete;
    DirectoryPublisher(const DirectoryPublisher&) = delete;
    DirectoryPublisher& operator=(const DirectoryPublisher&) = delete;

    ~DirectoryPublisher() {
        if (!committed_) {
            std::error_code ignored;
            fs::remove_all(staging_, ignored);
        }
    }

    bool write(std::string_view relative_utf8, std::span<const std::uint8_t> bytes) {
        if (!pulp::timeline::package_relative_path_is_lexically_safe(relative_utf8))
            return false;
        fs::path relative;
        try {
            relative = filesystem_path_from_utf8(relative_utf8);
        } catch (...) {
            return false;
        }
        if (relative.empty() || relative.is_absolute() || relative.has_root_name() ||
            relative.has_root_directory())
            return false;
        const fs::path output = staging_ / relative;
        std::error_code error;
        if (fs::exists(output, error) || error)
            return false;
        if (!fs::create_directories(output.parent_path(), error) && error)
            return false;
        try {
            std::ofstream stream(output, std::ios::binary | std::ios::trunc);
            if (!stream)
                return false;
            if (!bytes.empty())
                stream.write(reinterpret_cast<const char*>(bytes.data()),
                             static_cast<std::streamsize>(bytes.size()));
            stream.flush();
            return static_cast<bool>(stream);
        } catch (...) {
            return false;
        }
    }

    bool write(std::string_view relative_utf8, std::string_view text) {
        return write(relative_utf8,
                     std::span<const std::uint8_t>(
                         reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
    }

    bool commit() noexcept {
        if (!publish_path_no_replace(staging_, destination_))
            return false;
        committed_ = true;
        return true;
    }

  private:
    DirectoryPublisher(fs::path destination, fs::path staging)
        : destination_(std::move(destination)), staging_(std::move(staging)) {}

    fs::path destination_;
    fs::path staging_;
    bool committed_ = false;
};

class WorkingSetBudget {
  public:
    explicit WorkingSetBudget(std::uint64_t limit) : limit_(limit) {}

    bool acquire(std::uint64_t bytes) noexcept {
        if (bytes > limit_ || current_ > limit_ - bytes) {
            exceeded_ = true;
            return false;
        }
        current_ += bytes;
        peak_ = std::max(peak_, current_);
        return true;
    }
    void release(std::uint64_t bytes) noexcept {
        if (bytes > current_) std::abort();
        current_ -= bytes;
    }
    std::uint64_t peak() const noexcept { return peak_; }
    bool exceeded() const noexcept { return exceeded_; }

  private:
    std::uint64_t limit_ = 0;
    std::uint64_t current_ = 0;
    std::uint64_t peak_ = 0;
    bool exceeded_ = false;
};

struct alignas(std::max_align_t) TrackedAllocationHeader {
    std::size_t requested = 0;
};

std::optional<std::uint64_t> allocation_charge(std::size_t requested) noexcept {
    if (requested > std::numeric_limits<std::size_t>::max() - sizeof(TrackedAllocationHeader))
        return std::nullopt;
    return static_cast<std::uint64_t>(requested + sizeof(TrackedAllocationHeader));
}

void* tracked_miniz_alloc(void* opaque, std::size_t items, std::size_t size) {
    if (items != 0 && size > std::numeric_limits<std::size_t>::max() / items) return nullptr;
    const auto requested = items * size;
    const auto charge = allocation_charge(requested);
    auto& budget = *static_cast<WorkingSetBudget*>(opaque);
    if (!charge || !budget.acquire(*charge)) return nullptr;
    auto* header = static_cast<TrackedAllocationHeader*>(std::malloc(*charge));
    if (header == nullptr) {
        budget.release(*charge);
        return nullptr;
    }
    header->requested = requested;
    return header + 1;
}

void tracked_miniz_free(void* opaque, void* address) {
    if (address == nullptr) return;
    auto* header = static_cast<TrackedAllocationHeader*>(address) - 1;
    const auto charge = allocation_charge(header->requested).value_or(0);
    std::free(header);
    static_cast<WorkingSetBudget*>(opaque)->release(charge);
}

void* tracked_miniz_realloc(void* opaque, void* address, std::size_t items, std::size_t size) {
    if (address == nullptr) return tracked_miniz_alloc(opaque, items, size);
    if (items == 0 || size == 0) {
        tracked_miniz_free(opaque, address);
        return nullptr;
    }
    if (size > std::numeric_limits<std::size_t>::max() / items) return nullptr;
    auto* old_header = static_cast<TrackedAllocationHeader*>(address) - 1;
    const auto old_charge = allocation_charge(old_header->requested).value_or(0);
    const auto requested = items * size;
    const auto new_charge = allocation_charge(requested);
    if (!new_charge) return nullptr;
    auto& budget = *static_cast<WorkingSetBudget*>(opaque);
    if (*new_charge > old_charge && !budget.acquire(*new_charge - old_charge)) return nullptr;
    auto* new_header = static_cast<TrackedAllocationHeader*>(std::realloc(old_header, *new_charge));
    if (new_header == nullptr) {
        if (*new_charge > old_charge) budget.release(*new_charge - old_charge);
        return nullptr;
    }
    if (*new_charge < old_charge) budget.release(old_charge - *new_charge);
    new_header->requested = requested;
    return new_header + 1;
}

void install_tracked_allocator(mz_zip_archive& zip, WorkingSetBudget& budget) noexcept {
    zip.m_pAlloc = tracked_miniz_alloc;
    zip.m_pFree = tracked_miniz_free;
    zip.m_pRealloc = tracked_miniz_realloc;
    zip.m_pAlloc_opaque = &budget;
}

MZ_FILE* open_zip_file(const fs::path& path, bool write) noexcept {
#ifdef _WIN32
    return ::_wfopen(path.c_str(), write ? L"w+b" : L"rb");
#else
    return std::fopen(path.c_str(), write ? "w+b" : "rb");
#endif
}

constexpr std::uint64_t kFileIoMetadataCharge = 64u * 1024u;
constexpr std::uint64_t kMapNodeMetadataCharge =
    sizeof(std::pair<const std::string, std::vector<std::uint8_t>>) + 4u * sizeof(void*);
using ZipEntries =
    std::map<std::string, std::vector<std::uint8_t>, std::less<>>;

std::optional<std::uint64_t>
retained_artifact_charge(const pulp::interchange::ExportArtifacts& artifacts) noexcept {
    if (artifacts.artifacts.capacity() >
        UINT64_MAX / sizeof(pulp::interchange::ExportArtifact))
        return std::nullopt;
    std::uint64_t total = static_cast<std::uint64_t>(artifacts.artifacts.capacity()) *
                          sizeof(pulp::interchange::ExportArtifact);
    for (const auto& artifact : artifacts.artifacts) {
        const auto name = static_cast<std::uint64_t>(artifact.name.capacity()) + 1u;
        const auto bytes = static_cast<std::uint64_t>(artifact.bytes.capacity());
        if (name > UINT64_MAX - total || bytes > UINT64_MAX - total - name)
            return std::nullopt;
        total += name + bytes;
    }
    return total;
}

runtime::Result<ZipEntries, std::string>
read_zip_archive(const fs::path& input, WorkingSetBudget& budget,
                 std::size_t max_file_entries, std::size_t max_archive_entries,
                 std::size_t max_path_bytes) {
    mz_zip_archive zip{};
    install_tracked_allocator(zip, budget);
    if (!budget.acquire(kFileIoMetadataCharge))
        return runtime::Result<ZipEntries, std::string>(
            runtime::Err(std::string("DAWproject ZIP exceeds the working-set limit")));
    auto* file = open_zip_file(input, false);
    if (file == nullptr || !mz_zip_reader_init_cfile(&zip, file, 0, 0)) {
        if (zip.m_pState != nullptr) mz_zip_reader_end(&zip);
        if (file != nullptr) std::fclose(file);
        budget.release(kFileIoMetadataCharge);
        return runtime::Result<ZipEntries, std::string>(runtime::Err(
            std::string(budget.exceeded()
                            ? "DAWproject ZIP exceeds the working-set limit"
                            : "input is not a readable DAWproject ZIP container")));
    }

    ZipEntries entries;
    const auto count = mz_zip_reader_get_num_files(&zip);
    bool ok = count <= max_archive_entries;
    std::string message = ok ? std::string{} : "DAWproject ZIP has too many entries";
    std::size_t file_entries = 0;
    for (mz_uint index = 0; ok && index < count; ++index) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, index, &stat)) {
            message = "could not inspect a DAWproject ZIP entry";
            ok = false;
            break;
        }
        const auto name_size = mz_zip_reader_get_filename(&zip, index, nullptr, 0);
        if (name_size == 0 || name_size - 1 > max_path_bytes ||
            !budget.acquire(name_size)) {
            message = "DAWproject ZIP exceeds the working-set limit";
            ok = false;
            break;
        }
        std::vector<char> name_buffer(name_size);
        if (name_buffer.capacity() > name_size &&
            !budget.acquire(name_buffer.capacity() - name_size)) {
            message = "DAWproject ZIP exceeds the working-set limit";
            ok = false;
            break;
        }
        const auto name_buffer_charge = name_buffer.capacity();
        if (
            mz_zip_reader_get_filename(&zip, index, name_buffer.data(), name_buffer.size()) !=
                name_size ||
            name_buffer.back() != '\0' || stat.m_is_encrypted) {
            message = "DAWproject ZIP contains an unsupported or unsafe entry";
            ok = false;
            break;
        }
        const auto anticipated_name_charge =
            2u * static_cast<std::uint64_t>(name_size) + sizeof(std::string);
        if (!budget.acquire(anticipated_name_charge)) {
            message = "DAWproject ZIP exceeds the working-set limit";
            ok = false;
            break;
        }
        std::string name(name_buffer.data(), name_size - 1);
        const auto name_charge = static_cast<std::uint64_t>(name.capacity()) + 1u;
        budget.release(anticipated_name_charge - name_charge);
        budget.release(name_buffer_charge);
        if (stat.m_is_directory && !name.empty() && name.back() == '/') name.pop_back();
        if (name.empty() || !pulp::timeline::package_relative_path_is_lexically_safe(name)) {
            message = "DAWproject ZIP contains an unsupported or unsafe entry";
            ok = false;
            break;
        }
        const auto unix_mode = static_cast<unsigned>((stat.m_external_attr >> 16) & 0xffffu);
        if ((unix_mode & 0170000u) == 0120000u) {
            message = "DAWproject ZIP must not contain symbolic links";
            ok = false;
            break;
        }
        if (stat.m_is_directory) {
            budget.release(name_charge);
            continue;
        }
        if (++file_entries > max_file_entries) {
            message = "DAWproject ZIP has too many files";
            ok = false;
            break;
        }
        if (stat.m_uncomp_size > std::numeric_limits<std::size_t>::max() ||
            stat.m_uncomp_size > UINT64_MAX - kMapNodeMetadataCharge ||
            !budget.acquire(kMapNodeMetadataCharge + stat.m_uncomp_size)) {
            message = "DAWproject ZIP exceeds the working-set limit";
            ok = false;
            break;
        }
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(stat.m_uncomp_size));
        if (bytes.capacity() > stat.m_uncomp_size &&
            !budget.acquire(bytes.capacity() - stat.m_uncomp_size)) {
            message = "DAWproject ZIP exceeds the working-set limit";
            ok = false;
            break;
        }
        if (!mz_zip_reader_extract_to_mem(&zip, index, bytes.data(), bytes.size(), 0) ||
            !entries.emplace(std::move(name), std::move(bytes)).second) {
            message = budget.exceeded()
                          ? "DAWproject ZIP exceeds the working-set limit"
                          : "DAWproject ZIP contains a duplicate or unreadable entry: " + name;
            ok = false;
            break;
        }
    }
    mz_zip_reader_end(&zip);
    std::fclose(file);
    budget.release(kFileIoMetadataCharge);
    if (!ok)
        return runtime::Result<ZipEntries, std::string>(
            runtime::Err(std::move(message)));
    return runtime::Result<ZipEntries, std::string>(
        runtime::Ok(std::move(entries)));
}

std::string output_json(std::string_view format, const fs::path& output,
                        std::string_view extra = {}) {
    std::string result =
        "{\"format\":" + pulp::timeline::quote_json_string(format) + ",\"ok\":true,\"output\":";
    result += pulp::timeline::quote_json_string(filesystem_path_to_utf8(output));
    if (!extra.empty()) {
        result += ',';
        result += extra;
    }
    result += '}';
    return result;
}

std::string concept_array_json(std::span<const Concept> concepts) {
    std::string result = "[";
    bool first = true;
    for (const auto concept_value : concepts) {
        if (!first)
            result += ',';
        first = false;
        result += pulp::timeline::quote_json_string(
            pulp::interchange::concept_id(concept_value));
    }
    result += ']';
    return result;
}

std::string export_result_json(std::string_view format, const fs::path& output,
                               const pulp::interchange::ExportPlan& plan) {
    std::string result =
        "{\"format\":" + pulp::timeline::quote_json_string(format) +
        ",\"lossless\":" + (plan.is_lossless() ? std::string("true") : std::string("false")) +
        ",\"manifest\":" + pulp::interchange::loss_manifest_json(plan) + ",\"ok\":true";
    result += ",\"output\":";
    result += pulp::timeline::quote_json_string(filesystem_path_to_utf8(output));
    result += ",\"plan_only\":false,\"required_consent\":[]";
    result += '}';
    return result;
}

std::string export_plan_json(std::string_view format,
                             const pulp::interchange::ExportPlan& plan) {
    const auto required = plan.required_consent();
    std::string result =
        "{\"format\":" + pulp::timeline::quote_json_string(format) +
        ",\"lossless\":" + (plan.is_lossless() ? std::string("true") : std::string("false")) +
        ",\"manifest\":" + pulp::interchange::loss_manifest_json(plan) +
        ",\"ok\":true,\"plan_only\":true,\"required_consent\":";
    result += concept_array_json(required);
    result += '}';
    return result;
}

OperationResult export_error_result(std::string_view format,
                                    const pulp::interchange::ExportPlan& plan,
                                    std::string_view stage, std::string_view message,
                                    std::string_view path = {},
                                    std::span<const Concept> required_consent = {}) {
    std::string result = "{\"error\":{\"message\":";
    result += pulp::timeline::quote_json_string(message);
    if (!path.empty()) {
        result += ",\"path\":";
        result += pulp::timeline::quote_json_string(path);
    }
    result += ",\"stage\":";
    result += pulp::timeline::quote_json_string(stage);
    result += "},\"format\":";
    result += pulp::timeline::quote_json_string(format);
    result += ",\"manifest\":";
    result += pulp::interchange::loss_manifest_json(plan);
    result += ",\"ok\":false,\"required_consent\":";
    result += concept_array_json(required_consent);
    result += '}';
    return {1, std::move(result)};
}

runtime::Result<std::size_t, detail::DawProjectMediaError>
add_dawproject_media_impl(const detail::LoadedProject& loaded,
                          pulp::interchange::ExportArtifacts& artifacts,
                          std::uint64_t max_total_media_bytes) {
    const auto fail = [](detail::DawProjectMediaErrorCode code, std::uint64_t asset_id,
                         std::string asset_name, std::string reason) {
        return runtime::Result<std::size_t, detail::DawProjectMediaError>(runtime::Err(
            detail::DawProjectMediaError{code, asset_id, std::move(asset_name),
                                         std::move(reason)}));
    };
    const auto* root = loaded.value.find_sequence(loaded.value.root_sequence_id());
    if (root == nullptr)
        return fail(detail::DawProjectMediaErrorCode::MissingRootSequence, 0, {},
                    "project root sequence is missing");
    std::size_t candidate_count = 0;
    for (const auto& track : root->tracks()) {
        if (track.clips().size() > std::numeric_limits<std::size_t>::max() - candidate_count)
            return fail(detail::DawProjectMediaErrorCode::ByteLimitExceeded, 0, {},
                        "media artifact metadata exceeds the working-set limit");
        candidate_count += track.clips().size();
    }
    const auto fixed_metadata = static_cast<std::uint64_t>(candidate_count) *
        (sizeof(pulp::interchange::ExportArtifact) + sizeof(std::uint64_t));
    if (candidate_count != 0 &&
        fixed_metadata / candidate_count !=
            sizeof(pulp::interchange::ExportArtifact) + sizeof(std::uint64_t))
        return fail(detail::DawProjectMediaErrorCode::ByteLimitExceeded, 0, {},
                    "media artifact metadata exceeds the working-set limit");
    if (fixed_metadata > max_total_media_bytes)
        return fail(detail::DawProjectMediaErrorCode::ByteLimitExceeded, 0, {},
                    "media artifact metadata exceeds the working-set limit");
    std::uint64_t retained_media_bytes = fixed_metadata;
    std::vector<pulp::interchange::ExportArtifact> media_artifacts;
    std::vector<std::uint64_t> seen_assets;
    media_artifacts.reserve(candidate_count);
    seen_assets.reserve(candidate_count);
    for (const auto& track : root->tracks()) {
        for (const auto& clip : track.clips()) {
            if (clip.time_anchor() != pulp::timeline::ClipTimeAnchor::Musical)
                continue;
            const auto* media = std::get_if<pulp::timeline::MediaRef>(&clip.content());
            if (media == nullptr ||
                std::find(seen_assets.begin(), seen_assets.end(), media->asset_id.value) !=
                    seen_assets.end())
                continue;
            seen_assets.push_back(media->asset_id.value);
            const auto* asset = loaded.value.find_asset(media->asset_id);
            if (asset == nullptr)
                return fail(detail::DawProjectMediaErrorCode::MissingAsset,
                            media->asset_id.value, {}, "referenced asset is missing");
            if (asset->name.empty())
                return fail(detail::DawProjectMediaErrorCode::InvalidAssetName,
                            media->asset_id.value, {}, "asset name is empty");
            if (!pulp::timeline::package_relative_path_is_lexically_safe(asset->name))
                return fail(detail::DawProjectMediaErrorCode::InvalidAssetName,
                            media->asset_id.value, asset->name,
                            "asset name is not a safe package-relative path");
            if (asset->name.size() > (UINT64_MAX - sizeof(std::string) - 14u) / 2u)
                return fail(detail::DawProjectMediaErrorCode::ByteLimitExceeded,
                            media->asset_id.value, asset->name,
                            "media artifact name exceeds the working-set limit");
            const auto anticipated_name_charge =
                2u * (static_cast<std::uint64_t>(asset->name.size()) + 7u) +
                sizeof(std::string);
            if (anticipated_name_charge > max_total_media_bytes - retained_media_bytes)
                return fail(detail::DawProjectMediaErrorCode::ByteLimitExceeded,
                            media->asset_id.value, asset->name,
                            "media artifact name exceeds the working-set limit");
            retained_media_bytes += anticipated_name_charge;
            std::string artifact_name = "audio/" + asset->name;
            const auto name_charge = static_cast<std::uint64_t>(artifact_name.capacity()) + 1u;
            retained_media_bytes -= anticipated_name_charge - name_charge;
            const auto duplicate =
                std::any_of(artifacts.artifacts.begin(), artifacts.artifacts.end(),
                            [&](const auto& artifact) { return artifact.name == artifact_name; }) ||
                std::any_of(media_artifacts.begin(), media_artifacts.end(),
                            [&](const auto& artifact) { return artifact.name == artifact_name; });
            if (duplicate)
                return fail(detail::DawProjectMediaErrorCode::DuplicateArchivePath,
                            media->asset_id.value, asset->name,
                            "archive path collides with another export artifact");
            if (retained_media_bytes > max_total_media_bytes)
                return fail(detail::DawProjectMediaErrorCode::ByteLimitExceeded,
                            media->asset_id.value, asset->name,
                            "cumulative media byte limit is already exhausted");
            auto bytes = detail::read_verified_asset_bytes(
                loaded, *asset, max_total_media_bytes - retained_media_bytes);
            if (!bytes)
                return fail(detail::DawProjectMediaErrorCode::AssetReadFailed,
                            media->asset_id.value, asset->name,
                            "asset bytes are unavailable, invalid, or exceed the remaining limit");
            const auto artifact_charge = static_cast<std::uint64_t>(bytes->capacity());
            if (artifact_charge > max_total_media_bytes - retained_media_bytes)
                return fail(detail::DawProjectMediaErrorCode::ByteLimitExceeded,
                            media->asset_id.value, asset->name,
                            "cumulative media working-set limit is exhausted");
            retained_media_bytes += artifact_charge;
            media_artifacts.push_back({std::move(artifact_name), std::move(*bytes)});
        }
    }
    if (media_artifacts.size() >
        std::numeric_limits<std::size_t>::max() - artifacts.artifacts.size())
        return fail(detail::DawProjectMediaErrorCode::ByteLimitExceeded, 0, {},
                    "combined artifact metadata exceeds the working-set limit");
    const auto final_count = artifacts.artifacts.size() + media_artifacts.size();
    if (final_count > UINT64_MAX / (2u * sizeof(pulp::interchange::ExportArtifact)))
        return fail(detail::DawProjectMediaErrorCode::ByteLimitExceeded, 0, {},
                    "combined artifact metadata exceeds the working-set limit");
    const auto replacement_metadata = static_cast<std::uint64_t>(final_count) *
                                      2u * sizeof(pulp::interchange::ExportArtifact);
    if (replacement_metadata > max_total_media_bytes - retained_media_bytes)
        return fail(detail::DawProjectMediaErrorCode::ByteLimitExceeded, 0, {},
                    "combined artifact metadata exceeds the working-set limit");
    pulp::interchange::ExportArtifacts combined;
    combined.artifacts.reserve(final_count);
    combined.artifacts.insert(combined.artifacts.end(),
                              std::make_move_iterator(artifacts.artifacts.begin()),
                              std::make_move_iterator(artifacts.artifacts.end()));
    combined.artifacts.insert(combined.artifacts.end(),
                              std::make_move_iterator(media_artifacts.begin()),
                              std::make_move_iterator(media_artifacts.end()));
    artifacts = std::move(combined);
    return runtime::Result<std::size_t, detail::DawProjectMediaError>(
        runtime::Ok(media_artifacts.size()));
}

} // namespace

namespace detail {

runtime::Result<std::size_t, DawProjectMediaError>
add_dawproject_media(const LoadedProject& loaded,
                     pulp::interchange::ExportArtifacts& artifacts,
                     std::uint64_t max_total_media_bytes) {
    return add_dawproject_media_impl(loaded, artifacts, max_total_media_bytes);
}

runtime::Result<std::uint64_t, DawProjectArchiveError> write_dawproject_archive_no_replace(
    const pulp::interchange::ExportArtifacts& artifacts, const fs::path& destination,
    std::uint64_t max_working_set_bytes) {
    WorkingSetBudget budget(max_working_set_bytes);
    const auto artifact_charge = retained_artifact_charge(artifacts);
    if (!artifact_charge || !budget.acquire(*artifact_charge) ||
        !budget.acquire(kFileIoMetadataCharge))
        return runtime::Result<std::uint64_t, DawProjectArchiveError>(runtime::Err(
            DawProjectArchiveError{DawProjectArchiveErrorCode::Export,
                                   "DAWproject export exceeds the working-set limit"}));

    static std::atomic<std::uint64_t> counter{0};
    std::error_code error;
    for (std::uint64_t attempt = 0; attempt < 128; ++attempt) {
        const auto nonce = counter.fetch_add(1, std::memory_order_relaxed);
        const auto staging_directory = destination.parent_path() /
            fs::path(".pulp-dawproject-staging-" + std::to_string(nonce));
        if (!fs::create_directory(staging_directory, error)) {
            if (error == std::errc::file_exists) { error.clear(); continue; }
            break;
        }
        const auto staging = staging_directory / "archive.dawproject";
        mz_zip_archive zip{};
        install_tracked_allocator(zip, budget);
        auto* file = open_zip_file(staging, true);
        bool ok = file != nullptr && mz_zip_writer_init_cfile(&zip, file, 0) != 0;
        for (std::size_t index = 0; ok && index < artifacts.artifacts.size(); ++index) {
            const auto& artifact = artifacts.artifacts[index];
            bool duplicate = false;
            for (std::size_t prior = 0; prior < index; ++prior)
                duplicate = duplicate || artifacts.artifacts[prior].name == artifact.name;
            ok = !duplicate &&
                 pulp::timeline::package_relative_path_is_lexically_safe(artifact.name) &&
                 mz_zip_writer_add_mem(&zip, artifact.name.c_str(), artifact.bytes.data(),
                                       artifact.bytes.size(), MZ_DEFAULT_COMPRESSION) != 0;
        }
        if (ok) ok = mz_zip_writer_finalize_archive(&zip) != 0;
        if (zip.m_pState != nullptr) mz_zip_writer_end(&zip);
        if (file != nullptr && std::fclose(file) != 0) ok = false;
        if (ok && publish_path_no_replace(staging, destination)) {
            fs::remove(staging_directory, error);
            return runtime::Result<std::uint64_t, DawProjectArchiveError>(
                runtime::Ok(budget.peak()));
        }
        fs::remove_all(staging_directory, error);
        return runtime::Result<std::uint64_t, DawProjectArchiveError>(runtime::Err(
            DawProjectArchiveError{
                ok ? DawProjectArchiveErrorCode::Publish : DawProjectArchiveErrorCode::Export,
                ok ? "output file appeared before atomic publication"
                   : "could not build bounded DAWproject ZIP container"}));
    }
    return runtime::Result<std::uint64_t, DawProjectArchiveError>(runtime::Err(
        DawProjectArchiveError{DawProjectArchiveErrorCode::Publish,
                               "could not create DAWproject ZIP staging file"}));
}

runtime::Result<std::uint64_t, std::string> inspect_dawproject_archive(
    const fs::path& input, std::uint64_t max_working_set_bytes,
    std::size_t max_file_entries, std::size_t max_archive_entries,
    std::size_t max_path_bytes) {
    WorkingSetBudget budget(max_working_set_bytes);
    auto entries = read_zip_archive(input, budget, max_file_entries, max_archive_entries,
                                    max_path_bytes);
    if (!entries)
        return runtime::Result<std::uint64_t, std::string>(runtime::Err(entries.error()));
    return runtime::Result<std::uint64_t, std::string>(runtime::Ok(budget.peak()));
}

} // namespace detail

OperationResult plan_export_project(const ProjectSource& project, std::string_view format_text) {
    const auto format = parse_format(format_text);
    if (!format)
        return detail::failure("arguments", "format (smf or dawproject) is required", {}, 2);

    auto registry = pulp::timeline::make_builtin_timeline_registry();
    if (!registry)
        return detail::failure("registry", "could not construct the built-in schema registry");
    auto loaded = detail::load_project(project, registry.value());
    if (!loaded)
        return detail::failure("open", detail::persistence_message(loaded.error()),
                               loaded.error().path);
    const auto plan = pulp::interchange::plan_export(loaded.value().value, *format);
    return {0, export_plan_json(format_text, plan)};
}

OperationResult export_project(const ProjectSource& project, std::string_view format_text,
                               const fs::path& output_directory,
                               const std::vector<std::string>& accepted_loss_names) {
    const auto format = parse_format(format_text);
    if (!format || output_directory.empty())
        return detail::failure("arguments", "format (smf or dawproject) and output are required",
                               {}, 2);
    if (*format == Format::DawProject && output_directory.extension() != ".dawproject")
        return detail::failure("arguments", "DAWproject output must end in .dawproject",
                               filesystem_path_to_utf8(output_directory), 2);

    pulp::interchange::ExportOptions options;
    std::set<Concept> unique_losses;
    for (const auto& name : accepted_loss_names) {
        const auto concept_value = pulp::interchange::concept_from_id(name);
        if (concept_value == Concept::Unknown)
            return detail::failure("arguments", "unknown loss concept: " + name, {}, 2);
        if (unique_losses.insert(concept_value).second)
            options.accepted_losses.push_back(concept_value);
    }

    auto registry = pulp::timeline::make_builtin_timeline_registry();
    if (!registry)
        return detail::failure("registry", "could not construct the built-in schema registry");
    auto loaded = detail::load_project(project, registry.value());
    if (!loaded)
        return detail::failure("open", detail::persistence_message(loaded.error()),
                               loaded.error().path);
    const auto plan = pulp::interchange::plan_export(loaded.value().value, *format);
    auto exported = pulp::interchange::run_export(
        plan, options, *format == Format::Smf ? pulp::smf::writer() : pulp::dawproject::writer());
    if (!exported) {
        const auto path = exported.error().concepts.empty()
                              ? std::string_view{}
                              : pulp::interchange::concept_id(exported.error().concepts.front());
        return export_error_result(format_text, plan, "export", exported.error().message, path,
                                   exported.error().concepts);
    }
    if (*format == Format::DawProject) {
        const auto existing_charge = retained_artifact_charge(exported.value());
        if (!existing_charge ||
            *existing_charge > detail::kMaxAssetWorkingSetBytes - kFileIoMetadataCharge)
            return export_error_result(format_text, plan, "export",
                                       "DAWproject export exceeds the working-set limit");
        const auto media_budget = detail::kMaxAssetWorkingSetBytes -
                                  kFileIoMetadataCharge - *existing_charge;
        auto media_result = detail::add_dawproject_media(
            loaded.value(), exported.value(), media_budget);
        if (!media_result) {
            const auto& error = media_result.error();
            const auto asset = error.asset_name.empty()
                                   ? std::string("asset id ") + std::to_string(error.asset_id)
                                   : std::string("asset '") + error.asset_name + "'";
            return export_error_result(format_text, plan, "export",
                                       "could not add DAWproject media " + asset + ": " +
                                           error.reason,
                                       error.asset_name);
        }
        auto archive = detail::write_dawproject_archive_no_replace(
            exported.value(), output_directory, detail::kMaxAssetWorkingSetBytes);
        if (!archive)
            return export_error_result(
                format_text, plan,
                archive.error().code == detail::DawProjectArchiveErrorCode::Publish
                    ? "publish" : "export",
                archive.error().message,
                                       filesystem_path_to_utf8(output_directory));
        return {0, export_result_json(format_text, output_directory, plan)};
    }

    auto publisher = DirectoryPublisher::create(output_directory);
    if (!publisher)
        return export_error_result(format_text, plan, "publish",
                                   "output directory must not exist and its parent must exist",
                                   filesystem_path_to_utf8(output_directory));
    for (const auto& artifact : exported.value().artifacts)
        if (!publisher->write(artifact.name, artifact.bytes))
            return export_error_result(format_text, plan, "publish",
                                       "could not stage export artifact", artifact.name);
    if (!publisher->commit())
        return export_error_result(format_text, plan, "publish",
                                   "output directory appeared before atomic publication",
                                   filesystem_path_to_utf8(output_directory));
    return {0, export_result_json(format_text, output_directory, plan)};
}

OperationResult import_project(const fs::path& input, std::string_view format_text,
                               const fs::path& output_directory) {
    const auto format = parse_format(format_text);
    if (!format || input.empty() || output_directory.empty())
        return detail::failure("arguments",
                               "input, format (smf or dawproject), and output are required", {}, 2);

    std::optional<pulp::timeline::Project> imported;
    std::set<std::string> media_paths;
    ZipEntries archive_entries;
    std::string fidelity;
    if (*format == Format::Smf) {
        auto bytes = read_file_bounded(input, pulp::timeline::SmfImportLimits{}.max_file_bytes);
        if (!bytes)
            return detail::failure("import", "could not read bounded SMF input",
                                   filesystem_path_to_utf8(input));
        auto result = pulp::timeline::import_smf(*bytes);
        if (!result)
            return detail::failure("import", result.error().message);
        fidelity = "\"exact_tick_conversion\":" +
                   std::string(result.value().exact_tick_conversion ? "true" : "false") +
                   ",\"max_tick_rounding_error\":\"" +
                   std::to_string(result.value().max_tick_rounding_error) + "\"";
        imported.emplace(std::move(result).value().project);
    } else {
        if (input.extension() != ".dawproject")
            return detail::failure("arguments", "DAWproject input must end in .dawproject",
                                   filesystem_path_to_utf8(input), 2);
        const auto limits = pulp::timeline::DawProjectImportLimits{};
        if (limits.max_media_assets > std::numeric_limits<std::size_t>::max() - 2)
            return detail::failure("import", "DAWproject media asset limit is invalid");
        const auto max_files = limits.max_media_assets + 2; // project.xml + loss manifest
        if (max_files > std::numeric_limits<std::size_t>::max() / 2)
            return detail::failure("import", "DAWproject archive entry limit is invalid");
        const auto max_entries = max_files * 2; // permit one explicit directory per file
        WorkingSetBudget budget(detail::kMaxAssetWorkingSetBytes);
        auto read_entries = read_zip_archive(input, budget, max_files, max_entries,
                                             limits.max_package_path_bytes);
        if (!read_entries)
            return detail::failure("import", read_entries.error(), filesystem_path_to_utf8(input));
        archive_entries = std::move(read_entries).value();
        const auto xml_entry = archive_entries.find("project.xml");
        if (xml_entry == archive_entries.end() || xml_entry->second.size() > limits.max_xml_bytes)
            return detail::failure("import",
                                   "DAWproject ZIP must contain a bounded root project.xml entry",
                                   filesystem_path_to_utf8(input));
        std::uint64_t total_media = 0;
        std::uint64_t prior_resolver_copy_charge = 0;
        auto resolver = [&](std::string_view relative) -> std::optional<std::vector<std::uint8_t>> {
            budget.release(prior_resolver_copy_charge);
            prior_resolver_copy_charge = 0;
            const auto entry = archive_entries.find(relative);
            if (entry == archive_entries.end() ||
                entry->second.size() > limits.max_media_bytes_per_resolver_call ||
                total_media > limits.max_total_media_bytes ||
                entry->second.size() > limits.max_total_media_bytes - total_media)
                return std::nullopt;
            const auto copy_charge = static_cast<std::uint64_t>(entry->second.capacity());
            if (relative.size() > (UINT64_MAX - sizeof(std::string)) / 2u)
                return std::nullopt;
            const auto anticipated_path_charge =
                2u * static_cast<std::uint64_t>(relative.size()) + sizeof(std::string) +
                kMapNodeMetadataCharge;
            if (!budget.acquire(copy_charge) || !budget.acquire(anticipated_path_charge))
                return std::nullopt;
            std::string retained_path(relative);
            const auto path_charge = static_cast<std::uint64_t>(retained_path.capacity()) + 1u +
                                     kMapNodeMetadataCharge;
            budget.release(anticipated_path_charge - path_charge);
            prior_resolver_copy_charge = copy_charge;
            total_media += entry->second.size();
            if (!media_paths.insert(std::move(retained_path)).second)
                budget.release(path_charge);
            return entry->second;
        };
        auto result = pulp::timeline::import_dawproject_xml(
            std::string_view(reinterpret_cast<const char*>(xml_entry->second.data()),
                             xml_entry->second.size()),
            resolver, limits);
        if (!result)
            return detail::failure("import", result.error().message);
        budget.release(prior_resolver_copy_charge);
        imported.emplace(std::move(result).value());
    }

    auto registry = pulp::timeline::make_builtin_timeline_registry();
    if (!registry)
        return detail::failure("registry", "could not construct the built-in schema registry");
    auto serialized = pulp::timeline::serialize_project(*imported, registry.value());
    if (!serialized)
        return detail::failure("import", detail::persistence_message(serialized.error()),
                               serialized.error().path);

    auto publisher = DirectoryPublisher::create(output_directory);
    if (!publisher)
        return detail::failure("publish",
                               "output directory must not exist and its parent must exist",
                               filesystem_path_to_utf8(output_directory));
    if (!publisher->write("project.json", serialized.value().json))
        return detail::failure("publish", "could not stage canonical project.json");
    for (const auto& path : media_paths)
        if (!publisher->write(path, archive_entries.at(path)))
            return detail::failure("publish", "could not stage imported sibling media", path);
    if (!publisher->commit())
        return detail::failure("publish", "output directory appeared before atomic publication",
                               filesystem_path_to_utf8(output_directory));
    return {0, output_json(format_text, output_directory, fidelity)};
}

} // namespace pulp::tools::timeline
