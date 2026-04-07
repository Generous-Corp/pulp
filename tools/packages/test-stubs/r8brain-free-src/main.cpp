#include "CDSPResampler.h"
#include <cstdio>

int main() {
    r8b::CDSPResampler24 resampler(44100.0, 48000.0, 1024);
    std::printf("r8brain-free-src: OK (44100->48000 resampler created)\n");
    return 0;
}
