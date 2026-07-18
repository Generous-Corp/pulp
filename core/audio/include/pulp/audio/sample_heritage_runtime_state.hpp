#pragma once

#include <pulp/audio/sample_heritage_schema.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <string_view>

namespace pulp::audio {

inline constexpr std::uint32_t kSampleHeritageRuntimeStateSchemaVersion = 1;

enum class SampleHeritageRuntimeRngStageType : std::uint8_t {
    Quantization,
    Noise,
};

struct SampleHeritageRuntimeRngState {
    std::uint8_t stage_index = 0;
    SampleHeritageRuntimeRngStageType stage_type =
        SampleHeritageRuntimeRngStageType::Quantization;
    std::uint64_t random_state = 0;
};

/// Fixed-capacity RNG-stream continuation payload. It intentionally does not
/// serialize whole-engine DSP transients: restore resets DAC-hold phase/value
/// and reconstruction-filter history before resuming only stages that opt into
/// ContinueSerializedState. Capture and restore require the audio callback to
/// be quiescent; JSON conversion remains an off-audio-thread API.
struct SampleHeritageRuntimeState {
    std::uint32_t schema_version = kSampleHeritageRuntimeStateSchemaVersion;
    std::uint32_t profile_schema_version = 0;
    std::array<char, kSampleHeritageMaximumProfileIdBytes + 1> profile_id{};
    std::uint32_t profile_digest_version = kSampleHeritageProfileDigestVersion;
    std::array<std::uint8_t, 32> profile_digest{};
    std::array<SampleHeritageRuntimeRngState, kSampleHeritageMaximumStages>
        rng_states{};
    std::size_t rng_state_count = 0;

    std::string_view bound_profile_id() const noexcept {
        const auto end = std::find(profile_id.begin(), profile_id.end(), '\0');
        return {profile_id.data(),
                static_cast<std::size_t>(std::distance(profile_id.begin(), end))};
    }
};

enum class SampleHeritageRuntimeStateStatus : std::uint8_t {
    Ok,
    NotPrepared,
    UnsupportedSchemaVersion,
    ProfileMismatch,
    InvalidStageLayout,
    InvalidRandomState,
};

struct SampleHeritageRuntimeStateCapture {
    SampleHeritageRuntimeStateStatus status =
        SampleHeritageRuntimeStateStatus::NotPrepared;
    SampleHeritageRuntimeState state{};

    bool valid() const noexcept {
        return status == SampleHeritageRuntimeStateStatus::Ok;
    }
};

}  // namespace pulp::audio
