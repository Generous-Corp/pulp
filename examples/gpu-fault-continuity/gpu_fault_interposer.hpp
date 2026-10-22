#pragma once

// Test-only runtime provider faults (see gpu_fault_interposer.cpp).

#include "gpu_fault_continuity.hpp"

#include <cstdint>
#include <string>

namespace pulp::examples::gpu_fault {

// Host thread, after the shared-I/O provider prepared. Idempotent.
bool install_provider_fault_interposer(std::string& detail);
bool provider_fault_interposer_installed() noexcept;

// Host thread, around this instance's node prepare: the device whose queue the
// provider fetches in between (and that device's instance) become the only
// targets a fault may hit. Returns false when no target was captured.
void begin_provider_fault_target_capture() noexcept;
bool end_provider_fault_target_capture(std::string& detail);
bool provider_fault_target_captured() noexcept;

// Any non-audio thread. Arms one provider fault; it is applied, once, at the
// provider's next matching Dawn call on a non-main thread. `delay_ms` is the
// stall for delay_completion and hang.
bool arm_provider_fault(FaultKind kind, std::uint32_t delay_ms, std::string& detail);

// Host thread, before the engine is released. Drops a fault that was armed
// but never applied, and forgets every queue -> device mapping.
void disarm_provider_fault() noexcept;

// How many provider faults were actually applied, and at which call site.
std::uint64_t provider_faults_applied() noexcept;
const char* provider_fault_point() noexcept;

} // namespace pulp::examples::gpu_fault
