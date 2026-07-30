#pragma once

#include "delay_types.hpp"

#include <pulp/signal/character_delay.hpp>

#include <array>

namespace pulp::examples::delay {

struct CharacterEngineConfig {
    float time_ms = 380.0f;
    float right_time_ms = 425.6f;
    float time_offset = 1.12f;
    bool right_uses_ratio = true;
    float feedback = 0.62f;
    float crossfeed = 0.48f;
    float character_amount = 0.58f;
    float diffusion = 0.22f;
    float duck = 0.34f;
    float low_cut_hz = 180.0f;
    float high_cut_hz = 4800.0f;
    float low_cut_resonance = 1.2f;
    float high_cut_resonance = 0.8f;
    float mod_rate_normalized = 0.4f;
    float mod_depth = 0.28f;
    bool freeze = false;
    bool reverse = false;
};

class CharacterEngineBank {
  public:
    void prepare(double sample_rate, Character initial_character);
    void reset() noexcept;

    void request_character(Character character) noexcept;
    void apply(const CharacterEngineConfig& config) noexcept;

    void process(float* left, float* right, int num_samples, float* alternate_left,
                 float* alternate_right) noexcept;

    Character active_character() const noexcept {
        return active_character_;
    }
    Character target_character() const noexcept {
        return target_character_;
    }
    bool is_crossfading() const noexcept {
        return priming_ || crossfading_;
    }

  private:
    static signal::CharacterDelay::Character to_engine_character(Character character) noexcept;
    static void apply_to(signal::CharacterDelay& engine,
                         const CharacterEngineConfig& config) noexcept;
    void begin_priming(Character character) noexcept;
    void start_crossfade(Character character) noexcept;

    std::array<signal::CharacterDelay, 2> engines_;
    CharacterEngineConfig config_;
    double sample_rate_ = 48000.0;
    int active_index_ = 0;
    Character active_character_ = Character::tape;
    Character target_character_ = Character::tape;
    Character priming_character_ = Character::tape;
    Character fade_character_ = Character::tape;
    bool freeze_active_ = false;
    bool priming_ = false;
    bool crossfading_ = false;
    int prime_samples_remaining_ = 0;
    int fade_position_ = 0;
    int fade_samples_ = 1;
};

} // namespace pulp::examples::delay
