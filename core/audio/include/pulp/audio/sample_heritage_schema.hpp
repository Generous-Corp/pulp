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

inline constexpr std::uint32_t kSampleHeritageProfileSchemaVersion = 3;
inline constexpr std::uint32_t kSampleHeritageProfileDigestVersion = 3;
inline constexpr std::uint32_t kSampleHeritageSpectralTiltLawVersion = 1;
inline constexpr std::size_t kSampleHeritageMaximumStages = 7;
inline constexpr std::size_t kSampleHeritageMaximumVoiceBlocks = 8;
inline constexpr std::size_t kSampleHeritageMaximumBusBlocks = 2;
inline constexpr std::size_t kSampleHeritageMaximumRecordCommitBlocks = 4;
inline constexpr std::size_t kSampleHeritageMaximumChannels = 8;
inline constexpr std::size_t kSampleHeritageMaximumProfileIdBytes = 63;

enum class SampleHeritageSeedPolicy : std::uint8_t {
    RestartFromProfileSeed,
    ContinueSerializedState,
};

enum class SampleHeritageBlockDomain : std::uint8_t { Voice, Bus, RecordCommit };
enum class SampleHeritagePitchFamily : std::uint8_t {
    VariableClock,
    DropRepeat,
    EarlyLinear,
};
enum class SampleHeritageConverterFamily : std::uint8_t {
    LinearPcm,
    MuLaw,
    ALaw,
};
enum class SampleHeritageHoldMode : std::uint8_t { ZeroOrder };
enum class SampleHeritageCutoffLaw : std::uint8_t { FixedHz, MachineRateRatio };
enum class SampleHeritageNoiseGate : std::uint8_t { AlwaysOn, VoiceActive };
enum class SampleHeritageReconstructionFamily : std::uint8_t {
    OnePole,
    Butterworth,
    Chebyshev,
    Elliptic,
};
enum class SampleHeritageRecordFilterFamily : std::uint8_t {
    OnePole,
    Butterworth,
    Chebyshev,
    Elliptic,
};

struct SampleHeritageVoiceMachineDomainBlock { double sample_rate = 0.0; };
struct SampleHeritageVoiceClockBlock { double ratio = 1.0; };
struct SampleHeritageVoicePitchBlock {
    SampleHeritagePitchFamily family = SampleHeritagePitchFamily::VariableClock;
};
struct SampleHeritageVoiceConverterBlock {
    SampleHeritageConverterFamily family = SampleHeritageConverterFamily::LinearPcm;
    float bit_depth = 16.0f;
    float dac_nonlinearity = 0.0f;
    float dither_lsb = 0.0f;
    std::uint64_t seed = 0;
    SampleHeritageSeedPolicy seed_policy =
        SampleHeritageSeedPolicy::RestartFromProfileSeed;
};
struct SampleHeritageVoiceHoldDroopBlock {
    SampleHeritageHoldMode mode = SampleHeritageHoldMode::ZeroOrder;
    std::uint32_t hold_samples = 1;
    float droop = 0.0f;
};
struct SampleHeritageVoiceReconstructionBlock {
    SampleHeritageReconstructionFamily family =
        SampleHeritageReconstructionFamily::OnePole;
    SampleHeritageCutoffLaw cutoff_law = SampleHeritageCutoffLaw::FixedHz;
    double cutoff_value = 0.0;
    std::uint8_t order = 1;
    float ripple_db = 0.0f;
    float stopband_attenuation_db = 0.0f;
};
struct SampleHeritageVoiceAnalogColorBlock {
    float drive = 1.0f;
    float asymmetry = 0.0f;
    float mix = 1.0f;
};
struct SampleHeritageVoiceLiveCyclicStretchBlock {
    double factor = 1.0;
    double cycle_ms = 1.0;
    double splice_ms = 0.0;
    bool stereo_link = true;
    std::uint16_t shuffle_divisions = 0;
    std::uint64_t seed = 0;
    SampleHeritageSeedPolicy seed_policy =
        SampleHeritageSeedPolicy::RestartFromProfileSeed;
};

using SampleHeritageVoiceBlockParameters =
    std::variant<SampleHeritageVoiceMachineDomainBlock,
    SampleHeritageVoiceClockBlock,
    SampleHeritageVoicePitchBlock,
    SampleHeritageVoiceConverterBlock,
    SampleHeritageVoiceLiveCyclicStretchBlock,
    SampleHeritageVoiceHoldDroopBlock,
    SampleHeritageVoiceReconstructionBlock,
    SampleHeritageVoiceAnalogColorBlock>;

