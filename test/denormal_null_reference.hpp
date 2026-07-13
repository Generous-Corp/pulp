#pragma once
//
// Cross-PROCESS handle for the MF-3 denormal null test.
//
// The reference is the same harness compiled with PULP_DSP_ENABLE_SNAP_TO_ZERO=0.
// It CANNOT be linked into the test executable: `snap_to_zero()` is an inline
// function template in a header, and every filter that calls it (Svf::process,
// DcBlocker<float>::process, Reverb::process, ...) is likewise header-defined
// with external linkage. Compiling one TU with the macro off gives those
// symbols two different bodies under one mangled name — an ODR violation. The
// optimizer hid it: at -O3 each TU inlines its own copy and the A/B "works",
// but at -O0 nothing is inlined, the linker keeps a single weak definition for
// both TUs, and the "reference" silently runs the SNAPPING code. That is how
// the teeth check (the reference must reach a subnormal) failed in Debug — and
// had the linker picked the other definition, the guard assertions would have
// passed while testing nothing at all.
//
// So the reference runs as its own binary (test/denormal_null_refgen.cpp,
// snap-disabled end to end) and hands its results back over a file. Every
// executable is now internally consistent about the macro.

#include "denormal_null_harness.hpp"

#include <cstdint>

namespace denormal_null {

// Blob layout written by the refgen binary and decoded by the test.
inline constexpr std::uint32_t kRefMagic = 0x314E4E44u;  // "DNN1"

struct Reference {
    AllOutputs outputs;
    TailReport tail;
    // FP mode of the REFERENCE process — the one that had to be able to
    // produce a subnormal. Hardware FTZ makes that physically impossible.
    bool denormals_flushed = false;
};

// Writes `outputs`/`tail`/`flushed` to `path`. Used by the refgen binary.
// Returns false on any I/O failure.
bool write_reference(const char* path, const AllOutputs& outputs,
                     const TailReport& tail, bool flushed);

// Reads a blob written by write_reference(). Returns false if the file is
// missing, truncated, or does not carry kRefMagic.
bool read_reference(const char* path, Reference& out);

}  // namespace denormal_null

// Runs the snap-disabled reference binary in a child process and decodes its
// result. Fails the calling test (via Catch2) if the child cannot be run.
denormal_null::Reference denormal_null_run_reference();
