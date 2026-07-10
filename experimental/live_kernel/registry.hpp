#pragma once
//
// Pulp Live Kernel — S0 spike — node registry.
//
// The registry instantiates the ACTUAL core/signal C++ node classes behind a
// uniform (init / set_param / process / reset) surface, keyed by NodeType. This
// is the whole point of the kernel-VM approach: what the browser worklet renders
// is the SAME DSP the native Pulp plugins compute — not a re-implementation.
//
// RT contract: set_param and process allocate no memory. The only class that
// needs prepare-time allocation is DelayLineT (buffer_.assign); the executor
// owns a pool of DelayLineT prepared ONCE at kernel init (off the steady-state
// audio path) and binds them here, so process()/set_param never allocate.

#include "codec.hpp"

#include <pulp/signal/gain.hpp>          // GainT, SimpleMixerT
#include <pulp/signal/oscillator.hpp>    // OscillatorT
#include <pulp/signal/adsr.hpp>          // AdsrT
#include <pulp/signal/biquad.hpp>        // BiquadT
#include <pulp/signal/ladder_filter.hpp> // LadderFilterT
#include <pulp/signal/delay_line.hpp>    // DelayLineT

#include <algorithm>
#include <cmath>

namespace pulp::live_kernel {

using Osc    = pulp::signal::OscillatorT<float>;
using Gain   = pulp::signal::GainT<float>;
using Biquad = pulp::signal::BiquadT<float>;
using Ladder = pulp::signal::LadderFilterT<float>;
using Adsr   = pulp::signal::AdsrT<float>;
using Delay  = pulp::signal::DelayLineT<float>;
using Mixer  = pulp::signal::SimpleMixerT<float>;

// Number of input / output ports implied by a node type (authoritative — the
// wire table's num_in/num_out are informational and validated against these).
inline void ports_for(NodeType t, int& num_in, int& num_out) {
    switch (t) {
        case NodeType::Oscillator: num_in = 0; num_out = 1; break;
        case NodeType::Mixer:      num_in = 2; num_out = 1; break;
        default:                   num_in = 1; num_out = 1; break;
    }
}

// One node's live state: every class inlined (all tiny, alloc-free) except the
// delay ring, which is bound from the executor's pre-prepared pool.
struct NodeInstance {
    Osc    osc;
    Gain   gain;
    Biquad biquad;
    Ladder ladder;
    Adsr   adsr;
    Mixer  mixer;
    Delay* delay = nullptr; // bound from the plan's pool (Delay nodes only)

