#pragma once

#include <pulp/signal/drum/voice.hpp>

#include <algorithm>
#include <cstddef>
#include <vector>

namespace pulp::signal::drum {

/// A set of percussion voices summed into one output, with choke groups.
///
/// The kit does two things a caller would otherwise repeat for every project.
///
/// **It sums.** Voices render additively, so mixing is a loop rather than a
/// pass with scratch buffers, and a voice that has finished is skipped
/// entirely rather than contributing silence. On a sparse pattern most voices
/// are idle most of the time, so that skip is most of the CPU saving a kit
/// gets.
///
/// **It chokes.** A closed hi-hat and an open hi-hat are one pair of cymbals:
/// physically they cannot both ring, because closing the pedal stops the
/// ringing one. Every drum machine reproduces this, and it is the only place
/// in a kit where triggering one voice must silence another. Voices sharing a
/// non-zero choke group cut each other; group 0 means "chokes nothing", which
/// is the default so that a kit assembled without thinking about it behaves
/// like independent drums.
///
/// The choke is a short fade rather than a cut -- see `Voice::choke` -- so the
/// cymbal stops without a click where the closing hit lands.
///
/// The kit does not own its voices. A caller keeps its voices as concrete
/// types so it can reach their specific controls, and registers pointers here;
/// the kit only needs the lifecycle every voice shares. Registered voices must
/// outlive the kit.
///
/// RT contract: `add_voice()` and `prepare()` may allocate and are setup-time
/// calls. `trigger()`, `choke_group()`, `process()`, and `reset()` allocate
/// nothing and take no locks.
class Kit {
public:
    /// Registers a voice under `note`, which is whatever key a caller wants to
    /// address it by -- a MIDI note number is the usual choice. `choke_group`
    /// of 0 means the voice neither chokes others nor is choked.
    ///
    /// Returns the slot index, which is how a caller addresses the voice
    /// without a lookup on the audio thread.
    std::size_t add_voice(Voice* voice, int note, int choke_group = 0) {
        slots_.push_back({voice, note, choke_group});
        // Prepare on registration as well as in `prepare()`. Registering after
        // the kit was prepared is a reasonable thing to do -- a caller may add
        // a voice in response to a preset load -- and without this the new
        // voice would run at whatever rate it was constructed with, which is
        // audible as a wrongly-pitched drum rather than as an error.
        if (voice != nullptr) {
            set_output_oversampling(oversampling_);
            voice->prepare(sample_rate_);
        }
        return slots_.size() - 1;
    }

    std::size_t voice_count() const { return slots_.size(); }

    /// One latency for the whole summed kit. Individual voice quality changes
    /// are normalized back to this choice before triggering or rendering, so
    /// simultaneous layers remain sample-aligned.
    int latency_samples() const noexcept {
        return OutputStage::latency_samples_for(oversampling_);
    }

    OutputOversampling output_oversampling() const noexcept {
        return oversampling_;
    }

    void set_output_oversampling(OutputOversampling factor) {
        oversampling_ = factor;
        for (auto& slot : slots_) {
            if (slot.voice != nullptr)
                slot.voice->set_output_oversampling(oversampling_);
        }
        for (const auto& slot : slots_) {
            if (slot.voice != nullptr &&
                slot.voice->output_oversampling() != oversampling_) {
                // A source-compatible legacy Voice has no quality stage. Keep
                // the entire sum honest and aligned by falling back to the
                // zero-latency contract it reports.
                oversampling_ = OutputOversampling::bypass;
                for (auto& fallback_slot : slots_) {
                    if (fallback_slot.voice != nullptr)
                        fallback_slot.voice->set_output_oversampling(
                            oversampling_);
                }
                break;
            }
        }
    }

    void prepare(double sample_rate) {
        sample_rate_ = sample_rate > 0.0 ? sample_rate : sample_rate_;
        for (auto& slot : slots_) {
            if (slot.voice != nullptr) {
                slot.voice->set_output_oversampling(oversampling_);
                slot.voice->prepare(sample_rate_);
            }
        }
    }

    /// How long a choked voice takes to fade out, in milliseconds.
    void set_choke_fade_ms(float ms) { choke_fade_ms_ = std::max(ms, 0.1f); }

    /// Silences every voice immediately, without fades. For transport stops.
    void reset() {
        for (auto& slot : slots_) {
            if (slot.voice != nullptr) slot.voice->reset();
        }
    }

    /// Triggers the voice registered under `note`, choking any other voice in
    /// the same group first. Unknown notes are ignored, so a kit can be driven
    /// from a full MIDI stream without the caller filtering it.
    void trigger(int note, float velocity) {
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            if (slots_[i].note == note) {
                trigger_slot(i, velocity);
                return;
            }
        }
    }

    /// Triggers by slot index, which a caller that kept the index from
    /// `add_voice` uses to avoid searching on the audio thread.
    void trigger_slot(std::size_t index, float velocity) {
        if (index >= slots_.size() || slots_[index].voice == nullptr) return;

        slots_[index].voice->set_output_oversampling(oversampling_);
        const int group = slots_[index].choke_group;
        if (group != 0) {
            for (std::size_t other = 0; other < slots_.size(); ++other) {
                if (other == index || slots_[other].choke_group != group) continue;
                if (slots_[other].voice != nullptr) {
                    slots_[other].voice->choke(choke_fade_ms_);
                }
            }
        }
        slots_[index].voice->note_on(velocity);
    }

    /// Fades out every voice in `group`. Exposed because a pedal-up event is a
    /// choke with no accompanying hit, which `trigger` cannot express.
    void choke_group(int group) {
        if (group == 0) return;
        for (auto& slot : slots_) {
            if (slot.choke_group == group && slot.voice != nullptr) {
                slot.voice->choke(choke_fade_ms_);
            }
        }
    }

    /// Whether any voice is still rendering.
    bool is_active() const {
        for (const auto& slot : slots_) {
            if (slot.voice != nullptr && slot.voice->is_active()) return true;
        }
        return false;
    }

    /// Adds every active voice's output into `out`. Like a voice, this adds
    /// rather than assigns, so a kit can share a bus with anything else.
    void process(float* out, int num_samples) {
        if (out == nullptr || num_samples <= 0) return;
        for (auto& slot : slots_) {
            if (slot.voice != nullptr && slot.voice->is_active()) {
                slot.voice->set_output_oversampling(oversampling_);
                slot.voice->process(out, num_samples);
            }
        }
    }

private:
    struct Slot {
        Voice* voice = nullptr;
        int note = 0;
        int choke_group = 0;
    };

    std::vector<Slot> slots_;
    double sample_rate_ = 44100.0;
    float choke_fade_ms_ = 4.0f;
    OutputOversampling oversampling_ = OutputOversampling::x2;
};

}  // namespace pulp::signal::drum
