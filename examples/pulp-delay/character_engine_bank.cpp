#include "character_engine_bank.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace pulp::examples::delay {

void CharacterEngineBank::prepare(double sample_rate, Character initial_character) {
    const double valid_rate =
        std::isfinite(sample_rate) && sample_rate >= 1000.0 ? sample_rate : 48000.0;
    sample_rate_ = valid_rate;
    for (auto& engine : engines_) {
        engine.set_character(to_engine_character(initial_character));
        engine.set_sample_rate(valid_rate);
    }
    active_index_ = 0;
    active_character_ = initial_character;
    target_character_ = initial_character;
    priming_character_ = initial_character;
    fade_character_ = initial_character;
    freeze_active_ = false;
    priming_ = false;
    crossfading_ = false;
    prime_samples_remaining_ = 0;
    fade_position_ = 0;
    fade_samples_ = std::max(1, static_cast<int>(std::lround(valid_rate * 0.020)));
}

void CharacterEngineBank::reset() noexcept {
    for (auto& engine : engines_)
        engine.reset();
    priming_ = false;
    crossfading_ = false;
    target_character_ = active_character_;
    fade_position_ = 0;
}

void CharacterEngineBank::request_character(Character character) noexcept {
    if (character == target_character_ && (priming_ || crossfading_))
        return;
    target_character_ = character;
    if (crossfading_)
        return;
    if (priming_) {
        if (character == active_character_) {
            priming_ = false;
            prime_samples_remaining_ = 0;
        }
        return;
    }
    if (freeze_active_) {
        priming_ = false;
        prime_samples_remaining_ = 0;
        return;
    }

    if (character == active_character_) {
        priming_ = false;
        prime_samples_remaining_ = 0;
        return;
    }
    begin_priming(character);
}

void CharacterEngineBank::begin_priming(Character character) noexcept {
    const int incoming = 1 - active_index_;
    priming_character_ = character;
    engines_[incoming].set_character(to_engine_character(character));
    auto priming_config = config_;
    priming_config.freeze = false;
    apply_to(engines_[incoming], priming_config);
    const float longest_time_ms =
        std::max(config_.time_ms, config_.right_time_ms);
    prime_samples_remaining_ = std::max(
        fade_samples_, static_cast<int>(std::ceil(
                           static_cast<double>(longest_time_ms) * sample_rate_
                           * 0.001)));
    priming_ = true;
}

void CharacterEngineBank::start_crossfade(Character character) noexcept {
    const int incoming = 1 - active_index_;
    engines_[incoming].set_character(to_engine_character(character));
    fade_character_ = character;
    crossfading_ = true;
    fade_position_ = 0;
}

void CharacterEngineBank::apply(const CharacterEngineConfig& config) noexcept {
    const bool was_frozen = freeze_active_;
    freeze_active_ = config.freeze;
    config_ = config;
    if (freeze_active_ && priming_) {
        priming_ = false;
        prime_samples_remaining_ = 0;
    } else if (was_frozen && !freeze_active_ && !crossfading_
               && target_character_ != active_character_) {
        begin_priming(target_character_);
    }
    apply_to(engines_[active_index_], config);
    auto incoming_config = config;
    if (priming_)
        incoming_config.freeze = false;
    apply_to(engines_[1 - active_index_], incoming_config);
}

void CharacterEngineBank::process(float* left, float* right, int num_samples, float* alternate_left,
                                  float* alternate_right) noexcept {
    if (left == nullptr || right == nullptr || num_samples <= 0)
        return;
    if (!crossfading_) {
        if (alternate_left != nullptr && alternate_right != nullptr) {
            // Keep the inactive engine moving with the authored input. During
            // a pending character change, also feed it the outgoing wet signal
            // so character-owned delay lines (BBD/vintage) acquire the live
            // tail before the crossfade begins.
            std::copy_n(left, num_samples, alternate_left);
            std::copy_n(right, num_samples, alternate_right);
        }
        engines_[active_index_].process(left, right, num_samples);
        if (alternate_left != nullptr && alternate_right != nullptr) {
            if (priming_) {
                for (int index = 0; index < num_samples; ++index) {
                    alternate_left[index] += left[index];
                    alternate_right[index] += right[index];
                }
            }
            engines_[1 - active_index_].process(alternate_left, alternate_right,
                                                num_samples);
            if (priming_) {
                prime_samples_remaining_ -= num_samples;
                if (prime_samples_remaining_ <= 0) {
                    priming_ = false;
                    apply_to(engines_[1 - active_index_], config_);
                    start_crossfade(priming_character_);
                }
            }
        }
        return;
    }
    if (alternate_left == nullptr || alternate_right == nullptr)
        return;

    for (int i = 0; i < num_samples; ++i) {
        alternate_left[static_cast<std::size_t>(i)] = left[static_cast<std::size_t>(i)];
        alternate_right[static_cast<std::size_t>(i)] = right[static_cast<std::size_t>(i)];
    }

    const int incoming = 1 - active_index_;
    engines_[active_index_].process(left, right, num_samples);
    engines_[incoming].process(alternate_left, alternate_right, num_samples);

    for (int i = 0; i < num_samples; ++i) {
        const float blend = std::clamp(
            static_cast<float>(fade_position_ + i) / static_cast<float>(fade_samples_), 0.0f, 1.0f);
        const auto index = static_cast<std::size_t>(i);
        left[index] += blend * (alternate_left[index] - left[index]);
        right[index] += blend * (alternate_right[index] - right[index]);
    }
    fade_position_ += num_samples;
    if (fade_position_ >= fade_samples_) {
        active_index_ = incoming;
        active_character_ = fade_character_;
        crossfading_ = false;
        fade_position_ = 0;
        if (!freeze_active_ && target_character_ != active_character_)
            begin_priming(target_character_);
    }
}

signal::CharacterDelay::Character
CharacterEngineBank::to_engine_character(Character character) noexcept {
    using EngineCharacter = signal::CharacterDelay::Character;
    switch (character) {
    case Character::clean:
        return EngineCharacter::clean;
    case Character::vintage:
        return EngineCharacter::vintage_digital;
    case Character::bbd:
        return EngineCharacter::bbd;
    case Character::tape:
    default:
        return EngineCharacter::tape;
    }
}

void CharacterEngineBank::apply_to(signal::CharacterDelay& engine,
                                   const CharacterEngineConfig& config) noexcept {
    engine.set_time_ms(config.time_ms);
    if (config.right_uses_ratio)
        engine.set_time_offset(config.time_offset);
    else
        engine.set_right_time_ms(config.right_time_ms);
    engine.set_feedback(config.feedback);
    engine.set_crossfeed(config.crossfeed);
    engine.set_character_amount(config.character_amount);
    engine.set_diffusion_amount(config.diffusion);
    engine.set_duck(config.duck);
    engine.set_loop_low_cut_hz(config.low_cut_hz);
    engine.set_loop_high_cut_hz(config.high_cut_hz);
    engine.set_loop_low_cut_resonance(config.low_cut_resonance);
    engine.set_loop_high_cut_resonance(config.high_cut_resonance);
    engine.set_mod(config.mod_rate_normalized, config.mod_depth);
    engine.set_freeze(config.freeze);
    engine.set_reverse(config.reverse);
}

} // namespace pulp::examples::delay
