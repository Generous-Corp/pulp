#include "cmd_audio_heritage.hpp"
#include "json_writer.hpp"

#include <pulp/audio/audio_file.hpp>
#include <pulp/audio/sample_heritage.hpp>
#include <pulp/audio/sample_heritage_json.hpp>
#include <pulp/runtime/crypto.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <variant>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace pulp::audio;

namespace {

constexpr std::size_t kDefaultRenderFrames = 4096;
constexpr std::size_t kDefaultBlockSize = 256;
constexpr std::size_t kMaximumRenderFrames = 100000000;
constexpr double kPulpSamplerStreamConsumptionCap = 4.0;

void print_usage() {
    std::cout << "Usage:\n"
                 "  pulp audio heritage validate PROFILE [--json]\n"
                 "  pulp audio heritage canonicalize PROFILE --out FILE\n"
                 "  pulp audio heritage inspect PROFILE [--json]\n"
                 "  pulp audio heritage render PROFILE --fixture <impulse|sine|two-tone|WAV>\n"
                 "      --out WAV --report JSON [--frames N] [--block-size N]\n\n"
                 "Profiles are strict schema-v3 JSON. All file paths are resolved from the\n"
                 "caller's current directory; the command performs no network access.\n";
}

bool read_bytes(const fs::path& path, std::vector<std::uint8_t>& bytes) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return false;
    input.seekg(0, std::ios::end);
    const auto size = input.tellg();
    if (size < 0)
        return false;
    input.seekg(0, std::ios::beg);
    bytes.resize(static_cast<std::size_t>(size));
    if (!bytes.empty())
        input.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    return input.good() || input.eof();
}

fs::path absolute_path(const std::string& text) {
    std::error_code ec;
    auto result = fs::absolute(fs::path(text), ec);
    return ec ? fs::path{} : result.lexically_normal();
}

std::optional<fs::path> reserve_temporary_sibling(const fs::path& destination, std::string& error) {
    static std::atomic<std::uint64_t> serial{0};
#if defined(_WIN32)
    const auto process_id = static_cast<std::uint64_t>(_getpid());
#else
    const auto process_id = static_cast<std::uint64_t>(::getpid());
#endif
    for (std::size_t attempt = 0; attempt < 128; ++attempt) {
        auto temporary = destination;
        temporary += ".tmp." + std::to_string(process_id) + "." +
                     std::to_string(serial.fetch_add(1, std::memory_order_relaxed));
#if defined(_WIN32)
        const auto descriptor = _wopen(
            temporary.c_str(), _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY, _S_IREAD | _S_IWRITE);
        if (descriptor >= 0) {
            _close(descriptor);
            return temporary;
        }
        if (errno != EEXIST)
            break;
#else
        const auto descriptor = ::open(temporary.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
        if (descriptor >= 0) {
            ::close(descriptor);
            return temporary;
        }
        if (errno != EEXIST)
            break;
#endif
    }
    error = "could not reserve a unique temporary output beside " + destination.string();
    return std::nullopt;
}

bool prepare_destination(const fs::path& destination, std::string& error) {
    if (destination.empty()) {
        error = "could not resolve output path";
        return false;
    }
    const auto parent = destination.parent_path();
    std::error_code ec;
    if (!parent.empty())
        fs::create_directories(parent, ec);
    if (ec) {
        error = "could not create output directory: " + ec.message();
        return false;
    }
    return true;
}

bool publish_temporary(const fs::path& temporary, const fs::path& destination, std::string& error) {
    std::error_code ec;
#if defined(_WIN32)
    if (::MoveFileExW(temporary.c_str(), destination.c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return true;
    ec = std::error_code(static_cast<int>(::GetLastError()), std::system_category());
#else
    fs::rename(temporary, destination, ec);
#endif
    if (!ec)
        return true;
    std::error_code cleanup_error;
    fs::remove(temporary, cleanup_error);
    error = "could not publish output atomically: " + ec.message();
    return false;
}

bool write_text_atomic(const fs::path& destination, std::string_view text, std::string& error) {
    if (!prepare_destination(destination, error))
        return false;
    const auto reserved = reserve_temporary_sibling(destination, error);
    if (!reserved)
        return false;
    const auto& temporary = *reserved;
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            error = "could not open temporary output";
            return false;
        }
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!output.good()) {
            output.close();
            fs::remove(temporary);
            error = "could not write temporary output";
            return false;
        }
    }
    return publish_temporary(temporary, destination, error);
}

bool write_wav_atomic(const fs::path& destination, const AudioFileData& audio, std::string& error) {
    if (!prepare_destination(destination, error))
        return false;
    const auto reserved = reserve_temporary_sibling(destination, error);
    if (!reserved)
        return false;
    const auto& temporary = *reserved;
    if (!write_wav_file(temporary.string(), audio, WavBitDepth::Float32)) {
        fs::remove(temporary);
        error = "could not write Float32 WAV";
        return false;
    }
    return publish_temporary(temporary, destination, error);
}

bool parse_size(std::string_view text, std::size_t& value) {
    if (text.empty())
        return false;
    std::uint64_t parsed = 0;
    const auto [end, status] = std::from_chars(text.data(), text.data() + text.size(), parsed, 10);
    if (status != std::errc{} || end != text.data() + text.size() || parsed == 0 ||
        parsed > kMaximumRenderFrames || parsed > std::numeric_limits<std::size_t>::max())
        return false;
    value = static_cast<std::size_t>(parsed);
    return true;
}

