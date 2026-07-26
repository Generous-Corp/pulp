#pragma once

#include <pulp/signal/drum/output_stage.hpp>
#include <pulp/signal/drum/velocity.hpp>

#include <algorithm>
#include <cstddef>

namespace pulp::signal::drum {

/// Public lifecycle inspection for a drum voice. This is intentionally not a
/// serializable DSP snapshot: voices own heterogeneous filter, delay, and RNG
/// state, so pretending one base blob can restore them would create a false
/// portability contract. Plugin parameter/state persistence belongs above the
/// voice; this value is for allocation, UI, and choke-policy decisions.
enum class VoiceState {
    idle,
    ringing,
    choking,
};

/// The lifecycle every percussion voice shares.
///
/// Three parts of that lifecycle are implemented here rather than left to each
/// voice, because each of them is a correctness rule that is easy to get
/// subtly wrong in a way nobody notices until a specific arrangement exposes
/// it:
///
/// * **`process()` adds.** A voice writes into a buffer that may already hold
///   another voice's output, so a kit is a sum rather than a mixing pass with
///   its own scratch buffers. A voice that assigned instead of added would
///   silence whichever voice rendered before it, which sounds like a choke
///   group misfiring and is hard to trace back to the assignment.
///
/// * **Choking fades.** A closed hi-hat cutting an open one is an amplitude
///   step, and a step is broadband: cutting a ringing voice instantly puts a
///   click exactly where the new hit lands, which reads as a bad sample rather
///   than as a choke. The base class renders through a short linear fade so
///   every voice gets this without implementing it.
///
/// * **Velocity reaches timbre.** `note_on` stores the velocity and the
///   response curve together, so a voice implementation has the timbral terms
///   in hand at the point it needs them and cannot accidentally wire velocity
///   to gain alone.
///
/// A voice implements the four `on_*` hooks and `render_add`. It must not
/// override the public methods; they carry the rules above.
///
/// RT contract: `prepare()` may allocate in a derived voice; everything else,
/// including `process()`, `note_on()`, and `choke()`, allocates nothing and
/// takes no locks.
class Voice {
public:
    virtual ~Voice() = default;

    Voice() = default;
    Voice(const Voice&) = delete;
    Voice& operator=(const Voice&) = delete;

    /// Fixes the sample rate and clears all state. The only method a derived
    /// voice may allocate in.
    void prepare(double sample_rate) {
        sample_rate_ = sample_rate > 0.0 ? sample_rate : sample_rate_;
        on_prepare(sample_rate_);
        reset();
    }

    /// Silences the voice immediately, without a fade. For transport stops and
    /// buffer-size changes, where a click cannot be heard because nothing is
    /// playing either side of it.
    void reset() {
        choking_ = false;
        choke_gain_ = 1.0f;
        on_reset();
    }

    /// Starts a hit. `velocity` is in [0, 1]; values outside are clamped.
    void note_on(float velocity) {
        choking_ = false;
        choke_gain_ = 1.0f;
        velocity_ = std::clamp(velocity, 0.0f, 1.0f);
        on_note_on(velocity_);
    }

    /// Fades the voice out over `fade_ms` and then silences it. Used by choke
    /// groups; a shorter fade is more abrupt but never clicks the way an
    /// instant cut does.
    void choke(float fade_ms = 4.0f) {
        if (!is_active()) return;
        const double samples = std::max(0.001 * static_cast<double>(fade_ms) * sample_rate_, 1.0);
        choke_step_ = static_cast<float>(1.0 / samples);
        choking_ = true;
    }

    /// Whether the voice still has something to render. A kit uses this to
    /// skip voices entirely rather than summing silence.
    bool is_active() const { return on_is_active(); }

    VoiceState state() const {
        if (!on_is_active()) return VoiceState::idle;
        return choking_ ? VoiceState::choking : VoiceState::ringing;
    }

    float velocity() const { return velocity_; }
    double sample_rate() const { return sample_rate_; }

    void set_velocity_response(const VelocityResponse& r) { velocity_response_ = r; }
    const VelocityResponse& velocity_response() const { return velocity_response_; }

