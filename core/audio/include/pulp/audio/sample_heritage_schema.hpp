#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace pulp::audio {

inline constexpr std::uint32_t kSampleHeritageProfileSchemaVersion = 2;
inline constexpr std::uint32_t kSampleHeritageProfileDigestVersion = 2;
inline constexpr std::size_t kSampleHeritageMaximumStages = 7;
inline constexpr std::size_t kSampleHeritageMaximumChannels = 8;
inline constexpr std::size_t kSampleHeritageMaximumProfileIdBytes = 63;

enum class SampleHeritageSeedPolicy : std::uint8_t {
    RestartFromProfileSeed,
    ContinueSerializedState,
};

struct SampleHeritageMachineDomainStage {
    double sample_rate = 0.0;
};

struct SampleHeritageQuantizationStage {
    std::uint8_t bit_depth = 0;
    float dither_lsb = 0.0f;
    std::uint64_t seed = 0;
    SampleHeritageSeedPolicy seed_policy =
        SampleHeritageSeedPolicy::RestartFromProfileSeed;
};

struct SampleHeritageClockPitchStage {
    double ratio = 1.0;
};

struct SampleHeritageDacHoldStage {
    std::uint32_t hold_samples = 1;
};

struct SampleHeritageReconstructionFilterStage {
    double cutoff_hz = 0.0;
};

struct SampleHeritageNoiseStage {
    float amplitude = 0.0f;
    std::uint64_t seed = 0;
    SampleHeritageSeedPolicy seed_policy =
        SampleHeritageSeedPolicy::RestartFromProfileSeed;
};

struct SampleHeritageOutputStage {
    float gain = 1.0f;
};

using SampleHeritageStageParameters = std::variant<
    SampleHeritageMachineDomainStage,
    SampleHeritageQuantizationStage,
    SampleHeritageClockPitchStage,
    SampleHeritageDacHoldStage,
    SampleHeritageReconstructionFilterStage,
    SampleHeritageNoiseStage,
    SampleHeritageOutputStage>;

struct SampleHeritageStageSpec {
    bool bypass = true;
    SampleHeritageStageParameters parameters = SampleHeritageOutputStage{};
};

/// Authoring/serialization schema. Construct and validate this off the audio
/// thread; the callback consumes only SampleHeritagePreparedProfile.
struct SampleHeritageProfile {
    std::uint32_t schema_version = kSampleHeritageProfileSchemaVersion;
    std::string profile_id;
    double host_sample_rate = 0.0;
    std::vector<SampleHeritageStageSpec> stages;
};

struct SampleHeritagePreparedProfile {
    std::uint32_t schema_version = 0;
    std::array<char, kSampleHeritageMaximumProfileIdBytes + 1> profile_id{};
    double host_sample_rate = 0.0;
    std::array<SampleHeritageStageSpec, kSampleHeritageMaximumStages> stages{};
    std::size_t stage_count = 0;
    std::array<std::uint8_t, 32> profile_digest{};

    std::string_view id() const noexcept {
        const auto end = std::find(profile_id.begin(), profile_id.end(), '\0');
        return {profile_id.data(),
                static_cast<std::size_t>(std::distance(profile_id.begin(), end))};
    }
};

enum class SampleHeritageProfileStatus : std::uint8_t {
    Ok,
    UnsupportedSchemaVersion,
    InvalidProfileId,
    InvalidHostSampleRate,
    TooManyStages,
    DuplicateStage,
    InvalidStageOrder,
    InvalidStageParameter,
    UnsupportedRateConversion,
    DigestUnavailable,
};

struct SampleHeritageProfileValidation {
    SampleHeritageProfileStatus status =
        SampleHeritageProfileStatus::UnsupportedSchemaVersion;
    std::size_t stage_index = 0;
    SampleHeritagePreparedProfile profile{};

    bool valid() const noexcept { return status == SampleHeritageProfileStatus::Ok; }
};

namespace detail {

inline bool valid_neutral_profile_id(std::string_view id) noexcept {
    constexpr std::string_view prefix = "neutral.";
    if (!id.starts_with(prefix) || id.size() <= prefix.size() ||
        id.size() > kSampleHeritageMaximumProfileIdBytes) {
        return false;
    }
    bool previous_separator = true;
    for (std::size_t index = prefix.size(); index < id.size(); ++index) {
        const char character = id[index];
        const bool alpha = character >= 'a' && character <= 'z';
        const bool digit = character >= '0' && character <= '9';
        const bool separator = character == '.' || character == '-';
        if (!alpha && !digit && !separator) return false;
        if (separator && previous_separator) return false;
        previous_separator = separator;
    }
    return !previous_separator;
}

template<typename Stage>
inline constexpr std::size_t stage_type_index = [] {
    if constexpr (std::is_same_v<Stage, SampleHeritageMachineDomainStage>) return 0u;
    if constexpr (std::is_same_v<Stage, SampleHeritageQuantizationStage>) return 1u;
    if constexpr (std::is_same_v<Stage, SampleHeritageClockPitchStage>) return 2u;
    if constexpr (std::is_same_v<Stage, SampleHeritageDacHoldStage>) return 3u;
    if constexpr (std::is_same_v<Stage, SampleHeritageReconstructionFilterStage>) return 4u;
    if constexpr (std::is_same_v<Stage, SampleHeritageNoiseStage>) return 5u;
    return 6u;
}();

}  // namespace detail

