#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"
#include <cstdio>

int main() {
    drwav_data_format fmt{};
    fmt.container = drwav_container_riff;
    fmt.format = DR_WAVE_FORMAT_PCM;
    fmt.channels = 1;
    fmt.sampleRate = 48000;
    fmt.bitsPerSample = 16;
    std::printf("dr-libs: OK (drwav format configured)\n");
    return 0;
}
