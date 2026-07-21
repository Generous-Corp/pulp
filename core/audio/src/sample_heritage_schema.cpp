#include <pulp/audio/sample_heritage_schema.hpp>

#include <pulp/runtime/crypto.hpp>

#include <bit>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace pulp::audio {
namespace {

// Changing this byte contract requires a digest-version bump. The prefix keeps
// these bytes out of every other SHA-256 protocol domain in Pulp.
constexpr std::string_view kProfileDigestDomain =
    "pulp.sample-heritage.profile-digest.v5";

class CanonicalBytes {
public:
    void byte(std::uint8_t value) { bytes_.push_back(value); }

    template <typename Integer>
    void integer(Integer value) {
        using Unsigned = std::make_unsigned_t<Integer>;
        auto bits = static_cast<std::uint64_t>(static_cast<Unsigned>(value));
        for (std::size_t index = 0; index < sizeof(Unsigned); ++index) {
            byte(static_cast<std::uint8_t>(bits & 0xffu));
            bits >>= 8;
        }
    }

    template <typename Number>
    void floating(Number value) {
        // JSON canonicalization also maps -0 to 0. Do the same before hashing
        // so authoring, JSON round trips, and prepared profiles share identity.
        value = value == Number{0} ? Number{0} : value;
        if constexpr (std::is_same_v<Number, float>)
            integer(std::bit_cast<std::uint32_t>(value));
        else
            integer(std::bit_cast<std::uint64_t>(value));
    }

    void text(std::string_view value) {
        integer(static_cast<std::uint64_t>(value.size()));
        for (const char character : value)
            byte(static_cast<std::uint8_t>(character));
    }

    const std::vector<std::uint8_t>& get() const noexcept { return bytes_; }

private:
    std::vector<std::uint8_t> bytes_;
};

}  // namespace