std::string_view json_status_name(SampleHeritageJsonStatus status) {
    switch (status) {
    case SampleHeritageJsonStatus::Ok:
        return "ok";
    case SampleHeritageJsonStatus::InvalidJson:
        return "invalid_json";
    case SampleHeritageJsonStatus::RootNotObject:
        return "root_not_object";
    case SampleHeritageJsonStatus::UnknownField:
        return "unknown_field";
    case SampleHeritageJsonStatus::DuplicateField:
        return "duplicate_field";
    case SampleHeritageJsonStatus::MissingField:
        return "missing_field";
    case SampleHeritageJsonStatus::WrongType:
        return "wrong_type";
    case SampleHeritageJsonStatus::InvalidEnum:
        return "invalid_enum";
    case SampleHeritageJsonStatus::NumberOutOfRange:
        return "number_out_of_range";
    case SampleHeritageJsonStatus::ProfileValidationFailed:
        return "profile_validation_failed";
    }
    return "unknown";
}

std::string_view profile_status_name(SampleHeritageProfileStatus status) {
    switch (status) {
    case SampleHeritageProfileStatus::Ok:
        return "ok";
    case SampleHeritageProfileStatus::UnsupportedSchemaVersion:
        return "unsupported_schema_version";
    case SampleHeritageProfileStatus::InvalidProfileId:
        return "invalid_profile_id";
    case SampleHeritageProfileStatus::InvalidHostSampleRate:
        return "invalid_host_sample_rate";
    case SampleHeritageProfileStatus::TooManyStages:
        return "too_many_stages";
    case SampleHeritageProfileStatus::TooManyBlocks:
        return "too_many_blocks";
    case SampleHeritageProfileStatus::WrongBlockDomain:
        return "wrong_block_domain";
    case SampleHeritageProfileStatus::MixedLegacyAndTypedBlocks:
        return "mixed_legacy_and_typed_blocks";
    case SampleHeritageProfileStatus::NonCanonicalProfileRepresentation:
        return "noncanonical_profile_representation";
    case SampleHeritageProfileStatus::DuplicateStage:
        return "duplicate_stage";
    case SampleHeritageProfileStatus::InvalidStageOrder:
        return "invalid_stage_order";
    case SampleHeritageProfileStatus::InvalidStageParameter:
        return "invalid_stage_parameter";
    case SampleHeritageProfileStatus::UnsupportedRateConversion:
        return "unsupported_rate_conversion";
    case SampleHeritageProfileStatus::DigestUnavailable:
        return "digest_unavailable";
    }
    return "unknown";
}

std::string digest_hex(const std::array<std::uint8_t, 32>& digest) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string result(64, '0');
    for (std::size_t index = 0; index < digest.size(); ++index) {
        result[index * 2] = hex[digest[index] >> 4];
        result[index * 2 + 1] = hex[digest[index] & 0x0f];
    }
    return result;
}

SampleHeritageJsonParseResult load_profile(const fs::path& path, std::string& file_error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        file_error = "could not read profile " + path.string();
        return {};
    }
    const std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    return parse_sample_heritage_profile_json(text);
}

std::string diagnostic_json(const SampleHeritageJsonParseResult& parsed) {
    return "{\"status\":\"" + std::string(parsed.valid() ? "ok" : "invalid") +
           "\",\"field_path\":" + pulp::cli::json_string(parsed.field_path) +
           ",\"profile_status\":\"" + std::string(profile_status_name(parsed.profile_status)) +
           "\",\"json_status\":\"" + std::string(json_status_name(parsed.status)) + "\"}";
}

template <typename Block> std::string_view block_name() {
    if constexpr (std::is_same_v<Block, SampleHeritageVoiceMachineDomainBlock>)
        return "voice.machine_domain";
    if constexpr (std::is_same_v<Block, SampleHeritageVoiceClockBlock>)
        return "voice.clock";
    if constexpr (std::is_same_v<Block, SampleHeritageVoicePitchBlock>)
        return "voice.pitch";
    if constexpr (std::is_same_v<Block, SampleHeritageVoiceConverterBlock>)
        return "voice.converter";
    if constexpr (std::is_same_v<Block, SampleHeritageVoiceLiveCyclicStretchBlock>)
        return "voice.live_cyclic_stretch";
    if constexpr (std::is_same_v<Block, SampleHeritageVoiceHoldDroopBlock>)
        return "voice.hold_droop";
    if constexpr (std::is_same_v<Block, SampleHeritageVoiceReconstructionBlock>)
        return "voice.reconstruction";
    if constexpr (std::is_same_v<Block, SampleHeritageVoiceAnalogColorBlock>)
        return "voice.analog_color";
    if constexpr (std::is_same_v<Block, SampleHeritageBusNoiseIdleBlock>)
        return "bus.noise_idle";
    if constexpr (std::is_same_v<Block, SampleHeritageBusOutputDriveBlock>)
        return "bus.output_drive";
    if constexpr (std::is_same_v<Block, SampleHeritageRecordInputDriveClipBlock>)
        return "record_commit.input_drive_clip";
    if constexpr (std::is_same_v<Block, SampleHeritageRecordRateBlock>)
        return "record_commit.anti_alias_record_rate";
    if constexpr (std::is_same_v<Block, SampleHeritageRecordConverterBlock>)
        return "record_commit.converter";
    if constexpr (std::is_same_v<Block, SampleHeritageRecordCommitCyclicStretchBlock>)
        return "record_commit.commit_stretch.cyclic";
    return "record_commit.commit_stretch.adaptive";
}

