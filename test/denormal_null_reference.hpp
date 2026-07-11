#pragma once
//
// Cross-TU handle for the MF-3 denormal null test. Implemented in
// test/denormal_null_reference.cpp, which is compiled with
// PULP_DSP_ENABLE_SNAP_TO_ZERO=0 so it reproduces the exact pre-change
// (snap-disabled) filter output.

#include "denormal_null_harness.hpp"

// Renders the harness signal through every filter with snap_to_zero compiled
// out (the identity), i.e. the reference "before" behavior.
denormal_null::AllOutputs denormal_null_reference();

// Silence-tail subnormal report with snap_to_zero compiled out — expected to
// flag subnormals, confirming the tail configs actually reach that range.
denormal_null::TailReport denormal_null_reference_tail();