struct SampleHeritageVoiceBlockSpec {
    SampleHeritageBlockDomain domain = SampleHeritageBlockDomain::Voice;
    bool bypass = true;
    SampleHeritageVoiceBlockParameters parameters =
        SampleHeritageVoiceMachineDomainBlock{};
};

struct SampleHeritageBusNoiseIdleBlock {
    float noise_amplitude = 0.0f;
    float idle_amplitude = 0.0f;
    float tilt_db_per_octave = 0.0f;
    SampleHeritageNoiseGate gate = SampleHeritageNoiseGate::AlwaysOn;
    std::uint64_t seed = 0;
    SampleHeritageSeedPolicy seed_policy =
        SampleHeritageSeedPolicy::RestartFromProfileSeed;
    // Tilt law version 1 is a deterministic cascade of unit-slope RBJ high
    // shelves. The lowest shelf starts at tilt_floor_hz. Successive shelves
    // span one octave, or the last partial octave through Nyquist, with a
    // geometric-mean center and tilt_db_per_octave times the octave width.
    // The cascade is normalized to 0 dB at tilt_reference_hz. The requested
    // slope is a target, not an exact power law. Below the floor, response
    // approaches the low-frequency asymptote rather than being hard-clamped.
    // Compatibility compares the prepared response; the ideal slope has no
    // normative error tolerance.
    double tilt_reference_hz = 1000.0;
    double tilt_floor_hz = 20.0;
};
struct SampleHeritageBusOutputDriveBlock {
    float drive = 1.0f;
    float ceiling = 1.0f;
};
using SampleHeritageBusBlockParameters =
    std::variant<SampleHeritageBusNoiseIdleBlock,
    SampleHeritageBusOutputDriveBlock>;
struct SampleHeritageBusBlockSpec {
    SampleHeritageBlockDomain domain = SampleHeritageBlockDomain::Bus;
    bool bypass = true;
    SampleHeritageBusBlockParameters parameters = SampleHeritageBusNoiseIdleBlock{};
};

struct SampleHeritageRecordInputDriveClipBlock {
    float drive = 1.0f;
    float clip_level = 1.0f;
};
struct SampleHeritageRecordRateBlock {
    SampleHeritageRecordFilterFamily filter_family =
        SampleHeritageRecordFilterFamily::OnePole;
    double sample_rate = 0.0;
    SampleHeritageCutoffLaw cutoff_law = SampleHeritageCutoffLaw::FixedHz;
    double cutoff_value = 0.0;
    std::uint8_t order = 1;
    float ripple_db = 0.0f;
    float stopband_attenuation_db = 0.0f;
};
struct SampleHeritageRecordConverterBlock {
    SampleHeritageConverterFamily family = SampleHeritageConverterFamily::LinearPcm;
    float bit_depth = 16.0f;
    float dac_nonlinearity = 0.0f;
    float dither_lsb = 0.0f;
    std::uint64_t seed = 0;
    SampleHeritageSeedPolicy seed_policy =
        SampleHeritageSeedPolicy::RestartFromProfileSeed;
};
struct SampleHeritageRecordCommitCyclicStretchBlock {
    double factor = 1.0;
    std::uint32_t cycle_samples = 1;
    std::uint32_t crossfade_samples = 0;
    std::uint64_t zone_start_frame = 0;
    std::uint64_t zone_end_frame = 0;
};
struct SampleHeritageRecordCommitAdaptiveStretchBlock {
    double factor = 1.0;
    std::uint32_t decision_hop_samples = 1;
    std::uint32_t search_radius_samples = 0;
    std::uint32_t search_stride_samples = 1;
    std::uint32_t crossfade_samples = 0;
    std::uint64_t zone_start_frame = 0;
    std::uint64_t zone_end_frame = 0;
    bool stereo_link = true;
};
using SampleHeritageRecordCommitBlockParameters =
    std::variant<SampleHeritageRecordInputDriveClipBlock,
    SampleHeritageRecordRateBlock,
    SampleHeritageRecordConverterBlock, SampleHeritageRecordCommitCyclicStretchBlock,
                 SampleHeritageRecordCommitAdaptiveStretchBlock>;
