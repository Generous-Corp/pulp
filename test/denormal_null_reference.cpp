// Reference translation unit for the MF-3 denormal null test.
//
// Compiled with snap_to_zero DISABLED, so it renders exactly what the filters
// produced before this change. The test TU (test_denormal_null.cpp), compiled
// with the shipping default (snap enabled), compares against it bit-for-bit.
#define PULP_DSP_ENABLE_SNAP_TO_ZERO 0

#include "denormal_null_reference.hpp"

denormal_null::AllOutputs denormal_null_reference() {
    return denormal_null::render_all();
}

denormal_null::TailReport denormal_null_reference_tail() {
    return denormal_null::render_tails();
}
