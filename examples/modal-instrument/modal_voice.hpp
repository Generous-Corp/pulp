#pragma once

// ModalVoice -- one note of a polyphonic modal instrument.
//
// A voice is a ModalBankT struck by a contact pulse. Its mode set is loaded
// per note-on (retuned to the note's pitch and resolved at the current
// strike/pickup position), and it rings out on its own after the pulse
// finishes. Voices are the unit of polyphony: the instrument owns a fixed
// pool of them and hands each incoming note to one.
//
// Everything after prepare() is allocation- and lock-free. prepare() sizes
// the resonator bank, the reusable strike-pulse buffer, and the per-block
// input scratch; process()-time entry points (arm(), render()) only read and
// write that pre-sized storage. fill_strike_pulse() and set_modes() evaluate
// transcendentals but allocate nothing, which is what makes a control-rate
// retune-and-strike safe to run from the audio thread.

#include <pulp/signal/modal_bank.hpp>

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace pulp::examples {

class ModalVoice {
public:
    /// Allocate the resonator bank for up to @p max_modes modes, a strike-pulse
    /// buffer of @p max_contact_samples, and per-block input scratch of
    /// @p max_block samples. Allocates; call once before processing.
    void prepare(double sample_rate, int max_modes, int max_contact_samples,
                 int max_block) {
        sample_rate_ = sample_rate;
        bank_.prepare(sample_rate, std::max(max_modes, 1));
        pulse_.assign(static_cast<std::size_t>(std::max(max_contact_samples, 1)), 0.0f);
        input_.assign(static_cast<std::size_t>(std::max(max_block, 1)), 0.0f);
        pulse_len_ = 0;
        pulse_pos_ = 0;
        ring_remaining_ = 0;
        active_ = false;
        note_ = 0;
        age_ = 0;
    }

    /// Load a mode set into the bank (retune path). Preserves the existing
    /// resonator state lane-for-lane, so striking an already-ringing voice
    /// layers additively rather than resetting it. Allocation-free.
    void set_modes(std::span<const signal::ModalMode> modes) { bank_.set_modes(modes); }

    /// Arm a strike: fill @p contact_samples of the pulse buffer with a
    /// raised-cosine contact of amplitude @p velocity and begin injecting it on
    /// the next render. @p max_t60_s is the longest decay in the current mode
    /// set; it sets how long the voice is considered active after the strike.
    /// Allocation-free.
    void arm(float velocity, int contact_samples, uint8_t note, uint64_t age,
             double max_t60_s) {
        const int n = std::clamp(contact_samples, 1,
                                 static_cast<int>(pulse_.size()));
        signal::fill_strike_pulse(std::span<float>(pulse_.data(),
                                                   static_cast<std::size_t>(n)),
                                  velocity);
        pulse_len_ = n;
        pulse_pos_ = 0;
        note_ = note;
        age_ = age;
        active_ = true;
        // Ring long enough for the slowest mode to fall well below audibility.
        // T60 is 60 dB; 2.5 * T60 is ~150 dB down, comfortably into silence.
        ring_remaining_ = static_cast<int64_t>(max_t60_s * 2.5 * sample_rate_)
                        + pulse_len_;
    }

    /// Render @p count samples into @p out (accumulating). Drains any armed
    /// strike pulse into the bank's input, rings the bank, and counts down the
    /// voice's remaining active time. Allocation-free.
    void render(float* out, int count) {
        if (!active_ || count <= 0) return;
        const int n = std::min(count, static_cast<int>(input_.size()));
        for (int i = 0; i < n; ++i) {
            if (pulse_pos_ < pulse_len_)
                input_[static_cast<std::size_t>(i)] = pulse_[static_cast<std::size_t>(pulse_pos_++)];
            else
                input_[static_cast<std::size_t>(i)] = 0.0f;
        }
        bank_.process_add(input_.data(), out, n);
        ring_remaining_ -= n;
        if (ring_remaining_ <= 0) active_ = false;
    }

    /// Zero the resonator state and mark the voice free (discontinuity reset).
    void reset() {
        bank_.reset();
        pulse_len_ = 0;
        pulse_pos_ = 0;
        ring_remaining_ = 0;
        active_ = false;
    }

    bool active() const noexcept { return active_; }
    uint8_t note() const noexcept { return note_; }
    uint64_t age() const noexcept { return age_; }

private:
    signal::ModalBankT<float> bank_;
    std::vector<float> pulse_;   ///< reusable contact-pulse buffer (pre-sized)
    std::vector<float> input_;   ///< per-block excitation scratch (pre-sized)
    double sample_rate_ = 48000.0;
    int pulse_len_ = 0;
    int pulse_pos_ = 0;
    int64_t ring_remaining_ = 0;
    bool active_ = false;
    uint8_t note_ = 0;
    uint64_t age_ = 0;
};

}  // namespace pulp::examples
