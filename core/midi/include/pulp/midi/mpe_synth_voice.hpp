#pragma once

/// @file mpe_synth_voice.hpp
/// Helper base classes for writing MPE-aware synth voices.
///
/// `MpeSynthVoice` owns the per-note expression state (pitch bend,
/// pressure, timbre) with one-pole smoothing applied per-sample, so a
/// subclass only needs to implement note lifecycle and audio rendering.
///
/// `MpeVoiceAllocator<Voice>` routes `MpeExpressionEvent`s from an
/// `MpeBuffer` to a pool of voices, with a configurable voice-steal
/// policy when the pool is exhausted.

#include <pulp/midi/mpe_buffer.hpp>
#include <pulp/midi/mpe_voice_tracker.hpp>
#include <pulp/runtime/budget_policy.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace pulp::midi {

/// Abstract base for a single MPE voice. Subclasses render audio and
/// respond to note lifecycle; expression smoothing is handled here.
class MpeSynthVoice {
public:
    virtual ~MpeSynthVoice() = default;

    /// Called by the allocator when this voice is activated.
    virtual void on_note_on(const MpeNoteState& note) {
        note_ = note;
        active_ = true;
        // Jump targets to initial state; smoothers catch up naturally.
        pitch_bend_target_ = note.pitch_bend_semitones;
        pressure_target_ = note.pressure;
        timbre_target_ = note.timbre;
    }

    /// Called by the allocator when this voice should release.
    virtual void on_note_off() { releasing_ = true; }

    /// Called once per sample before rendering. Advances smoothers.
    void advance_smoothers() {
        pitch_bend_ = smooth(pitch_bend_, pitch_bend_target_);
        pressure_ = smooth(pressure_, pressure_target_);
        timbre_ = smooth(timbre_, timbre_target_);
    }

    /// Render `num_samples` of audio into `out` (additive or replacing —
    /// subclass decides). The allocator calls this once per block per
    /// active voice; subclasses should call `advance_smoothers()` at the
    /// start of each sample or block as appropriate.
    virtual void render(float* out, int num_samples) = 0;

    /// Hard-reset the voice (e.g. on allocation steal).
    virtual void reset() {
        active_ = false;
        releasing_ = false;
        pitch_bend_ = pitch_bend_target_ = 0.0f;
        pressure_ = pressure_target_ = 0.0f;
        timbre_ = timbre_target_ = 0.0f;
        note_ = {};
    }

    // Expression setters — advance_smoothers() ramps current → target.
    void set_pitch_bend(float semitones) { pitch_bend_target_ = semitones; }
    void set_pressure(float p) { pressure_target_ = p; }
    void set_timbre(float t) { timbre_target_ = t; }

    /// Smoothing coefficient in [0, 1). 0 = instant; 0.99 = ~100 sample TC.
    void set_smoothing(float coeff) {
        if (coeff < 0.0f) coeff = 0.0f;
        if (coeff >= 1.0f) coeff = 0.9999f;
        smoothing_ = coeff;
    }

    // Getters for subclasses and the allocator.
    bool active() const { return active_; }
    bool releasing() const { return releasing_; }
    uint32_t note_id() const { return note_.note_id; }
    uint8_t channel() const { return note_.channel; }
    uint8_t note_number() const { return note_.note; }
    uint8_t velocity() const { return note_.velocity; }
    float pitch_bend() const { return pitch_bend_; }
    float pressure() const { return pressure_; }
    float timbre() const { return timbre_; }

protected:
    void finish_release() { active_ = false; releasing_ = false; }

private:
    float smooth(float current, float target) const {
        return smoothing_ * current + (1.0f - smoothing_) * target;
    }

    MpeNoteState note_{};
    bool active_ = false;
    bool releasing_ = false;
    float pitch_bend_ = 0.0f, pitch_bend_target_ = 0.0f;
    float pressure_ = 0.0f, pressure_target_ = 0.0f;
    float timbre_ = 0.0f, timbre_target_ = 0.0f;
    float smoothing_ = 0.99f;
};

/// Voice-steal policy when the pool is full.
enum class MpeVoiceStealMode {
    Oldest,         ///< Steal the voice that started first
    LowestVelocity, ///< Steal the quietest voice
    LowestPitch,    ///< Steal the lowest-note voice
    HighestPitch,   ///< Steal the highest-note voice
};

struct MpeVoiceAllocatorTelemetry {
    std::size_t polyphony = 0;
    std::size_t active_voice_count = 0;
    std::size_t releasing_voice_count = 0;
    std::uint64_t steal_count = 0;
    MpeVoiceStealMode steal_mode = MpeVoiceStealMode::Oldest;
    bool last_was_glide = false;
};