struct ProfileSummary {
    std::vector<std::string_view> mechanisms;
    struct Seed {
        std::string_view mechanism;
        std::uint64_t value = 0;
        SampleHeritageSeedPolicy policy = SampleHeritageSeedPolicy::RestartFromProfileSeed;
    };
    std::vector<Seed> seeds;
    bool live_cyclic = false;
    bool commit_stretch = false;
    bool voice_artifact_path = false;
    double clock_ratio = 1.0;
    double live_source_consumption_ratio = 1.0;
};

ProfileSummary summarize_profile(const SampleHeritageProfile& profile) {
    ProfileSummary result;
    const auto visit = [&](const auto& specs) {
        for (const auto& spec : specs) {
            if (spec.bypass)
                continue;
            std::visit(
                [&](const auto& block) {
                    using Block = std::decay_t<decltype(block)>;
                    const auto name = block_name<Block>();
                    result.mechanisms.push_back(name);
                    if constexpr (std::is_same_v<Block, SampleHeritageVoiceConverterBlock> ||
                                  std::is_same_v<Block, SampleHeritageRecordConverterBlock>)
                        result.seeds.push_back({name, block.seed, block.seed_policy});
                    else if constexpr (std::is_same_v<Block, SampleHeritageBusNoiseIdleBlock> ||
                                       std::is_same_v<Block,
                                                      SampleHeritageVoiceLiveCyclicStretchBlock>)
                        result.seeds.push_back({name, block.seed, block.seed_policy});
                    if constexpr (std::is_same_v<Block,
                                                 SampleHeritageVoiceLiveCyclicStretchBlock>) {
                        result.live_cyclic = true;
                        result.voice_artifact_path = true;
                        result.live_source_consumption_ratio = 1.0 / block.factor;
                    }
                    if constexpr (std::is_same_v<Block, SampleHeritageVoicePitchBlock>) {
                        result.voice_artifact_path = true;
                    }
                    if constexpr (std::is_same_v<Block, SampleHeritageVoiceClockBlock>)
                        result.clock_ratio = block.ratio;
                    if constexpr (std::is_same_v<Block,
                                                 SampleHeritageRecordCommitCyclicStretchBlock> ||
                                  std::is_same_v<Block,
                                                 SampleHeritageRecordCommitAdaptiveStretchBlock>)
                        result.commit_stretch = true;
                },
                spec.parameters);
        }
    };
    visit(profile.voice);
    visit(profile.bus);
    visit(profile.record_commit);
    return result;
}

std::optional<double> active_live_cyclic_factor(const SampleHeritageProfile& profile) {
    for (const auto& spec : profile.voice) {
        if (spec.bypass)
            continue;
        if (const auto* live =
                std::get_if<SampleHeritageVoiceLiveCyclicStretchBlock>(&spec.parameters))
            return live->factor;
    }
    return std::nullopt;
}

std::string inspect_json(const SampleHeritageProfileValidation& validated,
                         const SampleHeritageProfile& profile) {
    const auto summary = summarize_profile(profile);
    SampleHeritageEngine engine;
    const auto prepared = engine.prepare(
        {.profile = validated.profile, .channel_count = 1, .maximum_output_frames = 1});
    const auto latency =
        prepared == SampleHeritagePrepareStatus::Ok ? engine.latency_output_frames() : 0.0;
    const auto source_consumption = summary.clock_ratio * summary.live_source_consumption_ratio;
    const auto maximum_stream_note_pitch_factor =
        std::min(SampleHeritagePitchProcessor::kMaximumFactor,
                 kPulpSamplerStreamConsumptionCap / source_consumption);
    std::ostringstream output;
    output << std::setprecision(17)
           << "{\"status\":\"ok\",\"schema_version\":" << profile.schema_version
           << ",\"profile_id\":" << pulp::cli::json_string(profile.profile_id)
           << ",\"profile_digest_sha256\":\"" << digest_hex(validated.profile.profile_digest)
           << "\",\"host_sample_rate\":" << profile.host_sample_rate
           << ",\"block_counts\":{\"voice\":" << profile.voice.size()
           << ",\"bus\":" << profile.bus.size()
           << ",\"record_commit\":" << profile.record_commit.size() << "},\"active_mechanisms\":[";
    for (std::size_t index = 0; index < summary.mechanisms.size(); ++index) {
        if (index)
            output << ',';
        output << pulp::cli::json_string(summary.mechanisms[index]);
    }
    output << "],\"stateful_seeds\":[";
    for (std::size_t index = 0; index < summary.seeds.size(); ++index) {
        if (index)
            output << ',';
        const auto& seed = summary.seeds[index];
        output << "{\"mechanism\":" << pulp::cli::json_string(seed.mechanism) << ",\"seed\":\""
               << seed.value << "\",\"policy\":\""
               << (seed.policy == SampleHeritageSeedPolicy::RestartFromProfileSeed
                       ? "restart_from_profile_seed"
                       : "continue_serialized_state")
               << "\"}";
    }
    output << "],\"capabilities\":{\"voice_playback\":"
           << (!profile.voice.empty() ? "true" : "false")
           << ",\"bus_processing\":" << (!profile.bus.empty() ? "true" : "false")
           << ",\"record_commit\":" << (!profile.record_commit.empty() ? "true" : "false")
           << ",\"live_cyclic_stretch\":" << (summary.live_cyclic ? "true" : "false")
           << ",\"commit_stretch\":" << (summary.commit_stretch ? "true" : "false") << "}"
           << ",\"execution\":{\"status\":\""
           << (prepared == SampleHeritagePrepareStatus::Ok ? "ok" : "unavailable")
           << "\",\"latency_frames\":" << latency
           << ",\"profile_source_frames_per_output\":" << source_consumption
           << ",\"note_pitch_multiplier\":\"external_runtime\""
           << ",\"streaming_admission\":{\"resident_source\":\"exempt\""
           << ",\"streamed_source_cap_frames_per_output\":" << kPulpSamplerStreamConsumptionCap
           << ",\"maximum_note_pitch_factor_at_cap\":" << maximum_stream_note_pitch_factor << "}"
           << ",\"clean_path_assistance\":{\"resident_mip_selection\":\""
           << (summary.voice_artifact_path ? "suppressed_when_active" : "unchanged")
           << "\",\"streamed_mip_selection\":\""
           << (summary.voice_artifact_path ? "suppressed_when_active" : "unchanged")
           << "\",\"sinc_promotion\":\""
           << (summary.voice_artifact_path ? "suppressed_when_active" : "unchanged") << "\"}}}";
    return output.str();
}