    /// Adds `num_samples` of this voice's output into `out`.
    void process(float* out, int num_samples) {
        if (out == nullptr || num_samples <= 0 || !is_active()) return;

        if (!choking_) {
            render_add(out, num_samples);
            return;
        }

        // A fade cannot be applied to an additive render in place, because the
        // buffer may already hold another voice. Render into a fixed-size
        // scratch in chunks, scale, and accumulate -- which keeps the fade
        // exact without allocating.
        int done = 0;
        while (done < num_samples) {
            const int n = std::min(kScratchSamples, num_samples - done);
            std::fill(scratch_, scratch_ + n, 0.0f);
            render_add(scratch_, n);
            for (int i = 0; i < n; ++i) {
                out[done + i] += scratch_[i] * choke_gain_;
                choke_gain_ = std::max(choke_gain_ - choke_step_, 0.0f);
            }
            done += n;
            if (choke_gain_ <= 0.0f) break;
        }

        if (choke_gain_ <= 0.0f) reset();
    }

    /// Adds a stereo rendering to `left` and `right`.
    ///
    /// Voices that do not provide a stereo realization are placed in the
    /// centre with half their mono signal in each channel, so summing the two
    /// channels reproduces `process()` exactly. A stereo-capable voice may
    /// override `render_add_stereo`, but the shared choke fade remains here.
    void process_stereo(float* left, float* right, int num_samples) {
        if (left == nullptr || right == nullptr || num_samples <= 0 ||
            !is_active())
            return;

        if (!choking_) {
            render_add_stereo(left, right, num_samples);
            return;
        }

        int done = 0;
        while (done < num_samples) {
            const int n = std::min(kScratchSamples, num_samples - done);
            std::fill(stereo_left_scratch_, stereo_left_scratch_ + n, 0.0f);
            std::fill(stereo_right_scratch_, stereo_right_scratch_ + n, 0.0f);
            render_add_stereo(stereo_left_scratch_, stereo_right_scratch_, n);
            for (int i = 0; i < n; ++i) {
                left[done + i] += stereo_left_scratch_[i] * choke_gain_;
                right[done + i] += stereo_right_scratch_[i] * choke_gain_;
                choke_gain_ = std::max(choke_gain_ - choke_step_, 0.0f);
            }
            done += n;
            if (choke_gain_ <= 0.0f) break;
        }

        if (choke_gain_ <= 0.0f) reset();
    }

protected:
    /// Sizes buffers and fixes coefficients. The only hook allowed to
    /// allocate.
    virtual void on_prepare(double sample_rate) = 0;

    /// Clears every piece of state the voice carries, so `is_active()` becomes
    /// false and the next render starts from silence.
    virtual void on_reset() = 0;

    /// Starts a hit at the given velocity, already clamped to [0, 1].
    virtual void on_note_on(float velocity) = 0;

    virtual bool on_is_active() const = 0;

    /// Adds the voice's output into `out`. Must add, never assign.
    virtual void render_add(float* out, int num_samples) = 0;

    /// Optional stereo realization. The default is centre-panned and preserves
    /// exact mono-sum equivalence.
    virtual void render_add_stereo(float* left, float* right,
                                   int num_samples) {
        int done = 0;
        while (done < num_samples) {
            const int n = std::min(kScratchSamples, num_samples - done);
            std::fill(mono_stereo_scratch_, mono_stereo_scratch_ + n, 0.0f);
            render_add(mono_stereo_scratch_, n);
            for (int i = 0; i < n; ++i) {
                const float centred = 0.5f * mono_stereo_scratch_[i];
                left[done + i] += centred;
                right[done + i] += centred;
            }
            done += n;
        }
    }

private:
    // 256 samples is longer than any block a plugin host is likely to ask for
    // in the low-latency configurations where choking matters, and small
    // enough that a whole kit's scratch stays inside a cache level.
    static constexpr int kScratchSamples = 256;

    double sample_rate_ = 44100.0;
    float velocity_ = 1.0f;
    VelocityResponse velocity_response_{};

    bool choking_ = false;
    float choke_gain_ = 1.0f;
    float choke_step_ = 1.0f;
    float scratch_[kScratchSamples]{};
    float mono_stereo_scratch_[kScratchSamples]{};
    float stereo_left_scratch_[kScratchSamples]{};
    float stereo_right_scratch_[kScratchSamples]{};
};

}  // namespace pulp::signal::drum
