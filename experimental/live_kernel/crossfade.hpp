#pragma once
//
// Pulp Live Kernel — S0 spike — dual-plan equal-power crossfade.
//
// The kernel holds TWO preallocated Plan slots. On a structural edit the new
// graph blob is decoded + built into the inactive slot (zero-alloc; the delay
// pool is pre-prepared), then swap() arms an equal-power crossfade that renders
// BOTH plans in parallel and blends old->new per-sample — the SAME law as the
// native live plugin swap (LiveSwapCurve::EqualPower, signal_graph.hpp), i.e. the
// shared signal::crossfade_gains equal-power split over a signal::crossfade_
// smoothstep progress ramp. Param-only edits route to set_param on the active
// plan with zero interruption and no rebuild.

#include "codec.hpp"
#include "executor.hpp"

#include <pulp/signal/crossfade.hpp>

#include <cmath>

namespace pulp::live_kernel {

// Equal-power old->new crossfade of one audio block, matching the native
// live-swap law exactly.
//
// Per sample: t = (fade_pos + i) / fade_len; the shared smoothstep ramp shapes t
// to a click-free progress u (zero slope at both ends), and the shared
// equal-power gain split gives (go, gn) = (cos(u*pi/2), sin(u*pi/2)). This is the
// IDENTICAL computation performed by signal::TransitionMixer (float, EqualPower)
// on the native live-swap path, so the kernel's structural swap and the native
// plugin swap fade by the same curve — proven by test_live_kernel_crossfade_null.
//
// NOTE — intended behavior change (SF-2): the previous kernel fade used a LINEAR
// theta (theta = t * pi/2, no smoothstep) while its comment claimed to match the
// native EqualPower law. That was a real divergence: mid-fade the linear-theta
// curve differs from the native smoothstep-shaped curve, so the kernel would
// fail a bit-exact null test against native. Applying the shared smoothstep here
// fixes it. Samples at or past the fade end map through u = smoothstep(1) = 1 to
// the native fade-end gains (cos(pi/2), sin(pi/2)). The old per-block cos/sin
// angle recurrence assumed a CONSTANT dtheta and is no longer valid once the
// smoothstep makes theta non-linear in i, so the fade evaluates the law directly.
inline void equal_power_fade_block(float* dst, const float* ob, const float* nb,
                                    int n, int fade_pos, int fade_len) noexcept {
    for (int i = 0; i < n; ++i) {
        // Division (not reciprocal-multiply) to match signal::TransitionMixer's
        // `t = float(pos) / float(len)` bit-for-bit.
        const float t = static_cast<float>(fade_pos + i) /
                        static_cast<float>(fade_len);
        const float u = pulp::signal::crossfade_smoothstep(t);
        float go, gn;
        pulp::signal::crossfade_gains(u, pulp::signal::CrossfadeGainLaw::EqualPower,
                                      go, gn);
        dst[i] = ob[i] * go + nb[i] * gn;
    }
}

class Kernel {
public:
    void init(double sample_rate, int /*max_block*/) {
        sr_ = sample_rate;
        plan_[0].prepare_pool(sr_);
        plan_[1].prepare_pool(sr_);
        active_ = 0;
        fading_ = false;
        plan_[0].valid = false;
        plan_[1].valid = false;
    }

    // Build a new graph into the inactive slot. Returns DecodeError::Ok or a
    // negative error. Refuses while a fade is in flight (the inactive slot is the
    // fade target). Zero-alloc.
    int load_plan(const uint8_t* bytes, int len) {
        if (fading_) return (int)DecodeError::Ok - 100; // busy
        DecodeError err = decode_plan(bytes, len, desc_);
        if (err != DecodeError::Ok) return (int)err;
        Plan& target = plan_[active_ ^ 1];
        if (!build_plan(target, desc_, sr_)) return -50; // cycle
        return (int)DecodeError::Ok;
    }

    // Arm the crossfade to the freshly-built inactive plan. fade_ms==0 => instant
    // (hard, gap-free switch at the next block boundary).
    void swap(float fade_ms) {
        Plan& target = plan_[active_ ^ 1];
        if (!target.valid) return;
        if (fade_ms <= 0.f) { active_ ^= 1; fading_ = false; return; }
        fade_len_ = (int)(fade_ms * 0.001 * sr_);
        if (fade_len_ < 1) fade_len_ = 1;
        fade_pos_ = 0;
        fading_ = true;
    }

    // Param-only edit path: applied to the active plan with zero interruption.
    // Deliberately does NOT touch the inactive/incoming plan: node indices are
    // per-plan, so writing the same index into a different topology (e.g. mid
    // structural fade) could poke the wrong node. Param edits and structural
    // swaps are distinct operations in v0 (the editor diffs value-vs-structural
    // edits, build plan §M1 item 5); a param edit issued during a fade lands on
    // the currently-audible plan, which is the musically correct target.
    void set_param(int node, int param_id, float value) {
        plan_set_param(plan_[active_], node, param_id, value);
    }

    bool is_fading() const { return fading_; }
    bool active_valid() const { return plan_[active_].valid; }

    // Enable/disable the per-node level tap. Off = measurement mode (CPU numbers
    // free of the meter cost, design §1.5); the worklet leaves it on.
    void set_meter(bool on) { meter_ = on; }

    // Copy the current graph's per-node output RMS into dst (alloc-free readout
    // for the signal-flow graph). While a structural fade is in flight the
    // INCOMING plan is reported — it matches the topology the editor already
    // drew. Returns the node count written (0 if no valid plan).
    int node_levels(float* dst, int max) const {
        const Plan& p = fading_ ? plan_[active_ ^ 1] : plan_[active_];
        if (!p.valid) return 0;
        int count = p.num_nodes < max ? p.num_nodes : max;
        for (int i = 0; i < count; ++i) dst[i] = std::sqrt(p.node_rms[i]); // mean-square → RMS
        return count;
    }

    // Render one block of mono audio into dst. Zero-alloc.
    void process(float* dst, int n) {
        if (n > LK_MAX_BLOCK) n = LK_MAX_BLOCK;
        if (!plan_[active_].valid) { for (int i = 0; i < n; ++i) dst[i] = 0.f; return; }

        if (!fading_) {
            const float* a = render_block(plan_[active_], n, meter_);
            for (int i = 0; i < n; ++i) dst[i] = a[i];
            return;
        }

        Plan& oldp = plan_[active_];
        Plan& newp = plan_[active_ ^ 1];
        const float* ob = render_block(oldp, n, meter_);
        const float* nb = render_block(newp, n, meter_);
        equal_power_fade_block(dst, ob, nb, n, fade_pos_, fade_len_);
        fade_pos_ += n;
        if (fade_pos_ >= fade_len_) { active_ ^= 1; fading_ = false; }
    }

    double sample_rate() const { return sr_; }

private:
    Plan   plan_[2];
    PlanDesc desc_;
    double sr_ = 48000.0;
    int    active_ = 0;
    bool   fading_ = false;
    bool   meter_ = true;
    int    fade_len_ = 1;
    int    fade_pos_ = 0;
};

} // namespace pulp::live_kernel