std::array<std::uint8_t, 32> sample_heritage_profile_digest(const SampleHeritageProfile& profile) {
    CanonicalBytes canonical;
    canonical.text(kProfileDigestDomain);
    canonical.integer(kSampleHeritageProfileDigestVersion);
    canonical.integer(profile.schema_version);
    canonical.text(profile.profile_id);
    // Host rate is an execution context, not profile identity. The same typed
    // machine profile must retain its identity when a host prepares at another
    // supported rate.
    const auto seed_policy = [&](SampleHeritageSeedPolicy value) {
        canonical.integer(static_cast<std::uint8_t>(value));
    };
    canonical.integer(static_cast<std::uint64_t>(profile.voice.size()));
    for (const auto& spec : profile.voice) {
        canonical.byte(static_cast<std::uint8_t>(spec.domain));
        canonical.byte(spec.bypass ? 1 : 0);
        canonical.integer(static_cast<std::uint32_t>(spec.parameters.index()));
        std::visit(
            [&](const auto& block) {
            using Block = std::decay_t<decltype(block)>;
            if constexpr (std::is_same_v<Block, SampleHeritageVoiceMachineDomainBlock>)
                canonical.floating(block.sample_rate);
            else if constexpr (std::is_same_v<Block, SampleHeritageVoiceClockBlock>)
                canonical.floating(block.ratio);
            else if constexpr (std::is_same_v<Block, SampleHeritageVoicePitchBlock>) {
                canonical.integer(static_cast<std::uint8_t>(block.family));
                canonical.floating(block.max_transpose_semitones);
            }
            else if constexpr (std::is_same_v<Block, SampleHeritageVoiceConverterBlock>) {
                canonical.integer(static_cast<std::uint8_t>(block.family));
                canonical.floating(block.bit_depth);
                canonical.floating(block.dac_nonlinearity);
                canonical.floating(block.dither_lsb);
                canonical.integer(block.seed);
                seed_policy(block.seed_policy);
            } else if constexpr (std::is_same_v<Block, SampleHeritageVoiceHoldDroopBlock>) {
                canonical.integer(static_cast<std::uint8_t>(block.mode));
                canonical.integer(block.hold_samples);
                canonical.floating(block.droop);
            } else if constexpr (std::is_same_v<Block,
                                                 SampleHeritageVoiceReconstructionBlock>) {
                canonical.integer(static_cast<std::uint8_t>(block.family));
                canonical.integer(static_cast<std::uint8_t>(block.cutoff_law));
                canonical.floating(block.cutoff_value);
                canonical.integer(block.order);
                canonical.floating(block.ripple_db);
                canonical.floating(block.stopband_attenuation_db);
            } else if constexpr (std::is_same_v<Block,
                                                 SampleHeritageVoiceAnalogColorBlock>) {
                canonical.floating(block.drive);
                canonical.floating(block.asymmetry);
                canonical.floating(block.mix);
                canonical.integer(static_cast<std::uint8_t>(block.filter_family));
                canonical.integer(static_cast<std::uint8_t>(block.cutoff_law));
                canonical.floating(block.cutoff_value);
                canonical.floating(block.resonance);
            } else {
                canonical.floating(block.factor);
                canonical.floating(block.cycle_ms);
                canonical.floating(block.splice_ms);
                canonical.byte(block.stereo_link ? 1 : 0);
                canonical.integer(block.shuffle_divisions);
                canonical.integer(block.seed);
                seed_policy(block.seed_policy);
                canonical.integer(static_cast<std::uint8_t>(block.pitch_mode));
                canonical.byte(block.tempo_lock ? 1 : 0);
            }
        }, spec.parameters);
    }
    canonical.integer(static_cast<std::uint64_t>(profile.bus.size()));
    for (const auto& spec : profile.bus) {
        canonical.byte(static_cast<std::uint8_t>(spec.domain));
        canonical.byte(spec.bypass ? 1 : 0);
        canonical.integer(static_cast<std::uint32_t>(spec.parameters.index()));
        std::visit(
            [&](const auto& block) {
            using Block = std::decay_t<decltype(block)>;
            if constexpr (std::is_same_v<Block, SampleHeritageBusNoiseIdleBlock>) {
                canonical.floating(block.noise_amplitude);
                canonical.floating(block.idle_amplitude);
                canonical.floating(block.tilt_db_per_octave);
                canonical.floating(block.tilt_reference_hz);
                canonical.floating(block.tilt_floor_hz);
                canonical.integer(static_cast<std::uint8_t>(block.gate));
                canonical.integer(block.seed);
                seed_policy(block.seed_policy);
            } else {
                canonical.floating(block.drive);
                canonical.floating(block.ceiling);
            }
        }, spec.parameters);
    }
    canonical.integer(static_cast<std::uint64_t>(profile.record_commit.size()));
    for (const auto& spec : profile.record_commit) {
        canonical.byte(static_cast<std::uint8_t>(spec.domain));
        canonical.byte(spec.bypass ? 1 : 0);
        canonical.integer(static_cast<std::uint32_t>(spec.parameters.index()));
        std::visit(
            [&](const auto& block) {
            using Block = std::decay_t<decltype(block)>;
            if constexpr (std::is_same_v<Block,
                                          SampleHeritageRecordInputDriveClipBlock>) {
                canonical.floating(block.drive);
                canonical.floating(block.clip_level);
            } else if constexpr (std::is_same_v<Block,
                                                 SampleHeritageRecordRateBlock>) {
                canonical.integer(static_cast<std::uint8_t>(block.filter_family));
                canonical.floating(block.sample_rate);
                canonical.integer(static_cast<std::uint8_t>(block.cutoff_law));
                canonical.floating(block.cutoff_value);
                canonical.integer(block.order);
                canonical.floating(block.ripple_db);
                canonical.floating(block.stopband_attenuation_db);
            } else if constexpr (std::is_same_v<Block,
                                                 SampleHeritageRecordConverterBlock>) {
                canonical.integer(static_cast<std::uint8_t>(block.family));
                canonical.floating(block.bit_depth);
                canonical.floating(block.dac_nonlinearity);
                canonical.floating(block.dither_lsb);
                canonical.integer(block.seed);
                seed_policy(block.seed_policy);
            } else if constexpr (std::is_same_v<Block,
                                                    SampleHeritageRecordCommitCyclicStretchBlock>) {
                canonical.floating(block.factor);
                canonical.integer(block.cycle_samples);
                    canonical.integer(block.crossfade_samples);
                    canonical.integer(block.zone_start_frame);
                    canonical.integer(block.zone_end_frame);
                } else {
                    canonical.floating(block.factor);
                    canonical.integer(block.decision_hop_samples);
                    canonical.integer(block.search_radius_samples);
                    canonical.integer(block.search_stride_samples);
                    canonical.integer(block.crossfade_samples);
                canonical.integer(block.zone_start_frame);
                canonical.integer(block.zone_end_frame);
                canonical.byte(block.stereo_link ? 1 : 0);
                canonical.integer(block.quality);
                canonical.integer(block.width);
            }
        }, spec.parameters);
    }

    const bool legacy = profile.voice.empty() && profile.bus.empty() &&
                        profile.record_commit.empty() && !profile.stages.empty();
    if (legacy) {
        canonical.integer(static_cast<std::uint64_t>(profile.stages.size()));
        for (const auto& spec : profile.stages) {
            canonical.byte(spec.bypass ? 1 : 0);
            canonical.integer(static_cast<std::uint32_t>(spec.parameters.index()));
            std::visit(
                [&](const auto& stage) {
                using Stage = std::decay_t<decltype(stage)>;
                if constexpr (std::is_same_v<Stage, SampleHeritageMachineDomainStage>) {
                    canonical.floating(stage.sample_rate);
                } else if constexpr (std::is_same_v<Stage,
                                                    SampleHeritageQuantizationStage>) {
                    canonical.integer(stage.bit_depth);
                    canonical.floating(stage.dither_lsb);
                    canonical.integer(stage.seed);
                    canonical.integer(static_cast<std::uint8_t>(stage.seed_policy));
                } else if constexpr (std::is_same_v<Stage,
                                                    SampleHeritageClockPitchStage>) {
                    canonical.floating(stage.ratio);
                } else if constexpr (std::is_same_v<Stage,
                                                    SampleHeritageDacHoldStage>) {
                    canonical.integer(stage.hold_samples);
                } else if constexpr (std::is_same_v<Stage,
                                         SampleHeritageReconstructionFilterStage>) {
                    canonical.floating(stage.cutoff_hz);
                } else if constexpr (std::is_same_v<Stage,
                                                    SampleHeritageNoiseStage>) {
                    canonical.floating(stage.amplitude);
                    canonical.integer(stage.seed);
                    canonical.integer(static_cast<std::uint8_t>(stage.seed_policy));
                } else if constexpr (std::is_same_v<Stage,
                                                    SampleHeritageOutputStage>) {
                    canonical.floating(stage.gain);
                }
            }, spec.parameters);
        }
    }

    const auto digest = runtime::sha256(canonical.get().data(), canonical.get().size());
    if (digest.size() != 32) throw std::runtime_error("SHA-256 digest unavailable");
    std::array<std::uint8_t, 32> result{};
    std::copy(digest.begin(), digest.end(), result.begin());
    return result;
}

}  // namespace pulp::audio
