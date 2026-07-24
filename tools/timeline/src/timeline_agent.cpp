#include <pulp/tools/timeline/agent.hpp>

#include <pulp/audio/audio_file.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/playback/audio_renderer.hpp>
#include <pulp/playback/program_compiler.hpp>
#include <pulp/playback/transport.hpp>
#include <pulp/runtime/crypto.hpp>
#include <pulp/runtime/url.hpp>
#include <pulp/timeline/document_session.hpp>
#include <pulp/timeline/schema_codegen.hpp>
#include <pulp/timeline/serialize.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <ostream>
#include <span>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/acl.h>
#endif
#endif

namespace pulp::tools::timeline {
namespace {

namespace fs = std::filesystem;
using namespace pulp::playback;
using namespace pulp::timebase;

constexpr std::uint64_t kMaxAssetWorkingSetBytes = 512ull * 1024 * 1024;
constexpr std::uint64_t kMaxRenderPcmBytes = 512ull * 1024 * 1024;

struct LoadedProject {
    pulp::timeline::Project value;
    fs::path base_directory;
};

std::string error_json(std::string_view stage, std::string_view message,
                       std::string_view path = {}) {
    std::string result = "{\"error\":{\"message\":";
    result += pulp::timeline::quote_json_string(message);
    if (!path.empty()) {
        result += ",\"path\":";
        result += pulp::timeline::quote_json_string(path);
    }
    result += ",\"stage\":";
    result += pulp::timeline::quote_json_string(stage);
    result += "},\"ok\":false}";
    return result;
}

OperationResult failure(std::string_view stage, std::string_view message,
                        std::string_view path = {}, int exit_code = 1) {
    return {exit_code, error_json(stage, message, path)};
}

std::optional<std::string> read_file(const fs::path& path,
                                     std::uint64_t max_bytes = kMaxAssetWorkingSetBytes) {
    std::error_code error;
    const auto size = fs::file_size(path, error);
    if (error || size > max_bytes || size > std::numeric_limits<std::size_t>::max())
        return std::nullopt;
    try {
        std::ifstream stream(path, std::ios::binary);
        if (!stream)
            return std::nullopt;
        std::string bytes(static_cast<std::size_t>(size), '\0');
        if (size != 0 && !stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size())))
            return std::nullopt;
        if (stream.peek() != std::ifstream::traits_type::eof())
            return std::nullopt;
        return bytes;
    } catch (...) {
        return std::nullopt;
    }
}

bool package_relative_hint_is_lexically_safe(std::string_view hint) {
    if (hint.empty() || hint.front() == '/' || hint.front() == '\\')
        return false;
    if (hint.size() >= 2 && std::isalpha(static_cast<unsigned char>(hint[0])) && hint[1] == ':')
        return false;

    std::size_t component_begin = 0;
    for (std::size_t index = 0; index <= hint.size(); ++index) {
        if (index != hint.size() && hint[index] != '/' && hint[index] != '\\')
            continue;
        if (hint.substr(component_begin, index - component_begin) == "..")
            return false;
        component_begin = index + 1;
    }
    return true;
}

bool path_is_beneath(const fs::path& base, const fs::path& candidate) {
    auto base_component = base.begin();
    auto candidate_component = candidate.begin();
    for (; base_component != base.end() && candidate_component != candidate.end();
         ++base_component, ++candidate_component) {
        if (*base_component != *candidate_component)
            return false;
    }
    return base_component == base.end() && candidate_component != candidate.end();
}

bool has_uri_scheme(std::string_view hint) {
    const auto colon = hint.find(':');
    if (colon == std::string_view::npos || colon == 0)
        return false;
    if (colon == 1 && std::isalpha(static_cast<unsigned char>(hint.front())))
        return false;
    if (!std::isalpha(static_cast<unsigned char>(hint.front())))
        return false;
    return std::all_of(
        hint.begin() + 1, hint.begin() + static_cast<std::ptrdiff_t>(colon), [](char c) {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '+' || c == '-' || c == '.';
        });
}

