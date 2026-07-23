#include <pulp/tools/timeline/agent.hpp>

#include <pulp/audio/audio_file.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/playback/audio_renderer.hpp>
#include <pulp/playback/program_compiler.hpp>
#include <pulp/playback/transport.hpp>
#include <pulp/runtime/crypto.hpp>
#include <pulp/timeline/document_session.hpp>
#include <pulp/timeline/schema_codegen.hpp>
#include <pulp/timeline/serialize.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <utility>
#include <vector>

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
        if (size != 0 &&
            !stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size())))
            return std::nullopt;
        if (stream.peek() != std::ifstream::traits_type::eof())
            return std::nullopt;
        return bytes;
    } catch (...) {
        return std::nullopt;
    }
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
load_assets(const LoadedProject& project) {
    std::vector<DecodedAudioAsset> decoded;
    decoded.reserve(project.value.assets().size());
    std::uint64_t decoded_pcm_bytes = 0;
    for (const auto& asset : project.value.assets()) {
        fs::path resolved;
        std::string resolved_hint;
        for (const auto& locator : asset.locators) {
            if (locator.hint.empty())
                continue;
            fs::path candidate(locator.hint);
            if (locator.kind == pulp::timeline::AssetLocatorKind::PackageRelative ||
                candidate.is_relative())
                candidate = project.base_directory / candidate;
            std::error_code error;
            if (fs::is_regular_file(candidate, error)) {
                resolved = std::move(candidate);
                resolved_hint = locator.hint;
                break;
            }
        }
        if (resolved.empty()) {
            AudioRendererError error;
            error.code = AudioRendererErrorCode::MissingDecodedAsset;
            error.item = asset.id;
            return runtime::Err(error);
        }
        const auto remaining_bytes = kMaxAssetWorkingSetBytes - decoded_pcm_bytes;
        const auto bytes = read_file(resolved, remaining_bytes);
        if (!bytes) {
            AudioRendererError error;
            error.code = AudioRendererErrorCode::CapacityExceeded;
            error.item = asset.id;
            return runtime::Err(error);
        }
        const auto expected_hash = asset.content_hash.to_hex();
        const auto actual_hash = pulp::runtime::sha256_hex(*bytes);
        // DAWproject XML import cannot seal media until its package is
        // extracted, so that importer uses the package path hash as an explicit
        // provisional identity. Every other identity must match the bytes.
        const auto provisional_hash = pulp::runtime::sha256_hex(resolved_hint);
        if (actual_hash != expected_hash && provisional_hash != expected_hash) {
            AudioRendererError error;
            error.code = AudioRendererErrorCode::InvalidAsset;
            error.item = asset.id;
            return runtime::Err(error);
        }
        if (bytes->size() >= remaining_bytes) {
            AudioRendererError error;
            error.code = AudioRendererErrorCode::CapacityExceeded;
            error.item = asset.id;
            return runtime::Err(error);
        }
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
    auto assets = load_assets(loaded);
    if (!assets && !loaded.value.assets().empty()) {
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

std::uint64_t render_frame_count(const pulp::timeline::Sequence& sequence,
                                 const CompiledTempoMap& tempo_map, std::uint32_t sample_rate) {
    std::uint64_t frames = 0;
    if (const auto duration = sequence.duration()) {
        const auto samples = tempo_map.ticks_to_samples({duration->value});
        if (samples.value > 0)
            frames = static_cast<std::uint64_t>(samples.value);
    }
    if (const auto duration = sequence.absolute_duration()) {
        const long double scaled = static_cast<long double>(duration->sample_count) * sample_rate *
                                   duration->sample_rate.denominator /
                                   duration->sample_rate.numerator;
        if (scaled > 0.0L &&
            scaled <= static_cast<long double>(std::numeric_limits<std::uint64_t>::max()))
            frames = std::max(frames, static_cast<std::uint64_t>(std::ceil(scaled)));
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
    if (sample_rate == 0)
        return failure("explain", "sample_rate must be positive", {}, 2);
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
    if (sample_rate == 0 || output.empty())
        return failure("render", "output and a positive sample_rate are required", {}, 2);
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
    const auto frames = render_frame_count(*sequence, *compiled.value()->tempo_map, sample_rate);
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
    rendered.channels.assign(channels, std::vector<float>(static_cast<std::size_t>(frames)));

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
    if (!pulp::audio::write_wav_file(std::string(output), rendered,
                                     pulp::audio::WavBitDepth::Float32))
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