int validate_command(const std::vector<std::string>& args) {
    bool json = false;
    bool have_json = false;
    std::string profile_arg;
    for (const auto& arg : args) {
        if (arg == "--json" && !have_json) {
            json = true;
            have_json = true;
        } else if (!arg.empty() && arg.front() == '-') {
            std::cerr << "heritage validate: unknown option " << arg << '\n';
            return 2;
        } else if (profile_arg.empty())
            profile_arg = arg;
        else {
            std::cerr << "heritage validate: exactly one PROFILE is required\n";
            return 2;
        }
    }
    if (profile_arg.empty()) {
        std::cerr << "heritage validate: PROFILE is required\n";
        return 2;
    }
    std::string file_error;
    const auto parsed = load_profile(absolute_path(profile_arg), file_error);
    if (!file_error.empty()) {
        if (json)
            std::cout << "{\"status\":\"error\",\"field_path\":\"\","
                         "\"profile_status\":\"unavailable\",\"json_status\":"
                         "\"file_error\",\"detail\":"
                      << pulp::cli::json_string(file_error) << "}\n";
        else
            std::cerr << "Error: " << file_error << '\n';
        return 1;
    }
    if (json)
        std::cout << diagnostic_json(parsed) << '\n';
    else if (parsed.valid())
        std::cout << "Valid Heritage profile: " << parsed.profile.profile_id << '\n';
    else
        std::cerr << "Invalid Heritage profile: " << json_status_name(parsed.status) << " at "
                  << (parsed.field_path.empty() ? "<root>" : parsed.field_path)
                  << " (profile_status=" << profile_status_name(parsed.profile_status) << ")\n";
    return parsed.valid() ? 0 : 1;
}

int canonicalize_command(const std::vector<std::string>& args) {
    std::string profile_arg;
    std::string output_arg;
    for (std::size_t index = 0; index < args.size(); ++index) {
        const auto& arg = args[index];
        if (arg == "--out") {
            if (++index == args.size() || !output_arg.empty()) {
                std::cerr << "heritage canonicalize: --out requires one FILE\n";
                return 2;
            }
            output_arg = args[index];
        } else if (!arg.empty() && arg.front() == '-') {
            std::cerr << "heritage canonicalize: unknown option " << arg << '\n';
            return 2;
        } else if (profile_arg.empty())
            profile_arg = arg;
        else {
            std::cerr << "heritage canonicalize: exactly one PROFILE is required\n";
            return 2;
        }
    }
    if (profile_arg.empty() || output_arg.empty()) {
        std::cerr << "heritage canonicalize: PROFILE and --out FILE are required\n";
        return 2;
    }
    std::string error;
    const auto parsed = load_profile(absolute_path(profile_arg), error);
    if (!error.empty()) {
        std::cerr << "Error: " << error << '\n';
        return 1;
    }
    if (!parsed.valid()) {
        std::cerr << "Invalid Heritage profile: " << json_status_name(parsed.status) << " at "
                  << (parsed.field_path.empty() ? "<root>" : parsed.field_path) << '\n';
        return 1;
    }
    const auto written = write_sample_heritage_profile_json(parsed.profile);
    if (!written.valid()) {
        std::cerr << "Error: canonical writer rejected profile (profile_status="
                  << profile_status_name(written.profile_status) << ")\n";
        return 1;
    }
    const auto output = absolute_path(output_arg);
    if (!write_text_atomic(output, written.json, error)) {
        std::cerr << "Error: " << error << '\n';
        return 1;
    }
    std::cout << "Wrote canonical Heritage profile " << output << '\n';
    return 0;
}