std::optional<fs::path> resolve_external_asset(const fs::path& base, std::string_view hint) {
    std::string decoded;
    constexpr std::string_view file_scheme = "file://";
    if (hint.starts_with(file_scheme)) {
        auto rest = hint.substr(file_scheme.size());
        if (rest.starts_with('/')) {
            decoded = pulp::runtime::percent_decode(rest);
        } else {
            constexpr std::string_view localhost = "localhost";
            if (!rest.starts_with(localhost) ||
                (rest.size() > localhost.size() && rest[localhost.size()] != '/'))
                return std::nullopt;
            decoded = pulp::runtime::percent_decode(rest.substr(localhost.size()));
        }
#ifdef _WIN32
        if (decoded.size() >= 3 && decoded.front() == '/' &&
            std::isalpha(static_cast<unsigned char>(decoded[1])) && decoded[2] == ':')
            decoded.erase(decoded.begin());
#endif
    } else {
        if (has_uri_scheme(hint))
            return std::nullopt;
        decoded.assign(hint);
    }
    if (decoded.empty() || decoded.find('\0') != std::string::npos)
        return std::nullopt;

    try {
        fs::path candidate(decoded);
        if (candidate.is_relative())
            candidate = base / candidate;
        return candidate;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<fs::path> resolve_package_relative_asset(const fs::path& canonical_base,
                                                       std::string_view hint) {
    if (!package_relative_hint_is_lexically_safe(hint))
        return std::nullopt;
    try {
        const fs::path relative(hint);
        if (relative.has_root_name() || relative.has_root_directory() || relative.is_absolute())
            return std::nullopt;
        std::error_code error;
        auto candidate = fs::canonical(canonical_base / relative, error);
        if (error || !path_is_beneath(canonical_base, candidate))
            return std::nullopt;
        return candidate;
    } catch (...) {
        return std::nullopt;
    }
}

fs::path render_temporary_path(const fs::path& destination, std::uint64_t serial) {
    auto temporary = destination;
#ifdef _WIN32
    const auto process = static_cast<std::uint64_t>(::GetCurrentProcessId());
#else
    const auto process = static_cast<std::uint64_t>(::getpid());
#endif
    temporary += ".tmp." + std::to_string(process) + "." + std::to_string(serial);
    return temporary;
}

void remove_render_temporary(const fs::path& path) {
    std::error_code ignored;
    fs::remove(path, ignored);
}

class NativeRenderStreamBuffer final : public std::streambuf {
  public:
#ifdef _WIN32
    explicit NativeRenderStreamBuffer(HANDLE handle) : handle_(handle) {}
#else
    explicit NativeRenderStreamBuffer(int descriptor) : descriptor_(descriptor) {}
#endif

  protected:
    std::streamsize xsputn(const char* data, std::streamsize count) override {
        if (count <= 0)
            return 0;
        std::streamsize offset = 0;
        while (offset < count) {
#ifdef _WIN32
            const auto remaining = static_cast<std::uint64_t>(count - offset);
            const auto chunk = static_cast<DWORD>(
                std::min<std::uint64_t>(remaining, std::numeric_limits<DWORD>::max()));
            DWORD written = 0;
            if (!::WriteFile(handle_, data + offset, chunk, &written, nullptr) || written == 0)
                break;
            offset += static_cast<std::streamsize>(written);
#else
            const auto remaining = static_cast<std::uint64_t>(count - offset);
            const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(
                remaining, static_cast<std::uint64_t>(std::numeric_limits<ssize_t>::max())));
            const auto written = ::write(descriptor_, data + offset, chunk);
            if (written < 0 && errno == EINTR)
                continue;
            if (written <= 0)
                break;
            offset += static_cast<std::streamsize>(written);
#endif
        }
        return offset;
    }

    int_type overflow(int_type value) override {
        if (traits_type::eq_int_type(value, traits_type::eof()))
            return traits_type::not_eof(value);
        const char byte = traits_type::to_char_type(value);
        return xsputn(&byte, 1) == 1 ? value : traits_type::eof();
    }

    pos_type seekoff(off_type offset, std::ios_base::seekdir direction,
                     std::ios_base::openmode mode) override {
        if ((mode & std::ios_base::out) == 0)
            return pos_type(off_type(-1));
#ifdef _WIN32
        DWORD method = FILE_BEGIN;
        if (direction == std::ios_base::cur)
            method = FILE_CURRENT;
        else if (direction == std::ios_base::end)
            method = FILE_END;
        LARGE_INTEGER distance{};
        distance.QuadPart = static_cast<LONGLONG>(offset);
        LARGE_INTEGER position{};
        if (!::SetFilePointerEx(handle_, distance, &position, method))
            return pos_type(off_type(-1));
        return pos_type(static_cast<off_type>(position.QuadPart));
#else
        int whence = SEEK_SET;
        if (direction == std::ios_base::cur)
            whence = SEEK_CUR;
        else if (direction == std::ios_base::end)
            whence = SEEK_END;
        const auto native_offset = static_cast<off_t>(offset);
        if (static_cast<off_type>(native_offset) != offset)
            return pos_type(off_type(-1));
        const auto position = ::lseek(descriptor_, native_offset, whence);
        return position < 0 ? pos_type(off_type(-1)) : pos_type(static_cast<off_type>(position));
#endif
    }

    pos_type seekpos(pos_type position, std::ios_base::openmode mode) override {
        return seekoff(static_cast<off_type>(position), std::ios_base::beg, mode);
    }

#ifdef _WIN32
    HANDLE handle_;
#else
    int descriptor_;
#endif
};

#ifndef _WIN32
bool sync_render_file(int descriptor) noexcept {
#if defined(__APPLE__) && defined(F_FULLFSYNC)
    if (::fcntl(descriptor, F_FULLFSYNC) == 0)
        return true;
    if (errno != ENOTSUP)
        return false;
#endif
    return ::fsync(descriptor) == 0;
}

bool sync_render_parent_directory(const fs::path& destination) noexcept {
    auto parent = destination.parent_path();
    if (parent.empty())
        parent = ".";
    const int descriptor = ::open(parent.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    if (descriptor < 0)
        return false;
    const bool synced = ::fsync(descriptor) == 0;
    const bool closed = ::close(descriptor) == 0;
    return synced && closed;
}
#endif

bool render_temporary_matches(
#ifdef _WIN32
    HANDLE handle,
#else
    int descriptor,
#endif
    const fs::path& path) noexcept {
#ifdef _WIN32
    BY_HANDLE_FILE_INFORMATION held{};
    if (::GetFileInformationByHandle(handle, &held) == 0)
        return false;
    const auto named = ::CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                     nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (named == INVALID_HANDLE_VALUE)
        return false;
    BY_HANDLE_FILE_INFORMATION current{};
    const bool queried = ::GetFileInformationByHandle(named, &current) != 0;
    ::CloseHandle(named);
    return queried && held.dwVolumeSerialNumber == current.dwVolumeSerialNumber &&
           held.nFileIndexHigh == current.nFileIndexHigh &&
           held.nFileIndexLow == current.nFileIndexLow;
#else
    struct stat held{};
    struct stat current{};
    return ::fstat(descriptor, &held) == 0 && S_ISREG(held.st_mode) && held.st_nlink == 1 &&
           ::lstat(path.c_str(), &current) == 0 && S_ISREG(current.st_mode) &&
           held.st_dev == current.st_dev && held.st_ino == current.st_ino;
#endif
}

bool write_wav_atomic(const fs::path& destination,
                      const pulp::audio::AudioFileData& audio) noexcept {
    static std::atomic<std::uint64_t> next_serial{1};
    fs::path temporary;
#ifdef _WIN32
    constexpr SECURITY_INFORMATION kSecurityInformation =
        OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;
    std::vector<std::uint8_t> security_descriptor;
    SECURITY_ATTRIBUTES security_attributes{};
    SECURITY_ATTRIBUTES* security = nullptr;
    const auto destination_attributes = ::GetFileAttributesW(destination.c_str());
    if (destination_attributes != INVALID_FILE_ATTRIBUTES) {
        if ((destination_attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) !=
            0)
            return false;
        DWORD required = 0;
        ::GetFileSecurityW(destination.c_str(), kSecurityInformation, nullptr, 0, &required);
        if (required == 0 || ::GetLastError() != ERROR_INSUFFICIENT_BUFFER)
            return false;
        security_descriptor.resize(required);
        if (!::GetFileSecurityW(destination.c_str(), kSecurityInformation,
                                security_descriptor.data(), required, &required))
            return false;
        security_attributes.nLength = sizeof(security_attributes);
        security_attributes.lpSecurityDescriptor = security_descriptor.data();
        security = &security_attributes;
    } else if (::GetLastError() != ERROR_FILE_NOT_FOUND &&
               ::GetLastError() != ERROR_PATH_NOT_FOUND) {
        return false;
    }

    HANDLE reservation = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt != 128; ++attempt) {
        temporary = render_temporary_path(destination, next_serial.fetch_add(1));
        reservation = ::CreateFileW(temporary.c_str(), GENERIC_WRITE | DELETE, FILE_SHARE_READ,
                                    security, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (reservation != INVALID_HANDLE_VALUE)
            break;
        if (::GetLastError() != ERROR_FILE_EXISTS && ::GetLastError() != ERROR_ALREADY_EXISTS)
            return false;
    }
    if (reservation == INVALID_HANDLE_VALUE)
        return false;

    NativeRenderStreamBuffer buffer(reservation);
    std::ostream output(&buffer);
    const bool written =
        pulp::audio::write_wav_stream(output, audio, pulp::audio::WavBitDepth::Float32);
    const bool matches = render_temporary_matches(reservation, temporary);
    const bool synced = written && matches && ::FlushFileBuffers(reservation) != 0;
    std::error_code destination_path_error;
    const auto destination_name = fs::absolute(destination, destination_path_error).wstring();
    const auto rename_size = offsetof(FILE_RENAME_INFO, FileName) +
                             destination_name.size() * sizeof(wchar_t);
    std::vector<std::uint8_t> rename_storage(rename_size);
    auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(rename_storage.data());
    rename->ReplaceIfExists = TRUE;
    rename->RootDirectory = nullptr;
    rename->FileNameLength =
        static_cast<DWORD>(destination_name.size() * sizeof(wchar_t));
    std::memcpy(rename->FileName, destination_name.data(), rename->FileNameLength);
    const bool published =
        synced && !destination_path_error &&
        ::SetFileInformationByHandle(reservation, FileRenameInfo, rename,
                                     static_cast<DWORD>(rename_size)) != 0;
    const bool publication_synced = published && ::FlushFileBuffers(reservation) != 0;
    const bool closed = ::CloseHandle(reservation) != 0;
    if (!written || !matches || !synced || !published || !publication_synced || !closed) {
        if (matches)
            remove_render_temporary(temporary);
        return false;
    }
    return true;
#else
    bool destination_exists = false;
    struct stat destination_status{};
    if (::lstat(destination.c_str(), &destination_status) == 0) {
        if (!S_ISREG(destination_status.st_mode))
            return false;
        destination_exists = true;
    } else if (errno != ENOENT) {
        return false;
    }
#if defined(__APPLE__)
    acl_t destination_acl = nullptr;
    if (destination_exists) {
        destination_acl = ::acl_get_file(destination.c_str(), ACL_TYPE_EXTENDED);
        if (destination_acl == nullptr && errno != ENOENT && errno != ENOATTR)
            return false;
        if (destination_acl != nullptr) {
            acl_entry_t entry = nullptr;
            const auto entry_result = ::acl_get_entry(destination_acl, ACL_FIRST_ENTRY, &entry);
            if (entry_result < 0) {
                ::acl_free(destination_acl);
                return false;
            }
            if (entry_result == 0) {
                ::acl_free(destination_acl);
                destination_acl = nullptr;
            }
        }
    }
#endif

    int reservation = -1;
    for (int attempt = 0; attempt != 128; ++attempt) {
        temporary = render_temporary_path(destination, next_serial.fetch_add(1));
        reservation = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
        if (reservation >= 0)
            break;
        if (errno != EEXIST) {
#if defined(__APPLE__)
            if (destination_acl != nullptr)
                ::acl_free(destination_acl);
#endif
            return false;
        }
    }
    if (reservation < 0) {
#if defined(__APPLE__)
        if (destination_acl != nullptr)
            ::acl_free(destination_acl);
#endif
        return false;
    }

    NativeRenderStreamBuffer buffer(reservation);
    std::ostream output(&buffer);
    bool complete = pulp::audio::write_wav_stream(output, audio, pulp::audio::WavBitDepth::Float32);
    const bool matches = render_temporary_matches(reservation, temporary);
    complete = complete && matches;
    if (destination_exists) {
        struct stat temporary_status{};
        complete = complete && ::fstat(reservation, &temporary_status) == 0;
        if (complete && (temporary_status.st_uid != destination_status.st_uid ||
                         temporary_status.st_gid != destination_status.st_gid))
            complete =
                ::fchown(reservation, destination_status.st_uid, destination_status.st_gid) == 0;
        complete = complete && ::fchmod(reservation, destination_status.st_mode &
                                                         static_cast<mode_t>(07777)) == 0;
#if defined(__APPLE__)
        if (complete && destination_acl != nullptr)
            complete = ::acl_set_fd_np(reservation, destination_acl, ACL_TYPE_EXTENDED) == 0;
#endif
    }
#if defined(__APPLE__)
    if (destination_acl != nullptr)
        ::acl_free(destination_acl);
#endif
    complete = complete && sync_render_file(reservation);
    complete = ::close(reservation) == 0 && complete;
    if (!complete) {
        if (matches)
            remove_render_temporary(temporary);
        return false;
    }
    if (::rename(temporary.c_str(), destination.c_str()) == 0)
        return sync_render_parent_directory(destination);
#endif
    remove_render_temporary(temporary);
    return false;
}

runtime::Result<LoadedProject, pulp::timeline::PersistenceError>
load_project(std::string_view source, const pulp::timeline::SchemaRegistry& registry) {
    std::string json;
    fs::path base;
    const auto first = source.find_first_not_of(" \t\r\n");
    if (first != std::string_view::npos && source[first] == '{') {
        json.assign(source);
        std::error_code error;
        base = fs::current_path(error);
    } else {
        fs::path path(source);
        auto bytes = read_file(path);
        if (!bytes) {
            pulp::timeline::PersistenceError error;
            error.code = pulp::timeline::PersistenceErrorCode::InvalidJson;
            error.path = path.string();
            return runtime::Err(std::move(error));
        }
        json = std::move(*bytes);
        std::error_code error;
        base = fs::absolute(path, error).parent_path();
    }
    auto decoded = pulp::timeline::deserialize_project(json, registry);
    if (!decoded)
        return runtime::Err(decoded.error());
    return runtime::Ok(LoadedProject{std::move(decoded).value(), std::move(base)});
}

std::string persistence_message(const pulp::timeline::PersistenceError& error) {
    return "timeline persistence error " + std::to_string(static_cast<unsigned>(error.code));
}

runtime::Result<std::shared_ptr<const DecodedAudioAssetPool>, AudioRendererError>
load_assets(const LoadedProject& project,
            const std::unordered_set<std::uint64_t>& reachable_asset_ids) {
    std::vector<DecodedAudioAsset> decoded;
    decoded.reserve(reachable_asset_ids.size());
    std::uint64_t decoded_pcm_bytes = 0;
    std::error_code package_base_error;
    const auto canonical_package_base = fs::canonical(project.base_directory, package_base_error);
    for (const auto& asset : project.value.assets()) {
        if (!reachable_asset_ids.contains(asset.id.value))
            continue;
        std::optional<std::string> bytes;
        bool found_regular = false;
        bool found_unreadable = false;
        for (const auto& locator : asset.locators) {
            if (locator.hint.empty())
                continue;
            fs::path candidate;
            if (locator.kind == pulp::timeline::AssetLocatorKind::PackageRelative) {
                if (package_base_error)
                    continue;
                auto resolved =
                    resolve_package_relative_asset(canonical_package_base, locator.hint);
                if (!resolved)
                    continue;
                candidate = std::move(*resolved);
            } else {
                auto resolved = resolve_external_asset(project.base_directory, locator.hint);
                if (!resolved)
                    continue;
                candidate = std::move(*resolved);
            }
            std::error_code error;
            if (!fs::is_regular_file(candidate, error))
                continue;
            found_regular = true;
            const auto remaining_bytes = kMaxAssetWorkingSetBytes - decoded_pcm_bytes;
            auto candidate_bytes = read_file(candidate, remaining_bytes);
            if (!candidate_bytes || candidate_bytes->size() >= remaining_bytes) {
                found_unreadable = true;
                continue;
            }
            if (pulp::runtime::sha256_hex(*candidate_bytes) == asset.content_hash.to_hex()) {
                bytes = std::move(candidate_bytes);
                break;
            }
        }
        if (!bytes) {
            AudioRendererError error;
            error.code = found_unreadable
                             ? AudioRendererErrorCode::CapacityExceeded
                             : (found_regular ? AudioRendererErrorCode::InvalidAsset
                                              : AudioRendererErrorCode::MissingDecodedAsset);
            error.item = asset.id;
            return runtime::Err(error);
        }
        const auto remaining_bytes = kMaxAssetWorkingSetBytes - decoded_pcm_bytes;
        pulp::audio::WavDecodeLimits limits;
        limits.max_output_bytes = remaining_bytes - bytes->size();
        const auto byte_span = std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(bytes->data()), bytes->size());
        auto audio = DecodedAudioAssetPool::decode_wav(asset.id, byte_span, limits);
        if (!audio)
            return runtime::Err(audio.error());
        const auto& data = *audio.value().audio;
        decoded_pcm_bytes += static_cast<std::uint64_t>(data.num_channels()) *
                             static_cast<std::uint64_t>(data.num_frames()) * sizeof(float);
        decoded.push_back(std::move(audio).value());
    }
    return DecodedAudioAssetPool::create(std::move(decoded));
}

std::unordered_set<std::uint64_t> reachable_assets(const pulp::timeline::Sequence& sequence) {
    std::unordered_set<std::uint64_t> result;
    for (const auto& track : sequence.tracks()) {
        if (const auto& freeze = track.freeze()) {
            result.insert(freeze->media.asset_id.value);
            continue;
        }
        if (track.active_take_lane_id().valid()) {
            const auto* lane = track.find_take_lane(track.active_take_lane_id());
            if (!lane)
                continue;
            for (const auto& segment : lane->comp_segments()) {
                const auto* take = lane->find_take(segment.take_id);
                if (take)
                    result.insert(take->media().asset_id.value);
            }
            continue;
        }
        for (const auto& clip : track.clips())
            if (const auto* media = std::get_if<pulp::timeline::MediaRef>(&clip.content()))
                result.insert(media->asset_id.value);
    }
    return result;
}

struct CompiledProject {
    std::shared_ptr<const CompiledTempoMap> tempo_map;
    PlaybackProgramStore store;
};

runtime::Result<std::unique_ptr<CompiledProject>, CompileError>
compile_project(const LoadedProject& loaded, std::uint32_t sample_rate) {
    auto tempo = CompiledTempoMap::compile(loaded.value.tempo_map().points(), {sample_rate, 1});
    if (!tempo) {
        CompileError error;
        error.code = CompileErrorCode::InvalidRequest;
        return runtime::Err(error);
    }
    const auto* sequence = loaded.value.find_sequence(loaded.value.root_sequence_id());
    if (!sequence) {
        CompileError error;
        error.code = CompileErrorCode::InvalidRequest;
        error.item = loaded.value.root_sequence_id();
        return runtime::Err(error);
    }
    auto assets = load_assets(loaded, reachable_assets(*sequence));
    if (!assets) {
        CompileError error;
        error.code = CompileErrorCode::AudioProgramInvalid;
        error.item = assets.error().item;
        error.audio_detail = assets.error().code;
        return runtime::Err(error);
    }

    auto result = std::make_unique<CompiledProject>();
    result->tempo_map = std::make_shared<const CompiledTempoMap>(std::move(tempo).value());
    DeferredCompileExecutor executor;
    PlaybackProgramCompiler compiler(result->store, executor, std::chrono::microseconds(0));
    ProgramCompileRequest request;
    request.project = std::make_shared<const pulp::timeline::Project>(loaded.value);
    request.sequence_id = loaded.value.root_sequence_id();
    request.tempo_map = result->tempo_map;
    request.document_revision = 1;
    request.dirty.all = true;
    if (assets)
        request.audio_assets = std::move(assets).value();
    auto submitted = compiler.submit(std::move(request));
    if (!submitted)
        return runtime::Err(submitted.error());
    while (compiler.status().busy)
        executor.run_for(std::chrono::seconds(1), 256);
    const auto status = compiler.status();
    if (status.has_error)
        return runtime::Err(status.last_error);
    return runtime::Ok(std::move(result));
}

void extend_absolute_frame(std::uint64_t& frames, timebase::SamplePosition start,
                           std::uint64_t count, timebase::RationalRate rate,
                           std::uint32_t sample_rate) {
    if (!rate.valid())
        return;
    const auto end = static_cast<long double>(start.value) + static_cast<long double>(count);
    const auto scaled = end * static_cast<long double>(sample_rate) *
                        static_cast<long double>(rate.denominator) /
                        static_cast<long double>(rate.numerator);
    if (scaled > 0.0L &&
        scaled <= static_cast<long double>(std::numeric_limits<std::uint64_t>::max()))
        frames = std::max(frames, static_cast<std::uint64_t>(std::ceil(scaled)));
}

std::uint64_t render_frame_count(const pulp::timeline::Sequence& sequence,
                                 const CompiledTempoMap& tempo_map, const PlaybackProgram& program,
                                 std::uint32_t sample_rate) {
    std::uint64_t frames = 0;
    if (const auto duration = sequence.duration()) {
        const auto samples = tempo_map.ticks_to_samples({duration->value});
        if (samples.value > 0)
            frames = static_cast<std::uint64_t>(samples.value);
    }
    if (const auto duration = sequence.absolute_duration()) {
        extend_absolute_frame(frames, {}, duration->sample_count, duration->sample_rate,
                              sample_rate);
    }
    for (const auto& track : program.tracks()) {
        if (!track->audio_program())
            continue;
        for (const auto& clip : track->audio_program()->clips())
            if (clip.timeline_end() > 0)
                frames = std::max(frames, static_cast<std::uint64_t>(clip.timeline_end()));
    }
    for (const auto& track : sequence.tracks()) {
        if (track.freeze() || track.active_take_lane_id().valid())
            continue;
        for (const auto& clip : track.clips()) {
            if (clip.time_anchor() == pulp::timeline::ClipTimeAnchor::Musical) {
                const auto end = tempo_map.ticks_to_samples(clip.end()).value;
                if (end > 0)
                    frames = std::max(frames, static_cast<std::uint64_t>(end));
            } else {
                extend_absolute_frame(frames, clip.absolute_start(),
                                      clip.absolute_duration_samples(), clip.absolute_sample_rate(),
                                      sample_rate);
            }
        }
    }
    return frames;
}

std::string compile_error_message(const CompileError& error) {
    std::string message =
        "timeline compile error " + std::to_string(static_cast<unsigned>(error.code));
    if (error.item.valid())
        message += " at item " + std::to_string(error.item.value);
    return message;
}

} // namespace

