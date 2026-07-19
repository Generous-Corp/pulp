#pragma once

#include <pulp/audio/sample_heritage_json.hpp>

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace pulp::examples {

inline constexpr std::uint32_t kSamplerHeritageStateMagic = 0x53485350u; // PSHS
inline constexpr std::uint16_t kSamplerHeritageStateVersion = 2;
inline constexpr std::uint16_t kSamplerHeritageStateHasRuntime = 1u << 0;
inline constexpr std::size_t kSamplerHeritageStateV1HeaderBytes = 16;
inline constexpr std::size_t kSamplerHeritageStateHeaderBytes = 24;
inline constexpr std::size_t kSamplerHeritageMaximumProfileJsonBytes = 16 * 1024;
inline constexpr std::size_t kSamplerHeritageMaximumRuntimeJsonBytes = 8 * 1024;

enum class SamplerHeritageStateStatus : std::uint8_t {
    Ok,
    BadMagic,
    UnsupportedVersion,
    UnsupportedFlags,
    Truncated,
    LengthOutOfRange,
    TrailingBytes,
    InvalidProfileJson,
    NonCanonicalProfileJson,
    InvalidRuntimeJson,
    NonCanonicalRuntimeJson,
    RuntimeProfileMismatch,
    RuntimeStageLayoutMismatch,
    AllocationFailure,
};

struct SamplerHeritagePersistentState {
    bool enabled = false;
    audio::SampleHeritageProfile profile;
    bool has_runtime_state = false;
    audio::SampleHeritageRuntimeState runtime_state{};
    double runtime_host_sample_rate = 0.0;
};

struct SamplerHeritageStateParseResult {
    SamplerHeritageStateStatus status = SamplerHeritageStateStatus::Truncated;
    SamplerHeritagePersistentState state{};

    bool valid() const noexcept { return status == SamplerHeritageStateStatus::Ok; }
};

struct SamplerHeritageStateWriteResult {
    SamplerHeritageStateStatus status =
        SamplerHeritageStateStatus::InvalidProfileJson;
    std::vector<std::uint8_t> bytes;

    bool valid() const noexcept { return status == SamplerHeritageStateStatus::Ok; }
};

namespace sampler_heritage_state_detail {

inline std::uint16_t read_u16(std::span<const std::uint8_t> bytes,
                              std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(bytes[offset]) |
           static_cast<std::uint16_t>(bytes[offset + 1] << 8);
}

inline std::uint32_t read_u32(std::span<const std::uint8_t> bytes,
                              std::size_t offset) noexcept {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

inline void append_u16(std::vector<std::uint8_t>& bytes,
                       std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8));
}

inline void append_u32(std::vector<std::uint8_t>& bytes,
                       std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8));
    bytes.push_back(static_cast<std::uint8_t>(value >> 16));
    bytes.push_back(static_cast<std::uint8_t>(value >> 24));
}

inline double read_f64(std::span<const std::uint8_t> bytes,
                       std::size_t offset) noexcept {
    std::uint64_t bits = 0;
    for (std::size_t index = 0; index < sizeof(bits); ++index)
        bits |= static_cast<std::uint64_t>(bytes[offset + index]) << (index * 8);
    return std::bit_cast<double>(bits);
}

inline void append_f64(std::vector<std::uint8_t>& bytes, double value) {
    auto bits = std::bit_cast<std::uint64_t>(value);
    for (std::size_t index = 0; index < sizeof(bits); ++index) {
        bytes.push_back(static_cast<std::uint8_t>(bits & 0xffu));
        bits >>= 8;
    }
}

inline bool runtime_matches_profile(
    const audio::SampleHeritageRuntimeState& runtime,
    const audio::SampleHeritageProfileValidation& validation) noexcept {
    return runtime.profile_schema_version == validation.profile.schema_version &&
           runtime.bound_profile_id() == validation.profile.id() &&
           runtime.profile_digest_version ==
               audio::kSampleHeritageProfileDigestVersion &&
           runtime.profile_digest == validation.profile.profile_digest;
}

inline bool runtime_layout_matches_profile(
    const audio::SampleHeritageRuntimeState& runtime,
    const audio::SampleHeritageProfile& profile) noexcept {
    std::size_t expected = 0;
    for (std::size_t index = 0; index < profile.stages.size(); ++index) {
        const auto expected_type = std::visit(
            [](const auto& stage)
                -> std::pair<bool, audio::SampleHeritageRuntimeRngStageType> {
                using Stage = std::decay_t<decltype(stage)>;
                if constexpr (std::is_same_v<
                                  Stage, audio::SampleHeritageQuantizationStage>) {
                    return {stage.seed_policy ==
                                audio::SampleHeritageSeedPolicy::ContinueSerializedState,
                            audio::SampleHeritageRuntimeRngStageType::Quantization};
                }
                if constexpr (std::is_same_v<Stage,
                                             audio::SampleHeritageNoiseStage>) {
                    return {stage.seed_policy ==
                                audio::SampleHeritageSeedPolicy::ContinueSerializedState,
                            audio::SampleHeritageRuntimeRngStageType::Noise};
                }
                return {false,
                        audio::SampleHeritageRuntimeRngStageType::Quantization};
            },
            profile.stages[index].parameters);
        if (!expected_type.first) continue;
        if (expected >= runtime.rng_state_count) return false;
        const auto& saved = runtime.rng_states[expected];
        if (saved.stage_index != index || saved.stage_type != expected_type.second)
            return false;
        ++expected;
    }
    return expected == runtime.rng_state_count;
}