struct MpeVoiceAllocatorRuntimeBudgetReport {
    runtime::RuntimeBudgetDecision decision{};
    runtime::RuntimeBudgetFrameStats frame_stats{};
    MpeVoiceAllocatorTelemetry telemetry{};
    std::uint64_t estimated_cost = 0;

    bool should_run_optional_work() const noexcept {
        return decision.should_run();
    }
};

/// Glide/legato detector — observes MPE note-on events and reports when
/// a new note on the same channel overlaps an existing held note.
class MpeGlideDetector {
public:
    /// Returns true if `note.channel` already has an active note at the
    /// time of a new note-on, signaling a legato/glide gesture.
    bool observe_note_on(const MpeNoteState& note) {
        const bool glide = channel_refcount_[note.channel] > 0;
        ++channel_refcount_[note.channel];
        return glide;
    }
    void observe_note_off(const MpeNoteState& note) {
        if (channel_refcount_[note.channel] > 0) --channel_refcount_[note.channel];
    }
    void reset() { channel_refcount_.fill(0); }
    bool channel_held(uint8_t ch) const { return channel_refcount_[ch] > 0; }
private:
    std::array<uint8_t, 16> channel_refcount_{};
};

/// Routes MPE expression events to a pool of voices of type `Voice`
/// (which must derive from `MpeSynthVoice`). Voices are preallocated.
template<typename Voice>
class MpeVoiceAllocator {
    static_assert(std::is_base_of_v<MpeSynthVoice, Voice>,
                  "Voice must derive from MpeSynthVoice");
public:
    explicit MpeVoiceAllocator(std::size_t polyphony)
        : voices_(polyphony) {}

    std::size_t polyphony() const { return voices_.size(); }
    Voice& voice(std::size_t i) { return voices_[i]; }
    const Voice& voice(std::size_t i) const { return voices_[i]; }

    void set_steal_mode(MpeVoiceStealMode m) { steal_mode_ = m; }
    MpeVoiceStealMode steal_mode() const { return steal_mode_; }

    /// Dispatch a single MPE expression event to the voice pool.
    void dispatch(const MpeExpressionEvent& e) {
        switch (e.kind) {
            case MpeExpressionEvent::Kind::NoteOn: {
                const bool glide = glide_detector_.observe_note_on(e.state);
                last_was_glide_ = glide;
                Voice* v = pick_free_voice();
                if (!v) {
                    v = steal_voice();
                    // Stealing ends the stolen note's lifetime; decrement
                    // the glide refcount so later note-ons on its channel
                    // aren't misclassified as legato. Only a still-HELD
                    // voice (active && !releasing) still owns its channel
                    // refcount here — a releasing voice already had its
                    // NoteOff processed (refcount decremented in the
                    // NoteOff case), so decrementing again would
                    // double-count and zero the count the just-placed
                    // note bumped.
                    if (v && v->active() && !v->releasing()) {
                        MpeNoteState released{};
                        released.channel = v->channel();
                        released.note = v->note_number();
                        released.note_id = v->note_id();
                        glide_detector_.observe_note_off(released);
                    }
                }
                if (v) {
                    v->reset();
                    v->on_note_on(e.state);
                    age_[static_cast<std::size_t>(v - voices_.data())] = ++age_counter_;
                } else {
                    // No voice could be placed (zero polyphony). Balance
                    // the unconditional observe_note_on() increment above
                    // so the channel doesn't read glide-held forever, and
                    // report no glide since nothing actually sounds.
                    glide_detector_.observe_note_off(e.state);
                    last_was_glide_ = false;
                }
                break;
            }
            case MpeExpressionEvent::Kind::NoteOff: {
                // Only decrement the glide refcount if the note is still
                // tracked. Stolen notes were already retired (and their
                // channel refcount decremented) in the NoteOn steal path;
                // decrementing here again would double-count.
                if (Voice* v = find_voice_by_id(e.state.note_id)) {
                    glide_detector_.observe_note_off(e.state);
                    v->on_note_off();
                }
                break;
            }
            case MpeExpressionEvent::Kind::PitchBend:
                if (Voice* v = find_voice_by_id(e.state.note_id))
                    v->set_pitch_bend(e.state.pitch_bend_semitones);
                break;
            case MpeExpressionEvent::Kind::Pressure:
                if (Voice* v = find_voice_by_id(e.state.note_id))
                    v->set_pressure(e.state.pressure);
                break;
            case MpeExpressionEvent::Kind::Timbre:
                if (Voice* v = find_voice_by_id(e.state.note_id))
                    v->set_timbre(e.state.timbre);
                break;
        }
    }

