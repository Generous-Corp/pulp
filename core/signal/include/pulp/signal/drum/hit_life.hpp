#pragma once

#include <cstdint>

namespace pulp::signal::drum {

/// Selects the intentional source of variation between otherwise identical
/// hits. These are lifecycle policies, not random "humanize" amounts.
///
/// Two independent things happen on a hit: what the excitation stream does,
/// and what the resonating body does. Every mode is one cell of that grid, so
/// a voice never has to encode a policy outside this enum:
///
/// | Mode                            | Excitation             | Body      |
/// |---------------------------------|------------------------|-----------|
/// | `fixed_seed`                    | restart, base seed     | reset     |
/// | `advancing_seed`                | restart, advancing     | reset     |
/// | `preserved_state`               | continue uninterrupted | preserved |
/// | `fixed_seed_preserved_body`     | restart, base seed     | preserved |
/// | `advancing_seed_preserved_body` | restart, advancing     | preserved |
///
/// Preserving the body is what stops a struck metal voice from sounding
/// machine-gunned: the previous hit keeps ringing and the new excitation is
/// added into a body that is already in motion, which is what happens when a
/// real cymbal is struck twice.
enum class HitLifeMode {
    fixed_seed,
    advancing_seed,
    preserved_state,
    fixed_seed_preserved_body,
    advancing_seed_preserved_body,
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
        if (mode_ == mode) return;
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
                return {advance_seed(base_seed), true, true};
            case HitLifeMode::preserved_state:
                return {base_seed, false, false};
            case HitLifeMode::fixed_seed_preserved_body:
                return {base_seed, true, false};
            case HitLifeMode::advancing_seed_preserved_body:
                return {advance_seed(base_seed), true, false};
        }
        return {base_seed, true, true};
    }

private:
    // Knuth's multiplicative constant. The hit index is what advances, so the
    // sequence is reproducible from the base seed rather than being drawn from
    // any ambient state.
    std::uint32_t advance_seed(std::uint32_t base_seed) {
        ++hit_index_;
        return base_seed + static_cast<std::uint32_t>(hit_index_ * 2654435761u);
    }

    HitLifeMode mode_ = HitLifeMode::fixed_seed;
    std::uint32_t hit_index_ = 0;
};

}  // namespace pulp::signal::drum