inline std::string_view as_string(std::span<const std::uint8_t> bytes) noexcept {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

}  // namespace sampler_heritage_state_detail

/// Serialize the sampler-owned heritage profile and, when present, the bounded
/// ContinueSerializedState RNG snapshot. This allocating API is off-audio-thread.
inline SamplerHeritageStateWriteResult write_sampler_heritage_state(
    const SamplerHeritagePersistentState& state) {
    using namespace sampler_heritage_state_detail;
    SamplerHeritageStateWriteResult result;
    if (!state.enabled) {
        result.status = SamplerHeritageStateStatus::Ok;
        return result;
    }

    try {
        const auto profile_json =
            audio::write_sample_heritage_profile_json(state.profile);
        if (!profile_json.valid()) return result;
        if (profile_json.json.empty() ||
            profile_json.json.size() > kSamplerHeritageMaximumProfileJsonBytes) {
            result.status = SamplerHeritageStateStatus::LengthOutOfRange;
            return result;
        }

        std::string_view runtime_json_view;
        audio::SampleHeritageRuntimeStateJsonWriteResult runtime_json;
        if (state.has_runtime_state) {
            if (!(state.runtime_host_sample_rate > 0.0) ||
                !std::isfinite(state.runtime_host_sample_rate)) {
                result.status = SamplerHeritageStateStatus::LengthOutOfRange;
                return result;
            }
            const auto validation =
                audio::validate_sample_heritage_profile(state.profile);
            if (!runtime_matches_profile(state.runtime_state, validation)) {
                result.status = SamplerHeritageStateStatus::RuntimeProfileMismatch;
                return result;
            }
            if (!runtime_layout_matches_profile(state.runtime_state, state.profile)) {
                result.status =
                    SamplerHeritageStateStatus::RuntimeStageLayoutMismatch;
                return result;
            }
            runtime_json = audio::write_sample_heritage_runtime_state_json(
                state.runtime_state);
            if (!runtime_json.valid()) {
                result.status = SamplerHeritageStateStatus::InvalidRuntimeJson;
                return result;
            }
            if (runtime_json.json.empty() ||
                runtime_json.json.size() > kSamplerHeritageMaximumRuntimeJsonBytes) {
                result.status = SamplerHeritageStateStatus::LengthOutOfRange;
                return result;
            }
            runtime_json_view = runtime_json.json;
        }

        const auto total = kSamplerHeritageStateHeaderBytes +
                           profile_json.json.size() + runtime_json_view.size();
        result.bytes.reserve(total);
        append_u32(result.bytes, kSamplerHeritageStateMagic);
        append_u16(result.bytes, kSamplerHeritageStateVersion);
        append_u16(result.bytes,
                   state.has_runtime_state ? kSamplerHeritageStateHasRuntime : 0);
        append_u32(result.bytes,
                   static_cast<std::uint32_t>(profile_json.json.size()));
        append_u32(result.bytes,
                   static_cast<std::uint32_t>(runtime_json_view.size()));
        append_f64(result.bytes, state.has_runtime_state
                                     ? state.runtime_host_sample_rate
                                     : 0.0);
        result.bytes.insert(result.bytes.end(), profile_json.json.begin(),
                            profile_json.json.end());
        result.bytes.insert(result.bytes.end(), runtime_json_view.begin(),
                            runtime_json_view.end());
        result.status = SamplerHeritageStateStatus::Ok;
        return result;
    } catch (...) {
        result.bytes.clear();
        result.status = SamplerHeritageStateStatus::AllocationFailure;
        return result;
    }
}

