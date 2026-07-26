#pragma once

#include <cstdint>

namespace pulp::signal::drum {

/// Selects the intentional source of variation between otherwise identical
/// hits. These are lifecycle policies, not random "humanize" amounts:
///
/// * fixed_seed restarts deterministic excitation and DSP state;
/// * advancing_seed restarts DSP state but advances deterministic excitation;
/// * preserved_state keeps both the DSP memory and excitation stream alive.
enum class HitLifeMode {
    fixed_seed,
    advancing_seed,
    preserved_state,
};

struct HitLifeDecision {
    std::uint32_t seed = 0;
    bool reseed_excitation = true;
    bool reset_dsp_state = true;
};

/// Allocation-free shared sequencing for a voice's anti-machine-gun policy.
class HitLife {
public:
    explicit HitLife(HitLifeMode mode = HitLifeMode::fixed_seed)
        : mode_(mode) {}

    void set_mode(HitLifeMode mode) {
        mode_ = mode;
        hit_index_ = 0;
    }
    HitLifeMode mode() const { return mode_; }

    void reset() { hit_index_ = 0; }

    HitLifeDecision trigger(std::uint32_t base_seed) {
        switch (mode_) {
            case HitLifeMode::fixed_seed:
                return {base_seed, true, true};
            case HitLifeMode::advancing_seed:
                ++hit_index_;
                return {
                    base_seed +
                        static_cast<std::uint32_t>(hit_index_ * 2654435761u),
                    true,
                    true,
                };
            case HitLifeMode::preserved_state:
                return {base_seed, false, false};
        }
        return {base_seed, true, true};
    }

private:
    HitLifeMode mode_ = HitLifeMode::fixed_seed;
    std::uint32_t hit_index_ = 0;
};

}  // namespace pulp::signal::drum