int inspect_command(const std::vector<std::string>& args) {
    bool json = false;
    bool have_json = false;
    std::string profile_arg;
    for (const auto& arg : args) {
        if (arg == "--json" && !have_json) {
            json = true;
            have_json = true;
        } else if (!arg.empty() && arg.front() == '-') {
            std::cerr << "heritage inspect: unknown option " << arg << '\n';
            return 2;
        } else if (profile_arg.empty())
            profile_arg = arg;
        else {
            std::cerr << "heritage inspect: exactly one PROFILE is required\n";
            return 2;
        }
    }
    if (profile_arg.empty()) {
        std::cerr << "heritage inspect: PROFILE is required\n";
        return 2;
    }
    std::string error;
    const auto parsed = load_profile(absolute_path(profile_arg), error);
    if (!error.empty()) {
        std::cerr << "Error: " << error << '\n';
        return 1;
    }
    if (!parsed.valid()) {
        std::cerr << "Invalid Heritage profile: " << json_status_name(parsed.status) << " at "
                  << (parsed.field_path.empty() ? "<root>" : parsed.field_path) << '\n';
        return 1;
    }
    const auto validated = validate_sample_heritage_profile(parsed.profile);
    if (!validated.valid()) {
        std::cerr << "Invalid Heritage profile: " << profile_status_name(validated.status) << '\n';
        return 1;
    }
    if (json) {
        std::cout << inspect_json(validated, parsed.profile) << '\n';
        return 0;
    }
    const auto summary = summarize_profile(parsed.profile);
    SampleHeritageEngine engine;
    const auto prepared = engine.prepare(
        {.profile = validated.profile, .channel_count = 1, .maximum_output_frames = 1});
    const auto latency =
        prepared == SampleHeritagePrepareStatus::Ok ? engine.latency_output_frames() : 0.0;
    const auto source_consumption = summary.clock_ratio * summary.live_source_consumption_ratio;
    const auto maximum_stream_note_pitch_factor =
        std::min(SampleHeritagePitchProcessor::kMaximumFactor,
                 kPulpSamplerStreamConsumptionCap / source_consumption);
    std::cout << "Heritage profile " << parsed.profile.profile_id << '\n'
              << "  schema: " << parsed.profile.schema_version << '\n'
              << "  digest: " << digest_hex(validated.profile.profile_digest) << '\n'
              << "  host rate: " << std::setprecision(17) << parsed.profile.host_sample_rate
              << " Hz\n"
              << "  blocks: voice=" << parsed.profile.voice.size()
              << " bus=" << parsed.profile.bus.size()
              << " record_commit=" << parsed.profile.record_commit.size() << '\n'
              << "  active mechanisms: " << summary.mechanisms.size() << '\n'
              << "  stateful seeds: " << summary.seeds.size() << '\n'
              << "  latency: " << latency << " frames"
              << (prepared == SampleHeritagePrepareStatus::Ok ? "" : " (unavailable)") << '\n'
              << "  profile source consumption: " << source_consumption
              << " frames/output frame (before runtime note pitch)\n"
              << "  streaming admission: resident exempt; 4x cap permits note-pitch factor "
              << maximum_stream_note_pitch_factor << '\n'
              << "  clean-path assistance: "
              << (summary.voice_artifact_path ? "suppressed when Heritage is active" : "unchanged")
              << '\n';
    for (const auto mechanism : summary.mechanisms)
        std::cout << "    " << mechanism << '\n';
    return 0;
}

enum class FixtureKind { Impulse, Sine, TwoTone, Wav };

struct Fixture {
    FixtureKind kind = FixtureKind::Impulse;
    std::string label;
    std::optional<AudioFileData> wav;
    std::string source_hash;
};

float analytic_sample(FixtureKind kind, std::size_t frame, double sample_rate) {
    constexpr double pi = 3.1415926535897932384626433832795;
    if (kind == FixtureKind::Impulse)
        return frame == 0 ? 1.0f : 0.0f;
    const auto first = std::sin(2.0 * pi * 997.0 * static_cast<double>(frame) / sample_rate);
    if (kind == FixtureKind::Sine)
        return static_cast<float>(first * 0.5);
    const auto second = std::sin(2.0 * pi * 1601.0 * static_cast<double>(frame) / sample_rate);
    return static_cast<float>(first * 0.35 + second * 0.25);
}

float fixture_sample(const Fixture& fixture, std::size_t frame, double sample_rate) {
    if (fixture.kind != FixtureKind::Wav)
        return analytic_sample(fixture.kind, frame, sample_rate);
    const auto& channel = fixture.wav->channels[0];
    return frame < channel.size() ? channel[frame] : 0.0f;
}

Buffer<float> materialize_fixture(const Fixture& fixture, std::size_t frames, double sample_rate) {
    Buffer<float> result(1, frames);
    for (std::size_t frame = 0; frame < frames; ++frame)
        result.channel(0)[frame] = fixture_sample(fixture, frame, sample_rate);
    return result;
}

std::string fixture_hash(const Fixture& fixture, std::size_t frames, double sample_rate) {
    if (fixture.kind == FixtureKind::Wav)
        return fixture.source_hash;
    auto materialized = materialize_fixture(fixture, frames, sample_rate);
    const auto samples = materialized.channel(0);
    return pulp::runtime::sha256_hex(reinterpret_cast<const std::uint8_t*>(samples.data()),
                                     samples.size_bytes());
}

struct RenderOptions {
    std::string profile;
    std::string fixture;
    std::string output;
    std::string report;
    std::size_t frames = 0;
    std::size_t block_size = kDefaultBlockSize;
};

bool parse_render_options(const std::vector<std::string>& args, RenderOptions& options,
                          std::string& error) {
    bool have_fixture = false;
    bool have_output = false;
    bool have_report = false;
    bool have_frames = false;
    bool have_block_size = false;
    for (std::size_t index = 0; index < args.size(); ++index) {
        const auto& arg = args[index];
        if (arg == "--fixture" || arg == "--out" || arg == "--report" || arg == "--frames" ||
            arg == "--block-size") {
            if (++index == args.size()) {
                error = arg + " requires a value";
                return false;
            }
            const auto& value = args[index];
            if (arg == "--fixture" && !have_fixture) {
                options.fixture = value;
                have_fixture = true;
            } else if (arg == "--out" && !have_output) {
                options.output = value;
                have_output = true;
            } else if (arg == "--report" && !have_report) {
                options.report = value;
                have_report = true;
            } else if (arg == "--frames" && !have_frames) {
                if (!parse_size(value, options.frames)) {
                    error = "--frames requires an integer from 1 through " +
                            std::to_string(kMaximumRenderFrames);
                    return false;
                }
                have_frames = true;
            } else if (arg == "--block-size" && !have_block_size) {
                if (!parse_size(value, options.block_size)) {
                    error = "--block-size requires an integer from 1 through " +
                            std::to_string(kMaximumRenderFrames);
                    return false;
                }
                have_block_size = true;
            } else {
                error = arg + " may be specified only once";
                return false;
            }
        } else if (!arg.empty() && arg.front() == '-') {
            error = "unknown option " + arg;
            return false;
        } else if (options.profile.empty())
            options.profile = arg;
        else {
            error = "exactly one PROFILE is required";
            return false;
        }
    }
    if (options.profile.empty() || options.fixture.empty() || options.output.empty() ||
        options.report.empty()) {
        error = "PROFILE, --fixture, --out, and --report are required";
        return false;
    }
    return true;
}