OperationResult project_open(std::string_view project) {
    auto registry = pulp::timeline::make_builtin_timeline_registry();
    if (!registry)
        return failure("registry", "could not construct the built-in schema registry");
    auto loaded = load_project(project, registry.value());
    if (!loaded)
        return failure("open", persistence_message(loaded.error()), loaded.error().path);
    auto serialized = pulp::timeline::serialize_project(loaded.value().value, registry.value());
    if (!serialized)
        return failure("open", persistence_message(serialized.error()), serialized.error().path);
    return {0, "{\"ok\":true,\"project\":" + serialized.value().json + "}"};
}

OperationResult command_apply(std::string_view project, std::string_view commands) {
    auto registry = pulp::timeline::make_builtin_timeline_registry();
    if (!registry)
        return failure("registry", "could not construct the built-in schema registry");
    auto loaded = load_project(project, registry.value());
    if (!loaded)
        return failure("open", persistence_message(loaded.error()), loaded.error().path);
    auto decoded = pulp::timeline::deserialize_commands(commands, registry.value());
    if (!decoded)
        return failure("apply", persistence_message(decoded.error()), decoded.error().path, 2);
    auto session = pulp::timeline::DocumentSession::create(std::move(loaded).value().value);
    if (!session)
        return failure("apply", "could not create a document session");
    auto writer = session.value()->register_writer();
    if (!writer)
        return failure("apply", "could not register a document writer");
    pulp::timeline::Transaction transaction;
    transaction.id = writer.value().allocate_transaction_id();
    transaction.expected_revision = session.value()->revision();
    transaction.commands.reserve(decoded.value().size());
    for (auto& command : decoded.value())
        transaction.commands.push_back({writer.value().allocate_command_id(), std::move(command)});
    auto committed = session.value()->submit(writer.value(), std::move(transaction));
    if (!committed) {
        const auto& error = committed.error();
        return failure("apply",
                       "timeline transaction conflict " +
                           std::to_string(static_cast<unsigned>(error.code)),
                       error.item.valid() ? std::to_string(error.item.value) : std::string{});
    }
    auto serialized =
        pulp::timeline::serialize_project(*committed.value().snapshot, registry.value());
    if (!serialized)
        return failure("apply", persistence_message(serialized.error()), serialized.error().path);
    return {0, "{\"ok\":true,\"project\":" + serialized.value().json + ",\"revision\":\"" +
                   std::to_string(committed.value().revision.value) + "\"}"};
}