    // Cached params so a set_param on one field recomputes only what it must.
    float  osc_amp   = 0.3f;
    // biquad
    int    bq_type   = 0;      // 0 lowpass..7 high_shelf
    float  bq_cutoff = 1000.f;
    float  bq_q      = 0.707f;
    float  bq_gain   = 0.f;
    // adsr
    Adsr::Params adsr_p{};
    bool   gate_on   = false;
    // delay
    float  dl_time_s = 0.25f;
    float  dl_fb     = 0.f;
    float  dl_mix    = 0.5f;
    float  dl_samps  = 0.f;    // cached time in samples (clamped)
};

// (Re)initialise a node to its defaults for a fresh plan bind. Alloc-free:
// DelayLineT::reset() only zeroes an already-allocated ring.
inline void init_node(NodeInstance& ni, NodeType t, double sr) {
    switch (t) {
        case NodeType::Oscillator:
            ni.osc.reset();
            ni.osc.set_sample_rate((float)sr);
            ni.osc.set_frequency(220.f);
            ni.osc.set_waveform(Osc::Waveform::saw);
            ni.osc_amp = 0.3f;
            break;
        case NodeType::Gain:
            ni.gain.set_gain_db(0.f);
            break;
        case NodeType::Biquad:
            ni.bq_type = 0; ni.bq_cutoff = 1000.f; ni.bq_q = 0.707f; ni.bq_gain = 0.f;
            ni.biquad.reset();
            ni.biquad.set_coefficients((Biquad::Type)ni.bq_type, ni.bq_cutoff,
                                       ni.bq_q, (float)sr, ni.bq_gain);
            break;
        case NodeType::Ladder:
            ni.ladder.reset();
            ni.ladder.set_sample_rate((float)sr);
            ni.ladder.set_frequency(1000.f);
            ni.ladder.set_resonance(0.3f);
            break;
        case NodeType::Adsr:
            ni.adsr.reset();
            ni.adsr.set_sample_rate((float)sr);
            ni.adsr_p = Adsr::Params{0.005f, 0.12f, 0.6f, 0.3f};
            ni.adsr.set_params(ni.adsr_p);
            ni.gate_on = false;
            break;
        case NodeType::Delay:
            ni.dl_time_s = 0.25f; ni.dl_fb = 0.f; ni.dl_mix = 0.5f;
            ni.dl_samps = std::clamp(ni.dl_time_s * (float)sr, 1.f,
                                     ni.delay ? (float)ni.delay->max_delay() : 1.f);
            if (ni.delay) ni.delay->reset();
            break;
        case NodeType::Mixer:
            ni.mixer.set_mix(0.5f);
            break;
        default: break;
    }
}

// Apply one parameter edit to a live node. Zero-alloc; audio-thread safe.
inline void set_node_param(NodeInstance& ni, NodeType t, int param_id,
                           float v, double sr) {
    switch (t) {
        case NodeType::Oscillator:
            if (param_id == 0) ni.osc.set_frequency(v);
            else if (param_id == 1) ni.osc.set_waveform((Osc::Waveform)(int)v);
            else if (param_id == 2) ni.osc_amp = v;
            break;
        case NodeType::Gain:
            if (param_id == 0) ni.gain.set_gain_db(v);
            break;
        case NodeType::Biquad:
            if (param_id == 0) ni.bq_type = (int)v;
            else if (param_id == 1) ni.bq_cutoff = v;
            else if (param_id == 2) ni.bq_q = v;
            else if (param_id == 3) ni.bq_gain = v;
            ni.biquad.set_coefficients((Biquad::Type)ni.bq_type, ni.bq_cutoff,
                                       ni.bq_q, (float)sr, ni.bq_gain);
            break;
        case NodeType::Ladder:
            if (param_id == 0) ni.ladder.set_frequency(v);
            else if (param_id == 1) ni.ladder.set_resonance(v);
            break;
        case NodeType::Adsr:
            if (param_id == 0) ni.adsr_p.attack = v;
            else if (param_id == 1) ni.adsr_p.decay = v;
            else if (param_id == 2) ni.adsr_p.sustain = v;
            else if (param_id == 3) ni.adsr_p.release = v;
            else if (param_id == 4) {
                bool want = v >= 0.5f;
                if (want && !ni.gate_on) ni.adsr.note_on();
                else if (!want && ni.gate_on) ni.adsr.note_off();
                ni.gate_on = want;
            }
            if (param_id <= 3) ni.adsr.set_params(ni.adsr_p);
            break;
        case NodeType::Delay:
            if (param_id == 0) ni.dl_time_s = v;
            else if (param_id == 1) ni.dl_fb = std::clamp(v, 0.f, 0.99f);
            else if (param_id == 2) ni.dl_mix = std::clamp(v, 0.f, 1.f);
            ni.dl_samps = std::clamp(ni.dl_time_s * (float)sr, 1.f,
                                     ni.delay ? (float)ni.delay->max_delay() : 1.f);
            break;
        case NodeType::Mixer:
            if (param_id == 0) ni.mixer.set_mix(v);
            break;
        default: break;
    }
}

// Render one block. `ins[p]` is the already-gathered (summed) buffer for input
// port p; `out` is this node's single output buffer. Zero-alloc.
inline void process_node(NodeInstance& ni, NodeType t,
                         const float* const* ins, int num_in,
                         float* out, int n) {
    switch (t) {
        case NodeType::Oscillator: {
            const float a = ni.osc_amp;
            for (int i = 0; i < n; ++i) out[i] = ni.osc.next() * a;
            break;
        }
        case NodeType::Gain: {
            const float* in = ins[0];
            for (int i = 0; i < n; ++i) out[i] = in[i];
            ni.gain.process(out, n);
            break;
        }
        case NodeType::Biquad: {
            const float* in = ins[0];
            for (int i = 0; i < n; ++i) out[i] = ni.biquad.process(in[i]);
            break;
        }
        case NodeType::Ladder: {
            const float* in = ins[0];
            for (int i = 0; i < n; ++i) out[i] = ni.ladder.process(in[i]);
            break;
        }
        case NodeType::Adsr: {
            const float* in = ins[0];
            for (int i = 0; i < n; ++i) out[i] = in[i];
            ni.adsr.apply_to_buffer(out, 0, n); // multiply by envelope
            break;
        }
        case NodeType::Delay: {
            const float* in = ins[0];
            const float ds = ni.dl_samps, fb = ni.dl_fb, mix = ni.dl_mix;
            Delay* dl = ni.delay;
            for (int i = 0; i < n; ++i) {
                float wet = dl->read(ds);
                dl->push(in[i] + fb * wet);
                out[i] = in[i] * (1.f - mix) + wet * mix;
            }
            break;
        }
        case NodeType::Mixer: {
            const float* dry = ins[0];
            const float* wet = (num_in > 1) ? ins[1] : ins[0];
            ni.mixer.process(dry, wet, out, n);
            break;
        }
        default:
            for (int i = 0; i < n; ++i) out[i] = 0.f;
            break;
    }
}

} // namespace pulp::live_kernel