/// Parse without publishing partial state. Empty legacy blobs mean heritage is
/// disabled; every non-empty blob must be a complete canonical v1 or v2
/// envelope.
inline SamplerHeritageStateParseResult parse_sampler_heritage_state(
    std::span<const std::uint8_t> bytes) {
    using namespace sampler_heritage_state_detail;
    SamplerHeritageStateParseResult result;
    if (bytes.empty()) {
        result.status = SamplerHeritageStateStatus::Ok;
        return result;
    }
    if (bytes.size() < kSamplerHeritageStateV1HeaderBytes) return result;
    if (read_u32(bytes, 0) != kSamplerHeritageStateMagic) {
        result.status = SamplerHeritageStateStatus::BadMagic;
        return result;
    }
    const auto version = read_u16(bytes, 4);
    if (version != 1 && version != kSamplerHeritageStateVersion) {
        result.status = SamplerHeritageStateStatus::UnsupportedVersion;
        return result;
    }
    const auto header_bytes = version == 1
        ? kSamplerHeritageStateV1HeaderBytes
        : kSamplerHeritageStateHeaderBytes;
    if (bytes.size() < header_bytes) return result;
    const auto flags = read_u16(bytes, 6);
    if ((flags & ~kSamplerHeritageStateHasRuntime) != 0) {
        result.status = SamplerHeritageStateStatus::UnsupportedFlags;
        return result;
    }
    const auto profile_size = static_cast<std::size_t>(read_u32(bytes, 8));
    const auto runtime_size = static_cast<std::size_t>(read_u32(bytes, 12));
    const bool has_runtime = (flags & kSamplerHeritageStateHasRuntime) != 0;
    const auto encoded_runtime_rate = version == 1
        ? 0.0
        : read_f64(bytes, kSamplerHeritageStateV1HeaderBytes);
    if (profile_size == 0 ||
        profile_size > kSamplerHeritageMaximumProfileJsonBytes ||
        runtime_size > kSamplerHeritageMaximumRuntimeJsonBytes ||
        has_runtime != (runtime_size != 0) ||
        (!has_runtime && encoded_runtime_rate != 0.0)) {
        result.status = SamplerHeritageStateStatus::LengthOutOfRange;
        return result;
    }
    if (profile_size > std::numeric_limits<std::size_t>::max() - header_bytes ||
        runtime_size > std::numeric_limits<std::size_t>::max() -
                           header_bytes - profile_size) {
        result.status = SamplerHeritageStateStatus::LengthOutOfRange;
        return result;
    }
    const auto expected_size =
        header_bytes + profile_size + runtime_size;
    if (bytes.size() < expected_size) return result;
    if (bytes.size() > expected_size) {
        result.status = SamplerHeritageStateStatus::TrailingBytes;
        return result;
    }

    try {
        const auto profile_bytes =
            bytes.subspan(header_bytes, profile_size);
        const auto parsed_profile =
            audio::parse_sample_heritage_profile_json(as_string(profile_bytes));
        if (!parsed_profile.valid()) {
            result.status = SamplerHeritageStateStatus::InvalidProfileJson;
            return result;
        }
        const auto canonical_profile =
            audio::write_sample_heritage_profile_json(parsed_profile.profile);
        if (!canonical_profile.valid() ||
            canonical_profile.json != as_string(profile_bytes)) {
            result.status = SamplerHeritageStateStatus::NonCanonicalProfileJson;
            return result;
        }

        SamplerHeritagePersistentState candidate;
        candidate.enabled = true;
        candidate.profile = parsed_profile.profile;
        candidate.has_runtime_state = has_runtime;
        if (has_runtime) {
            candidate.runtime_host_sample_rate = version == 1
                ? 0.0
                : encoded_runtime_rate;
            if (version != 1 &&
                (!(candidate.runtime_host_sample_rate > 0.0) ||
                 !std::isfinite(candidate.runtime_host_sample_rate))) {
                result.status = SamplerHeritageStateStatus::LengthOutOfRange;
                return result;
            }
            const auto runtime_bytes = bytes.subspan(
                header_bytes + profile_size, runtime_size);
            const auto parsed_runtime =
                audio::parse_sample_heritage_runtime_state_json(
                    as_string(runtime_bytes));
            if (!parsed_runtime.valid()) {
                result.status = SamplerHeritageStateStatus::InvalidRuntimeJson;
                return result;
            }
            const auto canonical_runtime =
                audio::write_sample_heritage_runtime_state_json(
                    parsed_runtime.state);
            if (!canonical_runtime.valid() ||
                canonical_runtime.json != as_string(runtime_bytes)) {
                result.status = SamplerHeritageStateStatus::NonCanonicalRuntimeJson;
                return result;
            }
            const auto validation =
                audio::validate_sample_heritage_profile(candidate.profile);
            if (!runtime_matches_profile(parsed_runtime.state, validation)) {
                result.status = SamplerHeritageStateStatus::RuntimeProfileMismatch;
                return result;
            }
            if (!runtime_layout_matches_profile(parsed_runtime.state,
                                                candidate.profile)) {
                result.status =
                    SamplerHeritageStateStatus::RuntimeStageLayoutMismatch;
                return result;
            }
            candidate.runtime_state = parsed_runtime.state;
        }
        result.state = std::move(candidate);
        result.status = SamplerHeritageStateStatus::Ok;
        return result;
    } catch (...) {
        result.state = {};
        result.status = SamplerHeritageStateStatus::AllocationFailure;
        return result;
    }
}

}  // namespace pulp::examples
