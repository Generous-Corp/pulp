#pragma once

// Shared oracle for the audio container readers, used by the libFuzzer target
// and by the Catch2 corpus replay so both lanes agree on what a finding is.
// approach after SimZhou/vscode-audiolens c5c5a24; re-authored
//
// Properties, given a path and DecodeLimits:
//   - the reader neither crashes nor trips a sanitizer (implicit);
//   - when read_info and read(limits) both succeed, they agree on sample rate
//     and channel count, and on frame count unless info reports 0 (an
//     undeclared length, decoded under the limits); the decoded planar
//     buffers are num_channels long with num_frames samples each;
//   - decoded output never exceeds the limits (bytes = frames x channels x 4).
// A format whose info is an estimate is not special-cased here: a
// disagreement is reported, and whoever owns the reader decides.
//
// This header deliberately shares nothing with core/audio/src's
// decode_limits_detail.hpp: an oracle that reused the code under test would
// agree with its bugs.

#include <pulp/audio/audio_file.hpp>
#include <pulp/audio/format_registry.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace pulp_fuzz::audio_decoder {

// The container formats the fuzz target routes inputs to and the corpus
// replay requires a seed for, with the extensions each may carry; the first
// extension is the one the fuzz target writes. CoreAudio's extensions are
// absent on purpose: that reader is covered by the replay alone.
struct FuzzableFormat {
    std::string_view name;
    std::array<std::string_view, 3> extensions; // unused slots are empty
};
inline constexpr std::array<FuzzableFormat, 5> kFuzzableFormats = {{
    {"wav", {".wav", ".wave", {}}},
    {"aiff", {".aiff", ".aif", ".aifc"}},
    {"flac", {".flac", {}, {}}},
    {"mp3", {".mp3", {}, {}}},
    {"ogg", {".ogg", ".oga", {}}},
}};

struct OracleFailure {
    std::string path;   // the file under test
    std::string field;  // the property that failed, e.g. "num_frames"
    std::string detail; // both sides of the disagreement, human readable
};

using FailureSink = std::function<void(const OracleFailure&)>;

struct OracleOutcome {
    bool info_ok = false;
    bool decode_ok = false;
    std::optional<pulp::audio::AudioFileInfo> info;
    std::optional<pulp::audio::AudioFileData> data;
};

// Ceilings the fuzz lanes decode under. Small enough that a legitimate
// multi-gigabyte claim is refused rather than allocated, large enough that
// every committed seed decodes in full.
inline pulp::audio::DecodeLimits fuzz_limits() {
    pulp::audio::DecodeLimits limits;
    limits.max_frames = 1ull << 22;
    limits.max_channels = 16u;
    limits.max_output_bytes = 64ull * 1024u * 1024u;
    return limits;
}

namespace detail {

inline void report(const FailureSink& fail, const std::string& path, std::string_view field,
                   std::string detail) {
    if (fail)
        fail(OracleFailure{path, std::string(field), std::move(detail)});
}

// Checks the decoded buffers against themselves and against `limits`. Returns
// false when any check failed so the caller can skip the agreement checks
// that would only repeat the same defect.
inline bool check_decoded_shape(const std::string& path, const pulp::audio::AudioFileData& data,
                                const pulp::audio::DecodeLimits& limits, const FailureSink& fail) {
    bool ok = true;
    const auto channels = data.channels.size();
    if (channels == 0) {
        report(fail, path, "decoded.num_channels", "decode succeeded with zero channels");
        return false;
    }
    const auto frames = data.channels.front().size();
    for (std::size_t c = 1; c < channels; ++c) {
        if (data.channels[c].size() != frames) {
            report(fail, path, "decoded.channel_length",
                   "channel " + std::to_string(c) + " holds " +
                       std::to_string(data.channels[c].size()) + " samples, channel 0 holds " +
                       std::to_string(frames));
            ok = false;
        }
    }
    if (limits.max_channels != 0u && channels > limits.max_channels) {
        report(fail, path, "limits.max_channels",
               std::to_string(channels) + " channels decoded, ceiling " +
                   std::to_string(limits.max_channels));
        ok = false;
    }
    if (limits.max_frames != 0u && frames > limits.max_frames) {
        report(fail, path, "limits.max_frames",
               std::to_string(frames) + " frames decoded, ceiling " +
                   std::to_string(limits.max_frames));
        ok = false;
    }
    // The buffers are already in memory, so the product cannot overflow.
    const std::uint64_t bytes = static_cast<std::uint64_t>(frames) * channels * sizeof(float);
    if (limits.max_output_bytes != 0u && bytes > limits.max_output_bytes) {
        report(fail, path, "limits.max_output_bytes",
               std::to_string(bytes) + " bytes decoded, ceiling " +
                   std::to_string(limits.max_output_bytes));
        ok = false;
    }
    return ok;
}

inline void check_agreement(const std::string& path, const pulp::audio::AudioFileInfo& info,
                            const pulp::audio::AudioFileData& data, const FailureSink& fail) {
    if (info.sample_rate != data.sample_rate) {
        report(fail, path, "sample_rate",
               "info " + std::to_string(info.sample_rate) + ", decoded " +
                   std::to_string(data.sample_rate));
    }
    if (info.num_channels != data.num_channels()) {
        report(fail, path, "num_channels",
               "info " + std::to_string(info.num_channels) + ", decoded " +
                   std::to_string(data.num_channels()));
    }
    // Zero is the documented "length not declared" value; a declared count
    // the decode does not deliver is the reader's refusal to make, not ours.
    if (info.num_frames != 0 && info.num_frames != data.num_frames()) {
        report(fail, path, "num_frames",
               "info " + std::to_string(info.num_frames) + ", decoded " +
                   std::to_string(data.num_frames()));
    }
}

} // namespace detail

// Runs read_info and read(limits) on `path`, dispatching on the extension
// through the global registry, and applies every property above, reporting
// each failure through `fail`. Never throws; the caller decides whether a
// failure traps (fuzzer) or fails a test case. A path no reader claims simply
// fails both reads, so the caller that needs "a reader exists" checks
// find_reader itself.
inline OracleOutcome check_registry(const std::string& path,
                                    const pulp::audio::DecodeLimits& limits,
                                    const FailureSink& fail) {
    auto& registry = pulp::audio::FormatRegistry::instance();
    OracleOutcome outcome;
    outcome.info = registry.read_info(path);
    outcome.info_ok = outcome.info.has_value();
    outcome.data = registry.read(path, limits);
    outcome.decode_ok = outcome.data.has_value();

    if (outcome.decode_ok) {
        const bool shape_ok = detail::check_decoded_shape(path, *outcome.data, limits, fail);
        if (shape_ok && outcome.info_ok)
            detail::check_agreement(path, *outcome.info, *outcome.data, fail);
    }
    return outcome;
}

} // namespace pulp_fuzz::audio_decoder