struct SampleHeritageRecordCommitBlockSpec {
    SampleHeritageBlockDomain domain = SampleHeritageBlockDomain::RecordCommit;
    bool bypass = true;
    SampleHeritageRecordCommitBlockParameters parameters =
        SampleHeritageRecordInputDriveClipBlock{};
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

using SampleHeritageStageParameters =
    std::variant<SampleHeritageMachineDomainStage,
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
    std::vector<SampleHeritageVoiceBlockSpec> voice;
    std::vector<SampleHeritageBusBlockSpec> bus;
    std::vector<SampleHeritageRecordCommitBlockSpec> record_commit;

    // Flat stages are an in-memory engine representation and never cross the
    // profile JSON boundary.
    std::vector<SampleHeritageStageSpec> stages;
};

struct SampleHeritagePreparedProfile {
    std::uint32_t schema_version = 0;
    std::array<char, kSampleHeritageMaximumProfileIdBytes + 1> profile_id{};
    double host_sample_rate = 0.0;
    std::array<SampleHeritageVoiceBlockSpec, kSampleHeritageMaximumVoiceBlocks>
        voice{};
    std::size_t voice_count = 0;
    std::array<SampleHeritageBusBlockSpec, kSampleHeritageMaximumBusBlocks> bus{};
    std::size_t bus_count = 0;
    std::array<SampleHeritageRecordCommitBlockSpec,
               kSampleHeritageMaximumRecordCommitBlocks> record_commit{};
    std::size_t record_commit_count = 0;
    // The post-mix engine consumes flat stages; profile JSON does not.
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
    TooManyBlocks,
    WrongBlockDomain,
    MixedLegacyAndTypedBlocks,
    NonCanonicalProfileRepresentation,
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
    SampleHeritageBlockDomain block_domain = SampleHeritageBlockDomain::Voice;
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

template <typename Stage>
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
std::array<std::uint8_t, 32> sample_heritage_profile_digest(const SampleHeritageProfile& profile);

inline SampleHeritageProfileValidation
validate_sample_heritage_profile(const SampleHeritageProfile& source) noexcept {
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
    const bool has_typed_blocks = !source.voice.empty() || !source.bus.empty() ||
                                  !source.record_commit.empty();
    if (has_typed_blocks && !source.stages.empty()) {
        result.status = SampleHeritageProfileStatus::MixedLegacyAndTypedBlocks;
        return result;
    }
    if (source.stages.size() > kSampleHeritageMaximumStages) {
        result.status = SampleHeritageProfileStatus::TooManyStages;
        return result;
    }
    if (source.voice.size() > kSampleHeritageMaximumVoiceBlocks ||
        source.bus.size() > kSampleHeritageMaximumBusBlocks ||
        source.record_commit.size() > kSampleHeritageMaximumRecordCommitBlocks) {
        result.status = SampleHeritageProfileStatus::TooManyBlocks;
        return result;
    }

    if (has_typed_blocks) {
        const auto finite_range = [](auto value, auto minimum, auto maximum) {
            return std::isfinite(value) && value >= minimum && value <= maximum;
        };
        const auto valid_seed_policy = [](SampleHeritageSeedPolicy policy) {
            return policy == SampleHeritageSeedPolicy::RestartFromProfileSeed ||
                   policy == SampleHeritageSeedPolicy::ContinueSerializedState;
        };
        const auto validate_seed = [&](float amount, std::uint64_t seed,
                                       SampleHeritageSeedPolicy policy) {
            return valid_seed_policy(policy) &&
                   !((amount > 0.0f ||
                      policy == SampleHeritageSeedPolicy::ContinueSerializedState) &&
                     seed == 0);
        };
        double voice_machine_rate = source.host_sample_rate;
        for (const auto& spec : source.voice) {
            if (spec.bypass) continue;
            if (const auto* machine =
                    std::get_if<SampleHeritageVoiceMachineDomainBlock>(&spec.parameters))
                voice_machine_rate = machine->sample_rate;
        }
        const auto validate_cutoff = [&](SampleHeritageCutoffLaw law,
                                         double value,
                                         double sample_rate) {
            if (law == SampleHeritageCutoffLaw::MachineRateRatio)
                return std::isfinite(value) && value > 0.0 && value < 0.5;
            return law == SampleHeritageCutoffLaw::FixedHz &&
                   std::isfinite(value) && value >= 1.0 &&
                   std::isfinite(sample_rate) && value < sample_rate * 0.5;
        };
        const auto validate_ordered = [&](const auto& blocks,
                                          SampleHeritageBlockDomain domain,
                                          auto validate_block) {
            std::array<bool, std::variant_size_v<std::decay_t<decltype(blocks.front().parameters)>>> seen{};
            std::size_t previous = 0;
            bool have_previous = false;
            for (std::size_t index = 0; index < blocks.size(); ++index) {
                result.stage_index = index;
                result.block_domain = domain;
                if (blocks[index].domain != domain)
                    return SampleHeritageProfileStatus::WrongBlockDomain;
                const auto type = blocks[index].parameters.index();
                if (std::exchange(seen[type], true))
                    return SampleHeritageProfileStatus::DuplicateStage;
                if (have_previous && type < previous)
                    return SampleHeritageProfileStatus::InvalidStageOrder;
                previous = type;
                have_previous = true;
                if (!validate_block(blocks[index].parameters))
                    return SampleHeritageProfileStatus::InvalidStageParameter;
            }
            return SampleHeritageProfileStatus::Ok;
        };

        auto status = validate_ordered(
            source.voice, SampleHeritageBlockDomain::Voice,
            [&](const SampleHeritageVoiceBlockParameters& parameters) {
                return std::visit(
                    [&](const auto& block) {
                    using Block = std::decay_t<decltype(block)>;
                    if constexpr (std::is_same_v<Block,
                                                  SampleHeritageVoiceMachineDomainBlock>)
                        return finite_range(block.sample_rate, 8000.0, 384000.0);
                    else if constexpr (std::is_same_v<Block,
                                                       SampleHeritageVoiceClockBlock>)
                        return finite_range(block.ratio, 0.015625, 64.0);
                    else if constexpr (std::is_same_v<Block,
                                                       SampleHeritageVoicePitchBlock>)
                        return block.family == SampleHeritagePitchFamily::VariableClock ||
                               block.family == SampleHeritagePitchFamily::DropRepeat ||
                               block.family == SampleHeritagePitchFamily::EarlyLinear;
                    else if constexpr (std::is_same_v<Block,
                                                       SampleHeritageVoiceConverterBlock>)
                        return (block.family == SampleHeritageConverterFamily::LinearPcm ||
                                block.family == SampleHeritageConverterFamily::MuLaw ||
                                block.family == SampleHeritageConverterFamily::ALaw) &&
                               finite_range(block.bit_depth, 4.0f, 16.0f) &&
                               finite_range(block.dac_nonlinearity, 0.0f, 1.0f) &&
                               finite_range(block.dither_lsb, 0.0f, 2.0f) &&
                               validate_seed(block.dither_lsb, block.seed,
                                             block.seed_policy);
                    else if constexpr (std::is_same_v<Block,
                                                       SampleHeritageVoiceHoldDroopBlock>)
                        return block.mode == SampleHeritageHoldMode::ZeroOrder &&
                               block.hold_samples >= 1 && block.hold_samples <= 65536 &&
                               finite_range(block.droop, 0.0f, 1.0f);
                    else if constexpr (std::is_same_v<Block,
                                                       SampleHeritageVoiceReconstructionBlock>)
                        return validate_cutoff(block.cutoff_law, block.cutoff_value,
                                               voice_machine_rate) &&
                               ((block.family ==
                                     SampleHeritageReconstructionFamily::OnePole &&
                                 block.order == 1 && block.ripple_db == 0.0f &&
                                 block.stopband_attenuation_db == 0.0f) ||
                                (block.family ==
                                     SampleHeritageReconstructionFamily::Butterworth &&
                                 block.order >= 2 && block.order <= 16 &&
                                 (block.order & 1u) == 0 &&
                                 block.ripple_db == 0.0f &&
                                 block.stopband_attenuation_db == 0.0f) ||
                                (block.family ==
                                     SampleHeritageReconstructionFamily::Chebyshev &&
                                 block.order >= 2 && block.order <= 16 &&
                                 (block.order & 1u) == 0 &&
                                 finite_range(block.ripple_db, 0.0f, 12.0f) &&
                                 block.ripple_db > 0.0f &&
                                 block.stopband_attenuation_db == 0.0f) ||
                                (block.family ==
                                     SampleHeritageReconstructionFamily::Elliptic &&
                                 block.order >= 2 && block.order <= 16 &&
                                 (block.order & 1u) == 0 &&
                                 finite_range(block.ripple_db, 0.0f, 12.0f) &&
                                 block.ripple_db > 0.0f &&
                                 finite_range(block.stopband_attenuation_db,
                                              block.ripple_db, 180.0f) &&
                                 block.stopband_attenuation_db > block.ripple_db));
                    else if constexpr (std::is_same_v<Block,
                                                       SampleHeritageVoiceAnalogColorBlock>)
                        return finite_range(block.drive, 0.0f, 16.0f) &&
                               finite_range(block.asymmetry, -1.0f, 1.0f) &&
                               finite_range(block.mix, 0.0f, 1.0f);
                    else
                        return finite_range(block.factor, 0.25, 20.0) &&
                               std::isfinite(block.cycle_ms) && block.cycle_ms > 0.0 &&
                               block.cycle_ms * voice_machine_rate * 0.001 <=
                                   1048576.0 &&
                               finite_range(block.splice_ms, 0.0, 20.0) &&
                               block.splice_ms <= block.cycle_ms * 0.5 &&
                               (block.shuffle_divisions == 0 ||
                                (block.shuffle_divisions >= 2 &&
                                 block.shuffle_divisions <= 64)) &&
                               valid_seed_policy(block.seed_policy) &&
                               !((block.shuffle_divisions != 0 ||
                                  block.seed_policy ==
                                      SampleHeritageSeedPolicy::ContinueSerializedState) &&
                                 block.seed == 0);
                }, parameters);
            });
        if (status == SampleHeritageProfileStatus::Ok)
            status = validate_ordered(
                source.bus, SampleHeritageBlockDomain::Bus,
                [&](const SampleHeritageBusBlockParameters& parameters) {
                    return std::visit(
                        [&](const auto& block) {
                        using Block = std::decay_t<decltype(block)>;
                        if constexpr (std::is_same_v<Block,
                                                      SampleHeritageBusNoiseIdleBlock>)
                            return finite_range(block.noise_amplitude, 0.0f, 1.0f) &&
                                   finite_range(block.idle_amplitude, 0.0f, 1.0f) &&
                                   finite_range(block.tilt_db_per_octave, -24.0f,
                                                24.0f) &&
                                   std::isfinite(block.tilt_floor_hz) &&
                                   block.tilt_floor_hz >= 1.0 &&
                                   std::isfinite(block.tilt_reference_hz) &&
                                   block.tilt_reference_hz >= block.tilt_floor_hz &&
                                   block.tilt_reference_hz <
                                       source.host_sample_rate * 0.5 &&
                                   (block.gate == SampleHeritageNoiseGate::AlwaysOn ||
                                    block.gate ==
                                        SampleHeritageNoiseGate::VoiceActive) &&
                                       validate_seed(
                                           std::max(block.noise_amplitude,
                                                          block.idle_amplitude),
                                                 block.seed, block.seed_policy);
                        else
                            return finite_range(block.drive, 0.0f, 16.0f) &&
                                   finite_range(block.ceiling, 0.001f, 4.0f);
                    }, parameters);
                });
        double record_processing_rate = source.host_sample_rate;
        if (status == SampleHeritageProfileStatus::Ok)
            status = validate_ordered(
                source.record_commit, SampleHeritageBlockDomain::RecordCommit,
                [&](const SampleHeritageRecordCommitBlockParameters& parameters) {
                    return std::visit(
                        [&](const auto& block) {
                        using Block = std::decay_t<decltype(block)>;
                        if constexpr (std::is_same_v<Block,
                                                      SampleHeritageRecordInputDriveClipBlock>)
                            return finite_range(block.drive, 0.0f, 16.0f) &&
                                   finite_range(block.clip_level, 0.001f, 4.0f);
                        else if constexpr (std::is_same_v<Block,
                                                           SampleHeritageRecordRateBlock>) {
                            const auto valid =
                                   finite_range(block.sample_rate, 8000.0,
                                                384000.0) &&
                                   validate_cutoff(block.cutoff_law,
                                                   block.cutoff_value,
                                                   record_processing_rate) &&
                                   ((block.filter_family ==
                                         SampleHeritageRecordFilterFamily::OnePole &&
                                     block.order == 1 && block.ripple_db == 0.0f &&
                                     block.stopband_attenuation_db == 0.0f) ||
                                    (block.filter_family ==
                                         SampleHeritageRecordFilterFamily::Butterworth &&
                                     block.order >= 2 && block.order <= 16 &&
                                     (block.order & 1u) == 0 &&
                                     block.ripple_db == 0.0f &&
                                     block.stopband_attenuation_db == 0.0f) ||
                                    (block.filter_family ==
                                         SampleHeritageRecordFilterFamily::Chebyshev &&
                                     block.order >= 2 && block.order <= 16 &&
                                     (block.order & 1u) == 0 &&
                                     finite_range(block.ripple_db, 0.0f, 12.0f) &&
                                     block.ripple_db > 0.0f &&
                                     block.stopband_attenuation_db == 0.0f) ||
                                    (block.filter_family ==
                                         SampleHeritageRecordFilterFamily::Elliptic &&
                                     block.order >= 2 && block.order <= 16 &&
                                     (block.order & 1u) == 0 &&
                                     finite_range(block.ripple_db, 0.0f, 12.0f) &&
                                     block.ripple_db > 0.0f &&
                                     finite_range(block.stopband_attenuation_db,
                                                  block.ripple_db, 180.0f) &&
                                     block.stopband_attenuation_db >
                                         block.ripple_db));
                            if (valid &&
                                !source.record_commit[result.stage_index].bypass)
                                record_processing_rate = block.sample_rate;
                            return valid;
                        }
                        else if constexpr (std::is_same_v<Block,
                                                           SampleHeritageRecordConverterBlock>)
                            return (block.family == SampleHeritageConverterFamily::LinearPcm ||
                                    block.family == SampleHeritageConverterFamily::MuLaw ||
                                    block.family == SampleHeritageConverterFamily::ALaw) &&
                                   finite_range(block.bit_depth, 4.0f, 16.0f) &&
                                   finite_range(block.dac_nonlinearity, 0.0f, 1.0f) &&
                                   finite_range(block.dither_lsb, 0.0f, 2.0f) &&
                                   validate_seed(block.dither_lsb, block.seed,
                                                 block.seed_policy);
                        else if constexpr (std::is_same_v<
                                                   Block,
                                                   SampleHeritageRecordCommitCyclicStretchBlock>)
                            return
                                   finite_range(block.factor, 0.25, 20.0) &&
                                   block.cycle_samples >= 1 &&
                                   block.cycle_samples <= 1048576 &&
                                       block.crossfade_samples <= block.cycle_samples / 2 &&
                                       ((block.zone_start_frame == 0 &&
                                         block.zone_end_frame == 0) ||
                                        block.zone_start_frame < block.zone_end_frame);
                            else
                                return finite_range(block.factor, 0.25, 20.0) &&
                                       block.decision_hop_samples >= 1 &&
                                       block.decision_hop_samples <= 1048576 &&
                                       block.search_radius_samples <= 1048576 &&
                                       block.search_stride_samples >= 1 &&
                                       block.search_stride_samples <= 1048576 &&
                                       block.crossfade_samples <= 524288 &&
                                       block.crossfade_samples <= block.decision_hop_samples &&
                                   ((block.zone_start_frame == 0 &&
                                     block.zone_end_frame == 0) ||
                                    block.zone_start_frame < block.zone_end_frame);
                    }, parameters);
                });
        if (status != SampleHeritageProfileStatus::Ok) {
            result.status = status;
            return result;
        }

        result.profile.schema_version = source.schema_version;
        std::copy(source.profile_id.begin(), source.profile_id.end(),
                  result.profile.profile_id.begin());
        result.profile.host_sample_rate =
            source.host_sample_rate == 0.0 ? 0.0 : source.host_sample_rate;
        result.profile.voice_count = source.voice.size();
        result.profile.bus_count = source.bus.size();
        result.profile.record_commit_count = source.record_commit.size();
        std::copy(source.voice.begin(), source.voice.end(), result.profile.voice.begin());
        std::copy(source.bus.begin(), source.bus.end(), result.profile.bus.begin());
        std::copy(source.record_commit.begin(), source.record_commit.end(),
                  result.profile.record_commit.begin());
        try {
            result.profile.profile_digest = sample_heritage_profile_digest(source);
        } catch (...) {
            result.status = SampleHeritageProfileStatus::DigestUnavailable;
            return result;
        }
        result.status = SampleHeritageProfileStatus::Ok;
        result.stage_index = source.record_commit.size();
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
        std::visit(
            [&](auto& stage) noexcept {
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
