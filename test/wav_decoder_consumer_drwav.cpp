// A downstream consumer may follow dr_wav's documented single-header setup.
// This TU intentionally supplies its own implementation while the test binary
// also calls Pulp's WAV decoder, proving Pulp's private copy cannot collide.
#define DR_WAV_IMPLEMENTATION
#include <dr_wav.h>

const char* consumer_drwav_version() {
    return drwav_version_string();
}