OperationResult validate(std::string_view project) {
    auto opened = project_open(project);
    if (!opened)
        return opened;
    return {0, "{\"diagnostics\":[],\"ok\":true}"};
}

OperationResult explain(std::string_view project, std::uint32_t sample_rate) {
    if (sample_rate == 0 || sample_rate > timebase::kMaximumCompiledSampleRate)
        return failure("explain", "sample_rate must be between 1 and 768000", {}, 2);
    auto registry = pulp::timeline::make_builtin_timeline_registry();
    if (!registry)
        return failure("registry", "could not construct the built-in schema registry");
    auto loaded = load_project(project, registry.value());
    if (!loaded)
        return failure("open", persistence_message(loaded.error()), loaded.error().path);
    auto compiled = compile_project(loaded.value(), sample_rate);
    if (!compiled)
        return failure("explain", compile_error_message(compiled.error()));
    auto program = compiled.value()->store.read();
    if (!program)
        return failure("explain", "compiled program was not published");

    std::string json = "{\"generation\":\"" + std::to_string(program->generation()) +
                       "\",\"ok\":true,\"project_id\":\"" +
                       std::to_string(program->project_id().value) + "\",\"sequence_id\":\"" +
                       std::to_string(program->sequence_id().value) + "\",\"tracks\":[";
    bool first_track = true;
    for (const auto& track : program->tracks()) {
        if (!first_track)
            json += ",";
        first_track = false;
        json += "{\"audio_regions\":";
        json += std::to_string(track->audio_program() ? track->audio_program()->clips().size() : 0);
        json += ",\"automation\":";
        json += track->automation_program() ? "true" : "false";
        json += ",\"clip_ids\":[";
        bool first_clip = true;
        for (const auto id : track->ordered_clip_ids()) {
            if (!first_clip)
                json += ",";
            first_clip = false;
            json += "\"" + std::to_string(id.value) + "\"";
        }
        json += "],\"note_events\":";
        json += std::to_string(track->arrangement_note_events().size());
        json += ",\"pdc_offset_samples\":null,\"track_id\":\"";
        json += std::to_string(track->id().value);
        json += "\"}";
    }
    json += "]}";
    return {0, std::move(json)};
}

