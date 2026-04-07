#include <pffft/pffft.h>
#include <cstdio>

int main() {
    PFFFT_Setup* setup = pffft_new_setup(1024, PFFFT_REAL);
    if (setup) {
        std::printf("pffft: OK (1024-point real FFT created)\n");
        pffft_destroy_setup(setup);
    } else {
        std::printf("pffft: FAIL (setup returned null)\n");
        return 1;
    }
    return 0;
}
