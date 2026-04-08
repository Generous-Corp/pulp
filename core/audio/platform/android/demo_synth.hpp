#pragma once
// Demo synth for Android — reads parameters from shared atomics,
// produces audio via Oboe. Lock-free: all parameter access is relaxed atomic.

#include <atomic>
#include <cstdint>

namespace pulp::demo {

// Shared parameter block — written by UI thread, read by audio thread.
// All values are 0..1 normalized unless noted.
struct SynthParams {
    // Oscillator
    std::atomic<float> osc_pitch{0.5f};     // 0=low, 1=high (mapped to Hz)
    std::atomic<float> osc_detune{0.3f};    // 0=none, 1=wide
    std::atomic<float> osc_mix{0.5f};       // 0=saw, 1=square
    std::atomic<float> osc_level{0.15f};    // output level

    // Filter
    std::atomic<float> filter_cutoff{0.65f};  // 0=20Hz, 1=20kHz
    std::atomic<float> filter_reso{0.35f};    // 0=none, 1=self-osc
    std::atomic<float> filter_env{0.5f};      // envelope amount

    // Envelope (ADSR)
    std::atomic<float> env_attack{0.05f};
    std::atomic<float> env_decay{0.3f};
    std::atomic<float> env_sustain{0.7f};
    std::atomic<float> env_release{0.4f};

    // Mixer faders (4 channels)
    std::atomic<float> mix1{0.75f};
    std::atomic<float> mix2{0.6f};
    std::atomic<float> mix3{0.4f};
    std::atomic<float> mix4{0.2f};

    // Master
    std::atomic<float> master{0.8f};

    // Toggles (osc enable)
    std::atomic<bool> osc1_on{true};
    std::atomic<bool> osc2_on{false};
    std::atomic<bool> osc3_on{true};
    std::atomic<bool> osc4_on{false};
};

// Global shared params — accessed by both UI and audio threads
SynthParams& synth_params();

// Start/stop the Oboe audio stream
bool synth_start();
void synth_stop();
bool synth_is_playing();

// Peak level for visual metering (written by audio thread, read by UI)
float synth_peak_level();

} // namespace pulp::demo
