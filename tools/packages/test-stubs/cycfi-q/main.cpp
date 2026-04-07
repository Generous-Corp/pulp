#include <q/pitch/pitch_detector.hpp>
#include <q/support/literals.hpp>
#include <cstdio>

int main() {
    using namespace cycfi::q::literals;
    cycfi::q::pitch_detector pd(50_Hz, 4000_Hz, 48000, -30_dB);
    std::printf("cycfi-q: OK (pitch detector created)\n");
    return 0;
}