    /// Dispatch all events in a buffer (in the order they appear).
    void dispatch_all(const MpeBuffer& buf) {
        for (const auto& e : buf) dispatch(e);
    }

    std::size_t active_count() const {
        std::size_t n = 0;
        for (const auto& v : voices_) if (v.active()) ++n;
        return n;
    }

    std::size_t releasing_count() const {
        std::size_t n = 0;
        for (const auto& v : voices_) if (v.active() && v.releasing()) ++n;
        return n;
    }

    std::uint64_t steal_count() const {
        return steal_count_.load(std::memory_order_relaxed);
    }

    void reset_steal_count() {
        steal_count_.store(0, std::memory_order_relaxed);
    }

    /// Cheap owner-thread snapshot for voice-count telemetry. Call from
    /// the allocator owner (normally the audio thread) and publish the
    /// returned value through a lock-free latest-value channel if another
    /// thread needs to observe it.
    MpeVoiceAllocatorTelemetry telemetry() const {
        return {
            .polyphony = polyphony(),
            .active_voice_count = active_count(),
            .releasing_voice_count = releasing_count(),
            .steal_count = steal_count(),
            .steal_mode = steal_mode_,
            .last_was_glide = last_was_glide_,
        };
    }

    std::uint64_t estimate_optional_runtime_cost() const {
        const auto t = telemetry();
        std::uint64_t cost = 0;
        cost = saturating_add_u64_(
            cost, saturating_mul_u64_(
                static_cast<std::uint64_t>(t.polyphony), 4));
        cost = saturating_add_u64_(
            cost, saturating_mul_u64_(
                static_cast<std::uint64_t>(t.active_voice_count), 64));
        cost = saturating_add_u64_(
            cost, saturating_mul_u64_(
                static_cast<std::uint64_t>(t.releasing_voice_count), 32));
        return cost;
    }

    MpeVoiceAllocatorRuntimeBudgetReport evaluate_optional_runtime_budget(
        runtime::RuntimeBudgetFrame& frame,
        runtime::RuntimeWorkLane lane = runtime::RuntimeWorkLane::Background,
        bool required = false) const {
        const auto t = telemetry();
        const auto cost = estimate_optional_runtime_cost();
        const auto decision = frame.evaluate(lane, cost, required);
        return {
            .decision = decision,
            .frame_stats = frame.stats(),
            .telemetry = t,
            .estimated_cost = cost,
        };
    }

    bool last_was_glide() const { return last_was_glide_; }

    void reset_all() {
        for (auto& v : voices_) v.reset();
        glide_detector_.reset();
        last_was_glide_ = false;
        age_counter_ = 0;
    }

private:
    static std::uint64_t saturating_add_u64_(std::uint64_t a,
                                             std::uint64_t b) {
        const auto max = std::numeric_limits<std::uint64_t>::max();
        return b > max - a ? max : a + b;
    }

    static std::uint64_t saturating_mul_u64_(std::uint64_t a,
                                             std::uint64_t b) {
        const auto max = std::numeric_limits<std::uint64_t>::max();
        if (a == 0 || b == 0) return 0;
        return a > max / b ? max : a * b;
    }

    Voice* pick_free_voice() {
        for (auto& v : voices_) if (!v.active()) return &v;
        return nullptr;
    }

    Voice* steal_voice() {
        if (voices_.empty()) return nullptr;
        auto cmp = [this](const Voice& a, const Voice& b) -> bool {
            switch (steal_mode_) {
                case MpeVoiceStealMode::Oldest:
                    return age_[&a - voices_.data()] < age_[&b - voices_.data()];
                case MpeVoiceStealMode::LowestVelocity:
                    return a.velocity() < b.velocity();
                case MpeVoiceStealMode::LowestPitch:
                    return a.note_number() < b.note_number();
                case MpeVoiceStealMode::HighestPitch:
                    return a.note_number() > b.note_number();
            }
            return false;
        };
        Voice* target = &voices_[0];
        for (auto& v : voices_) if (cmp(v, *target)) target = &v;
        steal_count_.fetch_add(1, std::memory_order_relaxed);
        return target;
    }

    Voice* find_voice_by_id(uint32_t id) {
        for (auto& v : voices_) {
            if (v.active() && v.note_id() == id) return &v;
        }
        return nullptr;
    }

    std::vector<Voice> voices_;
    std::vector<uint64_t> age_ = std::vector<uint64_t>(voices_.size(), 0);
    uint64_t age_counter_ = 0;
    MpeVoiceStealMode steal_mode_ = MpeVoiceStealMode::Oldest;
    MpeGlideDetector glide_detector_;
    bool last_was_glide_ = false;
    std::atomic<std::uint64_t> steal_count_{0};
};

} // namespace pulp::midi
