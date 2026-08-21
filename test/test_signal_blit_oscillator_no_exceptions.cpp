#include <pulp/signal/blit_oscillator.hpp>

int main() {
    pulp::signal::BlitOscillator oscillator;
    pulp::signal::BlitOscillator64 oscillator64;
    if (!oscillator.prepare(48000.0f) || !oscillator.set_frequency(440.0f))
        return 1;
    if (!oscillator64.prepare(96000.0) || !oscillator64.set_frequency(1000.0))
        return 2;
    return oscillator.next() == 0.0f || oscillator64.next() == 0.0 ? 3 : 0;
}
