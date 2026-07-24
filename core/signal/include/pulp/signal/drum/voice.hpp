#pragma once

#include <pulp/signal/lofi_chain.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace pulp::signal::drum {

/// How hard a hit changes the sound, beyond changing how loud it is.
///
/// Velocity that only scales amplitude is the single most common reason a
/// synthesised drum sounds synthetic. A real drum struck harder is not the
/// same sound turned up: the head deflects further so the pitch bends further
/// before settling, the stick excites more high partials, and the balance
/// between the body and the noise the strike makes shifts toward the noise.
/// Turning one knob cannot reproduce that, which is why this struct exists and
/// why every voice in this namespace consumes it rather than reading velocity
/// directly.
///
/// All four responses are zero-cost when left at their defaults except
/// `level_db`, so a voice that genuinely wants level-only velocity says so by
/// leaving the others at zero rather than by omitting the wiring.
struct VelocityResponse {
    /// Attenuation at velocity 0, in decibels below a full-velocity hit.
    float level_db = 18.0f;

    /// Extra pitch-envelope depth at full velocity, in octaves. A harder
    /// strike deflects the head further, so the pitch starts higher and falls
    /// through a wider range.
    float bend_octaves = 0.0f;

    /// Extra exciter brightness at full velocity, in octaves of filter corner.
    float brightness_octaves = 0.0f;

    /// Shift toward the noise layer at full velocity, 0 to 1. A harder strike
    /// puts proportionally more energy into the transient than into the body.
    float noise_balance = 0.0f;

    /// Amplitude multiplier for `velocity` in [0, 1].
    float gain(float velocity) const {
        const float v = std::clamp(velocity, 0.0f, 1.0f);
        return std::pow(10.0f, (level_db * (v - 1.0f)) / 20.0f);
    }

    /// Pitch-envelope depth to add, in octaves.
    float bend(float velocity) const {
        return bend_octaves * std::clamp(velocity, 0.0f, 1.0f);
    }

    /// Multiplier to apply to an exciter or filter frequency.
    float brightness_scale(float velocity) const {
        return std::exp2(brightness_octaves * std::clamp(velocity, 0.0f, 1.0f));
    }

    /// Amount to add to a body-versus-noise balance in [0, 1].
    float noise_shift(float velocity) const {
        return noise_balance * std::clamp(velocity, 0.0f, 1.0f);
    }
};

/// The output stage every percussion voice ends with: saturate, then degrade,
/// then set the level.
///
/// The order is not arbitrary and is not left to each voice to remember.
/// Saturation before quantisation means the quantiser sees a signal that
/// already fills its range, which is what makes a low bit depth sound like a
/// drum machine rather than like a fault; the reverse order quantises a small
/// signal and then amplifies its error. The output level is applied last so it
/// is a clean gain and does not change how hard the voice drives its own
/// distortion.
///
/// RT contract: `prepare()` and the setters allocate nothing. `process()`
/// allocates nothing and takes no locks.
template <typename SampleType = float>
class OutputStageT {
public:
    void prepare(double sample_rate) {
        lofi_.set_sample_rate(sample_rate);
        reset();
    }

    void reset() { lofi_.reset(); }

    /// Saturation amount, 0 (clean) to 1. Maps to a pre-gain of 1 to 10 into a
    /// tanh, so the control reaches obvious distortion without the top of its
    /// range being a dead zone.
    void set_drive(double amount) { drive_ = std::clamp(amount, 0.0, 1.0); }

    /// Wavefolding amount, 0 to 1. Folding runs before saturation because a
    /// folder generates the partials and the saturator then limits them; the
    /// other way round the limiter removes what the folder was for.
    void set_fold(double amount) { fold_ = std::clamp(amount, 0.0, 1.0); }

    /// Output gain, linear.
    void set_level(double level) { level_ = std::max(level, 0.0); }

    /// The degradation stages, exposed so a voice can configure bit depth,
    /// hold rate, and dead zone without this class proxying five setters.
    LofiChainT<SampleType>& lofi() { return lofi_; }
    const LofiChainT<SampleType>& lofi() const { return lofi_; }

    SampleType process(SampleType input) {
        double x = static_cast<double>(input);
        if (fold_ > 0.0) {
            const double k = 1.0 + fold_ * 4.0;
            x = std::sin(0.5 * 3.14159265358979323846 * k * x);
        }
        if (drive_ > 0.0) {
            x = std::tanh((1.0 + drive_ * 9.0) * x);
        }
        x = static_cast<double>(lofi_.process(static_cast<SampleType>(x)));
        return static_cast<SampleType>(x * level_);
    }

private:
    LofiChainT<SampleType> lofi_;
    double drive_ = 0.0;
    double fold_ = 0.0;
    double level_ = 1.0;
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
};

using OutputStage = OutputStageT<float>;
using OutputStage64 = OutputStageT<double>;

}  // namespace pulp::signal::drum