/// SHA-256 over a versioned, domain-separated, little-endian canonical byte
/// encoding of the authoring profile. This allocates and is strictly an
/// off-audio-thread API.
std::array<std::uint8_t, 32> sample_heritage_profile_digest(
    const SampleHeritageProfile& profile);

inline SampleHeritageProfileValidation validate_sample_heritage_profile(
    const SampleHeritageProfile& source) noexcept {
    SampleHeritageProfileValidation result;
    if (source.schema_version != kSampleHeritageProfileSchemaVersion) {
        result.status = SampleHeritageProfileStatus::UnsupportedSchemaVersion;
        return result;
    }
    if (!detail::valid_neutral_profile_id(source.profile_id)) {
        result.status = SampleHeritageProfileStatus::InvalidProfileId;
        return result;
    }
    if (!(source.host_sample_rate >= 8000.0 && source.host_sample_rate <= 384000.0) ||
        !std::isfinite(source.host_sample_rate)) {
        result.status = SampleHeritageProfileStatus::InvalidHostSampleRate;
        return result;
    }
    if (source.stages.size() > kSampleHeritageMaximumStages) {
        result.status = SampleHeritageProfileStatus::TooManyStages;
        return result;
    }

    double machine_sample_rate = source.host_sample_rate;
    double clock_ratio = 1.0;
    for (const auto& spec : source.stages) {
        if (spec.bypass) continue;
        if (const auto* machine =
                std::get_if<SampleHeritageMachineDomainStage>(&spec.parameters)) {
            machine_sample_rate = machine->sample_rate;
        } else if (const auto* clock =
                       std::get_if<SampleHeritageClockPitchStage>(&spec.parameters)) {
            clock_ratio = clock->ratio;
        }
    }
    const auto clocked_sample_rate = machine_sample_rate * clock_ratio;

    std::array<bool, kSampleHeritageMaximumStages> seen{};
    for (std::size_t index = 0; index < source.stages.size(); ++index) {
        const auto type = std::visit(
            [](const auto& stage) noexcept {
                using Stage = std::decay_t<decltype(stage)>;
                return detail::stage_type_index<Stage>;
            },
            source.stages[index].parameters);
        if (std::exchange(seen[type], true)) {
            result.status = SampleHeritageProfileStatus::DuplicateStage;
            result.stage_index = index;
            return result;
        }
    }

    std::size_t previous_type = 0;
    bool have_previous_type = false;
    for (std::size_t index = 0; index < source.stages.size(); ++index) {
        result.stage_index = index;
        const auto status = std::visit(
            [&](const auto& stage) noexcept {
                using Stage = std::decay_t<decltype(stage)>;
                constexpr auto type = detail::stage_type_index<Stage>;
                if (have_previous_type && type < previous_type)
                    return SampleHeritageProfileStatus::InvalidStageOrder;
                previous_type = type;
                have_previous_type = true;
                if constexpr (std::is_same_v<Stage, SampleHeritageMachineDomainStage>) {
                    if (!(stage.sample_rate >= 8000.0 && stage.sample_rate <= 384000.0) ||
                        !std::isfinite(stage.sample_rate))
                        return SampleHeritageProfileStatus::InvalidStageParameter;
                } else if constexpr (std::is_same_v<Stage,
                                                    SampleHeritageQuantizationStage>) {
                    if (stage.bit_depth < 2 || stage.bit_depth > 24 ||
                        !(stage.dither_lsb >= 0.0f && stage.dither_lsb <= 2.0f) ||
                        !std::isfinite(stage.dither_lsb) ||
                        (stage.seed_policy !=
                             SampleHeritageSeedPolicy::RestartFromProfileSeed &&
                         stage.seed_policy !=
                             SampleHeritageSeedPolicy::ContinueSerializedState) ||
                        ((stage.dither_lsb > 0.0f ||
                          stage.seed_policy ==
                              SampleHeritageSeedPolicy::ContinueSerializedState) &&
                         stage.seed == 0))
                        return SampleHeritageProfileStatus::InvalidStageParameter;
                } else if constexpr (std::is_same_v<Stage, SampleHeritageClockPitchStage>) {
                    if (!(stage.ratio > 0.0) || !std::isfinite(stage.ratio))
                        return SampleHeritageProfileStatus::InvalidStageParameter;
                } else if constexpr (std::is_same_v<Stage, SampleHeritageDacHoldStage>) {
                    if (stage.hold_samples == 0 || stage.hold_samples > 65536)
                        return SampleHeritageProfileStatus::InvalidStageParameter;
                } else if constexpr (std::is_same_v<Stage,
                                                    SampleHeritageReconstructionFilterStage>) {
                    if (!(stage.cutoff_hz > 0.0 &&
                          stage.cutoff_hz < clocked_sample_rate * 0.5) ||
                        !std::isfinite(stage.cutoff_hz))
                        return SampleHeritageProfileStatus::InvalidStageParameter;
                } else if constexpr (std::is_same_v<Stage, SampleHeritageNoiseStage>) {
                    if (!(stage.amplitude >= 0.0f && stage.amplitude <= 1.0f) ||
                        !std::isfinite(stage.amplitude) ||
                        (stage.seed_policy !=
                             SampleHeritageSeedPolicy::RestartFromProfileSeed &&
                         stage.seed_policy !=
                             SampleHeritageSeedPolicy::ContinueSerializedState) ||
                        ((stage.amplitude > 0.0f ||
                          stage.seed_policy ==
                              SampleHeritageSeedPolicy::ContinueSerializedState) &&
                         stage.seed == 0))
                        return SampleHeritageProfileStatus::InvalidStageParameter;
                } else if constexpr (std::is_same_v<Stage, SampleHeritageOutputStage>) {
                    if (!(stage.gain >= 0.0f && stage.gain <= 16.0f) ||
                        !std::isfinite(stage.gain))
                        return SampleHeritageProfileStatus::InvalidStageParameter;
                }
                return SampleHeritageProfileStatus::Ok;
            },
            source.stages[index].parameters);
        if (status != SampleHeritageProfileStatus::Ok) {
            result.status = status;
            return result;
        }
    }

    const auto host_to_machine = source.host_sample_rate / machine_sample_rate;
    const auto clocked_to_host = clocked_sample_rate / source.host_sample_rate;
    if (!(host_to_machine > 0.0 && host_to_machine <= 128.0) ||
        !(clocked_to_host > 0.0 && clocked_to_host <= 128.0) ||
        !std::isfinite(host_to_machine) || !std::isfinite(clocked_to_host)) {
        result.status = SampleHeritageProfileStatus::UnsupportedRateConversion;
        result.stage_index = source.stages.size();
        return result;
    }

    result.profile.schema_version = source.schema_version;
    std::copy(source.profile_id.begin(), source.profile_id.end(),
              result.profile.profile_id.begin());
    const auto canonical_zero = []<typename Number>(Number value) noexcept {
        return value == Number{0} ? Number{0} : value;
    };
    result.profile.host_sample_rate = canonical_zero(source.host_sample_rate);
    result.profile.stage_count = source.stages.size();
    std::copy(source.stages.begin(), source.stages.end(), result.profile.stages.begin());
    for (std::size_t index = 0; index < result.profile.stage_count; ++index) {
        std::visit([&](auto& stage) noexcept {
            using Stage = std::decay_t<decltype(stage)>;
            if constexpr (std::is_same_v<Stage, SampleHeritageMachineDomainStage>)
                stage.sample_rate = canonical_zero(stage.sample_rate);
            else if constexpr (std::is_same_v<Stage, SampleHeritageQuantizationStage>)
                stage.dither_lsb = canonical_zero(stage.dither_lsb);
            else if constexpr (std::is_same_v<Stage, SampleHeritageClockPitchStage>)
                stage.ratio = canonical_zero(stage.ratio);
            else if constexpr (std::is_same_v<Stage,
                                              SampleHeritageReconstructionFilterStage>)
                stage.cutoff_hz = canonical_zero(stage.cutoff_hz);
            else if constexpr (std::is_same_v<Stage, SampleHeritageNoiseStage>)
                stage.amplitude = canonical_zero(stage.amplitude);
            else if constexpr (std::is_same_v<Stage, SampleHeritageOutputStage>)
                stage.gain = canonical_zero(stage.gain);
        }, result.profile.stages[index].parameters);
    }
    try {
        result.profile.profile_digest = sample_heritage_profile_digest(source);
    } catch (...) {
        result.status = SampleHeritageProfileStatus::DigestUnavailable;
        return result;
    }
    result.status = SampleHeritageProfileStatus::Ok;
    result.stage_index = source.stages.size();
    return result;
}

}  // namespace pulp::audio
