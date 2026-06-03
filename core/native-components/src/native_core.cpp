// Production-side definitions for the native-component core ABI.
//
// The only runtime symbol the contract needs a default for is the RT-safety
// trap. In production it is a no-op: the contract is enforced by the adapter
// and by review, not by trapping in shipped audio code. Test harnesses provide
// a STRONG override of this symbol that actually traps when an allocation is
// attempted inside a no-alloc scope (see test/native_components/).
//
// The default is weak so a test executable that links this module AND a strong
// override resolves to the override without a duplicate-symbol error.
#include <pulp/native_components/native_core.h>

#include <cstddef>
#include <cstdint>

extern "C" __attribute__((weak)) void
pulp_rt_trap_if_no_alloc_scope(std::int32_t /*kind*/, std::size_t /*bytes*/) {
    // Intentionally empty: no allocation, no logging, no syscalls.
}
