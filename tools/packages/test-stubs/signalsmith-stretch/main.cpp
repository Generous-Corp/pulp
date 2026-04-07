#include "signalsmith-stretch.h"
#include <cstdio>

int main() {
    signalsmith::stretch::SignalsmithStretch<float> stretch;
    stretch.presetDefault(2, 48000.0f);
    std::printf("signalsmith-stretch: OK (channels=2, rate=48000)\n");
    return 0;
}
