#include "test_matrix.hpp"
#include "pulp_synth.hpp"

using namespace pulp;
using namespace pulp::test;

static format::ProcessorFactory factory = examples::create_pulp_synth;

TEST_CASE("PulpSynth matrix: silence finite", "[matrix][synth]") {
    verify_silence_produces_finite(factory);
}

TEST_CASE("PulpSynth matrix: state round-trip", "[matrix][synth]") {
    verify_state_round_trip(factory);
}

TEST_CASE("PulpSynth matrix: zero-length buffer", "[matrix][synth]") {
    verify_zero_length_buffer(factory);
}