bool load_fixture(const std::string& argument, double sample_rate, Fixture& fixture,
                  std::string& error) {
    if (argument == "impulse") {
        fixture.kind = FixtureKind::Impulse;
        fixture.label = "impulse";
        return true;
    }
    if (argument == "sine") {
        fixture.kind = FixtureKind::Sine;
        fixture.label = "sine";
        return true;
    }
    if (argument == "two-tone") {
        fixture.kind = FixtureKind::TwoTone;
        fixture.label = "two-tone";
        return true;
    }
    const auto path = absolute_path(argument);
    auto wav = read_audio_file(path.string());
    if (!wav) {
        error = "could not decode fixture WAV " + path.string();
        return false;
    }
    if (wav->num_channels() != 1) {
        error = "fixture WAV must be mono; channel conversion is not implicit";
        return false;
    }
    if (static_cast<double>(wav->sample_rate) != sample_rate) {
        error = "fixture WAV sample rate must exactly match profile host_sample_rate; "
                "sample-rate conversion is not implicit";
        return false;
    }
    std::vector<std::uint8_t> bytes;
    if (!read_bytes(path, bytes)) {
        error = "could not hash fixture WAV";
        return false;
    }
    fixture.kind = FixtureKind::Wav;
    fixture.label = "wav";
    fixture.source_hash = pulp::runtime::sha256_hex(bytes.data(), bytes.size());
    fixture.wav = std::move(wav);
    return true;
}

std::string render_report(const SampleHeritageProfile& profile,
                          const SampleHeritageProfileValidation& validated, const Fixture& fixture,
                          std::size_t requested_frames, std::size_t rendered_frames,
                          std::size_t tail_frames, std::size_t block_size, double latency,
                          std::string_view fixture_digest, std::string_view output_digest,
                          const std::optional<SampleHeritageCommittedAssetMetadata>& committed) {
    const auto summary = summarize_profile(profile);
    std::ostringstream output;
    output << std::setprecision(17)
           << "{\"schema\":\"pulp.sample-heritage-render.v1\",\"status\":\"ok\""
           << ",\"profile\":{\"id\":" << pulp::cli::json_string(profile.profile_id)
           << ",\"schema_version\":" << profile.schema_version << ",\"digest_sha256\":\""
           << digest_hex(validated.profile.profile_digest) << "\"}"
           << ",\"fixture\":{\"kind\":" << pulp::cli::json_string(fixture.label) << ",\"sha256\":\""
           << fixture_digest << "\"}"
           << ",\"render\":{\"sample_rate\":" << profile.host_sample_rate
           << ",\"channels\":1,\"requested_frames\":" << requested_frames
           << ",\"tail_frames\":" << tail_frames << ",\"frames\":" << rendered_frames
           << ",\"block_size\":" << block_size << ",\"latency_frames\":" << latency
           << ",\"wav_format\":\"float32\",\"audio_sha256\":\"" << output_digest << "\"}"
           << ",\"seeds\":[";
    for (std::size_t index = 0; index < summary.seeds.size(); ++index) {
        if (index)
            output << ',';
        output << "{\"mechanism\":" << pulp::cli::json_string(summary.seeds[index].mechanism)
               << ",\"value\":\"" << summary.seeds[index].value << "\"}";
    }
    output << "]";
    if (profile.record_commit.empty()) {
        output << ",\"record_commit\":{\"status\":\"not_requested\"}";
    } else {
        const auto& metadata = *committed;
        output << ",\"record_commit\":{\"status\":\"ok\","
                  "\"composition\":\"separate_verified_stage\","
                  "\"note\":\"The committed asset is verified separately; the host-rate "
                  "playback render is not silently composed across a rate boundary.\","
                  "\"sample_rate\":"
               << metadata.committed_sample_rate << ",\"frames\":" << metadata.committed_frames
               << ",\"channels\":" << metadata.committed_channels << ",\"source_audio_sha256\":\""
               << metadata.source_audio_sha256 << "\",\"committed_audio_sha256\":\""
               << metadata.committed_audio_sha256 << "\"}";
    }
    output << '}';
    return output.str();
}

