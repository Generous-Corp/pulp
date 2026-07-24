#include "timeline_agent_internal.hpp"

#include <pulp/audio/audio_file.hpp>
#include <pulp/playback/audio_renderer.hpp>
#include <pulp/runtime/crypto.hpp>
#include <pulp/runtime/url.hpp>
#include <pulp/timeline/asset_path.hpp>
#include <pulp/timeline/serialize.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pulp::tools::timeline::detail {
namespace {

namespace fs = std::filesystem;

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
        fs::path candidate = filesystem_path_from_utf8(decoded);
        if (candidate.is_relative())
            candidate = base / candidate;
        return candidate;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<fs::path> resolve_package_relative_asset(const fs::path& canonical_base,
                                                       std::string_view hint) {
    if (!pulp::timeline::package_relative_path_is_lexically_safe(hint))
        return std::nullopt;
    try {
        const auto relative = filesystem_path_from_utf8(hint);
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

runtime::Result<std::shared_ptr<const playback::DecodedAudioAssetPool>,
                playback::AudioRendererError>
load_assets(const LoadedProject& project,
            const std::unordered_set<std::uint64_t>& reachable_asset_ids) {
    std::vector<playback::DecodedAudioAsset> decoded;
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
            playback::AudioRendererError error;
            error.code =
                found_unreadable
                    ? playback::AudioRendererErrorCode::CapacityExceeded
                    : (found_regular ? playback::AudioRendererErrorCode::InvalidAsset
                                     : playback::AudioRendererErrorCode::MissingDecodedAsset);
            error.item = asset.id;
            return runtime::Err(error);
        }
        const auto remaining_bytes = kMaxAssetWorkingSetBytes - decoded_pcm_bytes;
        pulp::audio::WavDecodeLimits limits;
        limits.max_output_bytes = remaining_bytes - bytes->size();
        const auto byte_span = std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(bytes->data()), bytes->size());
        auto audio = playback::DecodedAudioAssetPool::decode_wav(asset.id, byte_span, limits);
        if (!audio)
            return runtime::Err(audio.error());
        const auto& data = *audio.value().audio;
        decoded_pcm_bytes += static_cast<std::uint64_t>(data.num_channels()) *
                             static_cast<std::uint64_t>(data.num_frames()) * sizeof(float);
        decoded.push_back(std::move(audio).value());
    }
    return playback::DecodedAudioAssetPool::create(std::move(decoded));
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

} // namespace

runtime::Result<LoadedProject, pulp::timeline::PersistenceError>
load_project(const ProjectSource& source, const pulp::timeline::SchemaRegistry& registry) {
    std::string json;
    fs::path base;
    auto kind = source.kind();
    if (kind == ProjectSourceKind::AutoDetect) {
        const auto text = source.text();
        const auto first = text.find_first_not_of(" \t\r\n");
        kind = first != std::string::npos && text[first] == '{' ? ProjectSourceKind::InlineJson
                                                                : ProjectSourceKind::File;
    }

    if (kind == ProjectSourceKind::InlineJson) {
        json = source.text();
        std::error_code error;
        base = fs::current_path(error);
    } else {
        const fs::path path = source.kind() == ProjectSourceKind::File
                                  ? source.file_path()
                                  : filesystem_path_from_utf8(source.text());
        auto bytes = read_file(path);
        if (!bytes) {
            pulp::timeline::PersistenceError error;
            error.code = pulp::timeline::PersistenceErrorCode::InvalidJson;
            try {
                error.path = filesystem_path_to_utf8(path);
            } catch (...) {
                error.path.clear();
            }
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

runtime::Result<std::unique_ptr<CompiledProject>, playback::CompileError>
compile_project(const LoadedProject& loaded, std::uint32_t sample_rate) {
    auto tempo =
        timebase::CompiledTempoMap::compile(loaded.value.tempo_map().points(), {sample_rate, 1});
    if (!tempo) {
        playback::CompileError error;
        error.code = playback::CompileErrorCode::InvalidRequest;
        return runtime::Err(error);
    }
    const auto* sequence = loaded.value.find_sequence(loaded.value.root_sequence_id());
    if (!sequence) {
        playback::CompileError error;
        error.code = playback::CompileErrorCode::InvalidRequest;
        error.item = loaded.value.root_sequence_id();
        return runtime::Err(error);
    }
    auto assets = load_assets(loaded, reachable_assets(*sequence));
    if (!assets) {
        playback::CompileError error;
        error.code = playback::CompileErrorCode::AudioProgramInvalid;
        error.item = assets.error().item;
        error.audio_detail = assets.error().code;
        return runtime::Err(error);
    }

    auto result = std::make_unique<CompiledProject>();
    result->tempo_map =
        std::make_shared<const timebase::CompiledTempoMap>(std::move(tempo).value());
    playback::DeferredCompileExecutor executor;
    playback::PlaybackProgramCompiler compiler(result->store, executor,
                                               std::chrono::microseconds(0));
    playback::ProgramCompileRequest request;
    request.project = std::make_shared<const pulp::timeline::Project>(loaded.value);
    request.sequence_id = loaded.value.root_sequence_id();
    request.tempo_map = result->tempo_map;
    request.document_revision = 1;
    request.dirty.all = true;
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

} // namespace pulp::tools::timeline::detail
