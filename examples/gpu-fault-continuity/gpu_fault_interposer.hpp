#pragma once

// Test-only runtime provider faults (see gpu_fault_interposer.cpp).

#include "gpu_fault_continuity.hpp"

#include <cstdint>
#include <string>

namespace pulp::examples::gpu_fault {

// Host thread, after the shared-I/O provider prepared. Idempotent.
bool install_provider_fault_interposer(std::string& detail);
bool provider_fault_interposer_installed() noexcept;

// Any non-audio thread. Arms one provider fault; it is applied, once, at the
// provider's next matching Dawn call on a non-main thread. `delay_ms` is the
// stall for delay_completion and hang.
bool arm_provider_fault(FaultKind kind, std::uint32_t delay_ms, std::string& detail);

// Host thread. Drops a fault that was armed but never applied.
void disarm_provider_fault() noexcept;

// How many provider faults were actually applied, and at which call site.
std::uint64_t provider_faults_applied() noexcept;
const char* provider_fault_point() noexcept;

} // namespace pulp::examples::gpu_fault