OperationResult render(std::string_view project, std::string_view output,
                       std::uint32_t sample_rate) {
    if (sample_rate == 0 || sample_rate > timebase::kMaximumCompiledSampleRate || output.empty())
        return failure("render", "output and sample_rate between 1 and 768000 are required", {}, 2);
    auto registry = pulp::timeline::make_builtin_timeline_registry();
    if (!registry)
        return failure("registry", "could not construct the built-in schema registry");
    auto loaded = load_project(project, registry.value());
    if (!loaded)
        return failure("open", persistence_message(loaded.error()), loaded.error().path);
    auto compiled = compile_project(loaded.value(), sample_rate);
    if (!compiled)
        return failure("render", compile_error_message(compiled.error()));
    const auto* sequence =
        loaded.value().value.find_sequence(loaded.value().value.root_sequence_id());
    if (!sequence)
        return failure("render", "root sequence is missing");
    auto program = compiled.value()->store.read();
    if (!program)
        return failure("render", "compiled program was not published");
    const auto frames =
        render_frame_count(*sequence, *compiled.value()->tempo_map, *program, sample_rate);
    if (frames == 0 || frames > std::numeric_limits<std::size_t>::max())
        return failure("render", "sequence duration is empty or too large");

    std::uint32_t channels = 1;
    if (auto program = compiled.value()->store.read()) {
        if (const auto& assets = program->audio_assets_owner())
            for (const auto& asset : assets->assets())
                channels = std::max(channels, asset.audio->num_channels());
    }
    const auto bytes_per_frame = static_cast<std::uint64_t>(channels) * sizeof(float);
    if (frames > kMaxRenderPcmBytes / bytes_per_frame)
        return failure("render", "sequence exceeds the in-memory render budget");

    pulp::audio::AudioFileData rendered;
    rendered.sample_rate = sample_rate;
    try {
        rendered.channels.reserve(channels);
        for (std::uint32_t channel = 0; channel < channels; ++channel)
            rendered.channels.emplace_back(static_cast<std::size_t>(frames));
    } catch (const std::bad_alloc&) {
        return failure("render", "could not allocate the in-memory render buffer");
    } catch (const std::length_error&) {
        return failure("render", "could not allocate the in-memory render buffer");
    }

    MasterTransport transport;
    constexpr std::uint32_t block_size = 512;
    if (transport.prepare(*compiled.value()->tempo_map,
                          {.max_buffer_size = block_size, .initially_playing = true}) !=
        TransportError::None)
        return failure("render", "transport preparation failed");
    std::uint64_t offset = 0;
    while (offset < frames) {
        const auto count =
            static_cast<std::uint32_t>(std::min<std::uint64_t>(block_size, frames - offset));
        TransportSnapshot snapshot;
        if (transport.begin_block(count, snapshot) != TransportError::None)
            return failure("render", "transport block failed");
        std::vector<float*> channel_data;
        channel_data.reserve(channels);
        for (auto& channel : rendered.channels)
            channel_data.push_back(channel.data() + offset);
        pulp::audio::BufferView<float> block(channel_data.data(), channels, count);
        auto program = compiled.value()->store.read();
        if (!program)
            return failure("render", "compiled program disappeared");
        const auto status = ArrangementAudioRenderer::process(*program, snapshot, block);
        if (status != AudioRenderStatus::Rendered && status != AudioRenderStatus::Silent)
            return failure("render",
                           "audio renderer error " + std::to_string(static_cast<unsigned>(status)));
        offset += count;
    }
    if (!write_wav_atomic(fs::path(output), rendered))
        return failure("render", "could not write output WAV", output);
    return {0, "{\"channels\":" + std::to_string(channels) + ",\"frames\":\"" +
                   std::to_string(frames) +
                   "\",\"ok\":true,\"output\":" + pulp::timeline::quote_json_string(output) +
                   ",\"sample_rate\":" + std::to_string(sample_rate) + "}"};
}

OperationResult schema() {
    auto registry = pulp::timeline::make_builtin_timeline_registry();
    if (!registry)
        return failure("registry", "could not construct the built-in schema registry");
    auto manifest = pulp::timeline::emit_schema_manifest(registry.value());
    if (!manifest)
        return failure("schema", persistence_message(manifest.error()), manifest.error().path);
    return {0, std::move(manifest).value()};
}

} // namespace pulp::tools::timeline
