#include "filters.h"
#include <cstdio>

int main() {
    signalsmith::filters::BiquadStatic<float> bq;
    bq.lowpass(1000.0f / 48000.0f, 0.707f);
    float sample = bq(1.0f);
    std::printf("signalsmith-dsp: OK (lowpass output=%.4f)\n", sample);
    return 0;
}
