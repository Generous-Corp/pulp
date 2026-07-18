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
    "pulp.sample-heritage.profile-digest.v1";

class CanonicalBytes {
public:
    void byte(std::uint8_t value) { bytes_.push_back(value); }

    template<typename Integer>
    void integer(Integer value) {
        using Unsigned = std::make_unsigned_t<Integer>;
        auto bits = static_cast<std::uint64_t>(static_cast<Unsigned>(value));
        for (std::size_t index = 0; index < sizeof(Unsigned); ++index) {
            byte(static_cast<std::uint8_t>(bits & 0xffu));
            bits >>= 8;
        }
    }

    template<typename Number>
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

std::array<std::uint8_t, 32> sample_heritage_profile_digest(
    const SampleHeritageProfile& profile) {
    CanonicalBytes canonical;
    canonical.text(kProfileDigestDomain);
    canonical.integer(kSampleHeritageProfileDigestVersion);
    canonical.integer(profile.schema_version);
    canonical.text(profile.profile_id);
    canonical.floating(profile.host_sample_rate);
    canonical.integer(static_cast<std::uint64_t>(profile.stages.size()));
    for (const auto& spec : profile.stages) {
        canonical.byte(spec.bypass ? 1 : 0);
        canonical.integer(static_cast<std::uint32_t>(spec.parameters.index()));
        std::visit([&](const auto& stage) {
            using Stage = std::decay_t<decltype(stage)>;
            if constexpr (std::is_same_v<Stage, SampleHeritageMachineDomainStage>) {
                canonical.floating(stage.sample_rate);
            } else if constexpr (std::is_same_v<Stage, SampleHeritageQuantizationStage>) {
                canonical.integer(stage.bit_depth);
                canonical.floating(stage.dither_lsb);
                canonical.integer(stage.seed);
                canonical.integer(static_cast<std::uint8_t>(stage.seed_policy));
            } else if constexpr (std::is_same_v<Stage, SampleHeritageClockPitchStage>) {
                canonical.floating(stage.ratio);
            } else if constexpr (std::is_same_v<Stage, SampleHeritageDacHoldStage>) {
                canonical.integer(stage.hold_samples);
            } else if constexpr (std::is_same_v<Stage,
                                                SampleHeritageReconstructionFilterStage>) {
                canonical.floating(stage.cutoff_hz);
            } else if constexpr (std::is_same_v<Stage, SampleHeritageNoiseStage>) {
                canonical.floating(stage.amplitude);
                canonical.integer(stage.seed);
                canonical.integer(static_cast<std::uint8_t>(stage.seed_policy));
            } else if constexpr (std::is_same_v<Stage, SampleHeritageOutputStage>) {
                canonical.floating(stage.gain);
            }
        }, spec.parameters);
    }

    const auto digest = runtime::sha256(canonical.get().data(), canonical.get().size());
    if (digest.size() != 32) throw std::runtime_error("SHA-256 digest unavailable");
    std::array<std::uint8_t, 32> result{};
    std::copy(digest.begin(), digest.end(), result.begin());
    return result;
}

}  // namespace pulp::audio
