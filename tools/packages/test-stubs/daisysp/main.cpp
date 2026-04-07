#include "Synthesis/oscillator.h"
#include <cstdio>

int main() {
    daisysp::Oscillator osc;
    osc.Init(48000.0f);
    osc.SetFreq(440.0f);
    osc.SetWaveform(daisysp::Oscillator::WAVE_SIN);
    float sample = osc.Process();
    std::printf("daisysp: OK (sine osc output=%.4f)\n", sample);
    return 0;
}