int render_command(const std::vector<std::string>& args) {
    RenderOptions options;
    std::string error;
    if (!parse_render_options(args, options, error)) {
        std::cerr << "heritage render: " << error << '\n';
        return 2;
    }
    const auto output_path = absolute_path(options.output);
    const auto report_path = absolute_path(options.report);
    if (output_path.empty() || report_path.empty() || output_path == report_path) {
        std::cerr << "heritage render: --out and --report must resolve to distinct paths\n";
        return 2;
    }
    const auto profile_path = absolute_path(options.profile);
    if (output_path == profile_path || report_path == profile_path) {
        std::cerr << "heritage render: outputs must not overwrite PROFILE\n";
        return 2;
    }
    if (options.fixture != "impulse" && options.fixture != "sine" &&
        options.fixture != "two-tone") {
        const auto fixture_path = absolute_path(options.fixture);
        if (output_path == fixture_path || report_path == fixture_path) {
            std::cerr << "heritage render: outputs must not overwrite the fixture WAV\n";
            return 2;
        }
    }
    const auto parsed = load_profile(profile_path, error);
    if (!error.empty()) {
        std::cerr << "Error: " << error << '\n';
        return 1;
    }
    if (!parsed.valid()) {
        std::cerr << "Invalid Heritage profile: " << json_status_name(parsed.status) << " at "
                  << (parsed.field_path.empty() ? "<root>" : parsed.field_path) << '\n';
        return 1;
    }
    const auto validated = validate_sample_heritage_profile(parsed.profile);
    if (!validated.valid()) {
        std::cerr << "Invalid Heritage profile: " << profile_status_name(validated.status) << '\n';
        return 1;
    }

    Fixture fixture;
    if (!load_fixture(options.fixture, parsed.profile.host_sample_rate, fixture, error)) {
        std::cerr << "Error: " << error << '\n';
        return 1;
    }
    if (options.frames == 0) {
        options.frames = fixture.kind == FixtureKind::Wav
                             ? static_cast<std::size_t>(fixture.wav->num_frames())
                             : kDefaultRenderFrames;
    }
    if (options.frames == 0 || options.frames > kMaximumRenderFrames) {
        std::cerr << "Error: fixture has no renderable frames\n";
        return 1;
    }
    if (parsed.profile.host_sample_rate != std::floor(parsed.profile.host_sample_rate) ||
        parsed.profile.host_sample_rate >
            static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
        std::cerr << "Error: render requires an integer host_sample_rate representable by WAV\n";
        return 1;
    }

    std::optional<SampleHeritageCommittedAssetMetadata> committed_metadata;
    if (!parsed.profile.record_commit.empty()) {
        auto source = materialize_fixture(fixture, options.frames, parsed.profile.host_sample_rate);
        const SampleHeritageRecordProvenance provenance{
            .source_id = "fixture:" + fixture.label,
            .capture_method = "pulp-audio-heritage-render",
            .evidence_id = "profile:" + parsed.profile.profile_id};
        const auto committed = commit_sample_heritage_recording(
            parsed.profile, static_cast<const Buffer<float>&>(source).view(),
            parsed.profile.host_sample_rate, provenance);
        if (!committed.valid()) {
            std::cerr << "Error: record-commit stage failed: " << committed.detail << '\n';
            return 1;
        }
        committed_metadata = committed.asset->metadata();
    }

    SampleHeritageEngine engine;
    const SampleHeritagePrepareConfig config{.profile = validated.profile,
                                             .channel_count = 1,
                                             .maximum_output_frames = options.block_size,
                                             .external_sinc_bank = nullptr,
                                             .maximum_runtime_clock_factor = 1.0};
    const auto prepare_status = engine.prepare(config);
    if (prepare_status != SampleHeritagePrepareStatus::Ok) {
        std::cerr << "Error: Heritage voice engine preparation failed (status="
                  << static_cast<unsigned>(prepare_status) << ")\n";
        return 1;
    }
    SampleHeritageBusDsp bus;
    const auto bus_status = bus.prepare(validated.profile, parsed.profile.host_sample_rate, 1);
    if (bus_status != SampleHeritageBusDspStatus::Ok) {
        std::cerr << "Error: Heritage bus preparation failed (status="
                  << static_cast<unsigned>(bus_status) << ")\n";
        return 1;
    }

    const auto live_factor = active_live_cyclic_factor(parsed.profile);
    std::size_t exact_live_output_frames = 0;
    if (live_factor) {
        const auto rounded = std::floor(static_cast<long double>(options.frames) *
                                            static_cast<long double>(*live_factor) +
                                        0.5L);
        if (!std::isfinite(rounded) || rounded < 0.0L ||
            rounded > static_cast<long double>(kMaximumRenderFrames)) {
            std::cerr << "Error: stretched fixture exceeds the render safety limit\n";
            return 1;
        }
        exact_live_output_frames = static_cast<std::size_t>(rounded);
    }

    AudioFileData rendered;
    rendered.sample_rate = static_cast<std::uint32_t>(parsed.profile.host_sample_rate);
    rendered.channels.resize(1);
    rendered.channels[0].reserve(live_factor ? exact_live_output_frames : options.frames);

    std::size_t source_cursor = 0;
    const auto process = [&](std::size_t output_frames, std::size_t valid_input_frames,
                             bool end_of_source, bool tail,
                             std::size_t& valid_output_frames) -> bool {
        const auto plan = engine.plan_exact(output_frames);
        if (!plan.valid()) {
            error = "checked Heritage engine plan failed";
            return false;
        }
        Buffer<float> input(1, plan.input_frames);
        if (!tail) {
            for (std::size_t frame = 0; frame < valid_input_frames; ++frame)
                input.channel(0)[frame] =
                    fixture_sample(fixture, source_cursor + frame, parsed.profile.host_sample_rate);
            source_cursor += valid_input_frames;
        }
        Buffer<float> output(1, output_frames);
        const auto status =
            tail
                ? engine.process_tail_exact(plan, static_cast<const Buffer<float>&>(input).view(),
                                            output.view())
                : engine.process_source_exact(plan, static_cast<const Buffer<float>&>(input).view(),
                                              output.view(), valid_input_frames, end_of_source);
        if (status != SampleHeritageProcessStatus::Ok) {
            error = "Heritage voice engine rejected its checked plan";
            return false;
        }
        valid_output_frames = live_factor ? engine.last_valid_output_frames() : output_frames;
        if (valid_output_frames > output_frames) {
            error = "Heritage voice engine reported invalid output dimensions";
            return false;
        }
        if (bus.process(output.view(), valid_output_frames != 0) !=
            SampleHeritageBusDspStatus::Ok) {
            error = "Heritage bus processing failed";
            return false;
        }
        const auto samples = output.channel(0);
        rendered.channels[0].insert(rendered.channels[0].end(), samples.begin(),
                                    samples.begin() +
                                        static_cast<std::ptrdiff_t>(valid_output_frames));
        return true;
    };

    std::size_t tail_frames = 0;
    if (live_factor) {
        bool source_ended = false;
        while (!source_ended) {
            const auto plan = engine.plan_exact(options.block_size);
            if (!plan.valid()) {
                std::cerr << "Error: checked Heritage engine plan failed\n";
                return 1;
            }
            const auto remaining_source = options.frames - source_cursor;
            const auto valid_input_frames = std::min(plan.input_frames, remaining_source);
            source_ended = valid_input_frames == remaining_source;
            std::size_t valid_output_frames = 0;
            if (!process(options.block_size, valid_input_frames, source_ended, false,
                         valid_output_frames)) {
                std::cerr << "Error: " << error << '\n';
                return 1;
            }
            if (rendered.channels[0].size() > kMaximumRenderFrames) {
                std::cerr << "Error: stretched fixture exceeds the render safety limit\n";
                return 1;
            }
        }
        const auto before_drain = rendered.channels[0].size();
        while (rendered.channels[0].size() < exact_live_output_frames) {
            const auto remaining = exact_live_output_frames - rendered.channels[0].size();
            const auto count = std::min(options.block_size, remaining);
            std::size_t valid_output_frames = 0;
            if (!process(count, 0, true, true, valid_output_frames)) {
                std::cerr << "Error: " << error << '\n';
                return 1;
            }
            if (valid_output_frames == 0)
                break;
        }
        if (rendered.channels[0].size() != exact_live_output_frames) {
            std::cerr << "Error: live cyclic render did not reach its exact rounded duration\n";
            return 1;
        }
        tail_frames = rendered.channels[0].size() - before_drain;
    } else {
        for (std::size_t offset = 0; offset < options.frames;) {
            const auto count = std::min(options.block_size, options.frames - offset);
            const auto plan = engine.plan_exact(count);
            if (!plan.valid()) {
                std::cerr << "Error: checked Heritage engine plan failed\n";
                return 1;
            }
            std::size_t valid_output_frames = 0;
            if (!process(count, plan.input_frames, false, false, valid_output_frames)) {
                std::cerr << "Error: " << error << '\n';
                return 1;
            }
            offset += count;
        }
        const auto tail_u64 = engine.tail_output_frames();
        if (tail_u64 > kMaximumRenderFrames ||
            tail_u64 > std::numeric_limits<std::size_t>::max() - options.frames) {
            std::cerr << "Error: Heritage tail exceeds the render safety limit\n";
            return 1;
        }
        tail_frames = static_cast<std::size_t>(tail_u64);
        for (std::size_t offset = 0; offset < tail_frames;) {
            const auto count = std::min(options.block_size, tail_frames - offset);
            std::size_t valid_output_frames = 0;
            if (!process(count, 0, true, true, valid_output_frames)) {
                std::cerr << "Error: " << error << '\n';
                return 1;
            }
            offset += count;
        }
    }

    if (!write_wav_atomic(output_path, rendered, error)) {
        std::cerr << "Error: " << error << '\n';
        return 1;
    }
    std::vector<std::uint8_t> output_bytes;
    if (!read_bytes(output_path, output_bytes)) {
        std::cerr << "Error: could not hash rendered WAV\n";
        return 1;
    }
    const auto output_digest = pulp::runtime::sha256_hex(output_bytes.data(), output_bytes.size());
    const auto input_digest =
        fixture_hash(fixture, options.frames, parsed.profile.host_sample_rate);
    const auto report = render_report(parsed.profile, validated, fixture, options.frames,
                                      rendered.channels[0].size(), tail_frames, options.block_size,
                                      engine.latency_output_frames(), input_digest, output_digest,
                                      committed_metadata);
    if (!write_text_atomic(report_path, report, error)) {
        std::cerr << "Error: " << error << '\n';
        return 1;
    }
    std::cout << "Rendered Heritage fixture to " << output_path << '\n'
              << "Wrote canonical report to " << report_path << '\n';
    return 0;
}

} // namespace

int cmd_audio_heritage(const std::vector<std::string>& args) {
    if (args.empty() || args[0] == "--help" || args[0] == "-h") {
        print_usage();
        return 0;
    }
    if (args.size() == 2 && (args[1] == "--help" || args[1] == "-h") &&
        (args[0] == "validate" || args[0] == "canonicalize" || args[0] == "inspect" ||
         args[0] == "render")) {
        print_usage();
        return 0;
    }
    if (args[0] == "validate")
        return validate_command({args.begin() + 1, args.end()});
    if (args[0] == "canonicalize")
        return canonicalize_command({args.begin() + 1, args.end()});
    if (args[0] == "inspect")
        return inspect_command({args.begin() + 1, args.end()});
    if (args[0] == "render")
        return render_command({args.begin() + 1, args.end()});
    std::cerr << "heritage: unknown verb " << args[0] << '\n';
    print_usage();
    return 2;
}
