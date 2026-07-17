#pragma once

// GongVoice -- one note of a polyphonic gong / tam-tam.
//
// A voice is a GongPlate (a dense inharmonic thin-plate modal cloud plus the
// bounded nonlinear bloom coupling) struck by a contact pulse. Its mode set is
// loaded per note-on (retuned to the note's pitch), and it rings out on its own
// after the pulse finishes -- with the bloom cascading energy upward for the
// first seconds of the ring. Voices are the unit of polyphony: the instrument
// owns a fixed pool of them and hands each incoming note to one.
//
// A strike is ADDITIVE: it injects a contact pulse into the (possibly still
// ringing) plate without resetting state, so re-striking a sounding gong layers
// rather than cutting -- the physical behaviour of hitting a plate that is
// already vibrating.
//
// Everything after prepare() is allocation- and lock-free. prepare() sizes the
// plate, the reusable strike-pulse buffer, and the per-block input scratch;
// arm() and render() only read and write that pre-sized storage. set_modes()
// rebuilds the triad set (a per-note retune resolves the same plate spectrum to
// a new pitch), which allocates in its rebuild scratch -- so a voice's mode set
// is loaded from the control path, and a note-on that changes pitch pays that
// control-rate rebuild. (The triad list is scale-invariant, so a same-pitch
// re-strike does not rebuild.)

#include "gong_plate.hpp"

#include <pulp/signal/modal_bank.hpp>

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace pulp::examples {

class GongVoice {
public:
    /// Allocate the plate for up to @p max_modes modes and @p max_partners
    /// coupling partners per mode, a strike-pulse buffer of
    /// @p max_contact_samples, and per-block input scratch of @p max_block
    /// samples. Allocates; call once before processing.
    void prepare(double sample_rate, int max_modes, int max_partners,
                 int max_contact_samples, int max_block) {
        sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
        plate_.prepare(sample_rate_, std::max(max_modes, 1), max_partners, max_block);
        pulse_.assign(static_cast<std::size_t>(std::max(max_contact_samples, 1)), 0.0f);
        input_.assign(static_cast<std::size_t>(std::max(max_block, 1)), 0.0f);
        pulse_len_ = 0;
        pulse_pos_ = 0;
        ring_remaining_ = 0;
        active_ = false;
        note_ = 0;
        age_ = 0;
    }

    /// Load a mode set + per-mode output weights into the plate (retune path;
    /// rebuilds the triad set). Preserves resonator state lane-for-lane so an
    /// additive re-strike layers. Control-rate.
    void set_modes(std::span<const signal::ModalMode> modes,
                   std::span<const float> out_weights) {
        plate_.set_modes(modes, out_weights);
    }

    /// Retune the plate cloud by a uniform pitch ratio (note pitch + Tune).
    /// Alloc-free; keeps the scale-invariant triad set. RT-safe note-on path.
    void retune(double ratio) noexcept { plate_.retune(ratio); }

    /// Scale the plate's decay times (the Decay control). Alloc-free.
    void set_decay_scale(double factor) noexcept { plate_.set_decay_scale(factor); }

    /// Set the bloom (coupling) amount [0,1] on this voice's plate.
    void set_bloom(double bloom) noexcept { plate_.set_bloom(bloom); }

    /// Arm a strike. @p velocity is the peak contact amplitude; @p contact_samples
    /// is the contact duration (short = hard = brighter attack + more low-mode
    /// energy for the cascade to pump); @p max_t60_s sets how long the voice is
    /// considered active after the strike. Allocation-free. The pulse is
    /// area-normalized by the caller so contact shapes the spectrum without
    /// changing loudness.
    void arm(float velocity, int contact_samples, uint8_t note, uint64_t age,
             double max_t60_s) {
        const int n = std::clamp(contact_samples, 1, static_cast<int>(pulse_.size()));
        signal::fill_strike_pulse(
            std::span<float>(pulse_.data(), static_cast<std::size_t>(n)), velocity);
        pulse_len_ = n;
        pulse_pos_ = 0;
        note_ = note;
        age_ = age;
        active_ = true;
        // Ring long enough for the slowest mode to fall well below audibility
        // (2.5*T60 ~ 150 dB down). The bloom only redistributes energy, so it
        // does not extend the tail past the linear T60 budget.
        ring_remaining_ = static_cast<int64_t>(max_t60_s * 2.5 * sample_rate_) + pulse_len_;
    }

    /// Render @p count samples into @p out (accumulating). Drains any armed
    /// strike pulse into the plate's input, advances the plate (linear ring +
    /// block-rate bloom), and counts down the voice's active time.
    /// Allocation-free.
    void render(float* out, int count) {
        if (!active_ || count <= 0) return;
        const int n = std::min(count, static_cast<int>(input_.size()));
        for (int i = 0; i < n; ++i) {
            input_[static_cast<std::size_t>(i)] =
                pulse_pos_ < pulse_len_ ? pulse_[static_cast<std::size_t>(pulse_pos_++)] : 0.0f;
        }
        plate_.render(input_.data(), out, n);
        ring_remaining_ -= n;
        if (ring_remaining_ <= 0) active_ = false;
    }

    /// Zero the plate state and mark the voice free (discontinuity reset).
    void reset() {
        plate_.reset();
        pulse_len_ = 0;
        pulse_pos_ = 0;
        ring_remaining_ = 0;
        active_ = false;
    }

    bool active() const noexcept { return active_; }
    uint8_t note() const noexcept { return note_; }
    uint64_t age() const noexcept { return age_; }
    const GongPlate& plate() const noexcept { return plate_; }

private:
    GongPlate plate_;
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
