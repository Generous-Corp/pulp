#include <samplerate.h>
#include <cstdio>

int main() {
    const char* name = src_get_name(SRC_SINC_BEST_QUALITY);
    std::printf("libsamplerate: OK (best quality = %s)\n", name);
    return 0;
}
