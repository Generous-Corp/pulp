#include <pulp/signal/sub_oscillator.hpp>

int main() {
    pulp::signal::SubOscillator oscillator;
    pulp::signal::SubOscillator64 oscillator64;
    if (!oscillator.set_octave(1) || !oscillator64.set_octave(2))
        return 1;
    oscillator.set_waveform(pulp::signal::SubOscillatorWaveform::square);
    oscillator64.set_waveform(pulp::signal::SubOscillatorWaveform::sine);
    oscillator.on_parent_cycle();
    return oscillator.next(0.25f) == 0.0f || oscillator64.next(0.25) == 0.0 ? 2 : 0;
}
