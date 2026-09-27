#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace pulp::host {

// These reads are control-thread operations, never audio callback operations.
enum class CustomNodeDiagnosticAvailability : std::uint8_t {
    Available,
    Unsupported,
    MissingNode,
    StaleHandle,
    NotPrepared,
    Busy,
    SchemaMismatch
};
enum class CustomNodeDiagnosticConsistency : std::uint8_t {
    LiveApproximate,
    // Caller promises its sole audio process caller is stopped. Worker telemetry
    // may still advance; this does not certify drain or per-sequence uniqueness.
    AudioCallerStopped
};
struct CustomNodeDiagnosticHandle {
    std::uint64_t graph_identity = 0;
    std::uint64_t graph_generation = 0;
    std::uint32_t node_id = 0;
};
struct CustomNodeDiagnosticResult {
    CustomNodeDiagnosticAvailability availability = CustomNodeDiagnosticAvailability::Unsupported;
    CustomNodeDiagnosticConsistency consistency = CustomNodeDiagnosticConsistency::LiveApproximate;
    CustomNodeDiagnosticHandle handle{};
    std::uint64_t schema = 0;
    std::uint64_t preparation_generation = 0;
    int type_version = 0;
    std::array<char, 128> type_id{};          // Registered identity, including a live alias.
    std::array<char, 128> producer_type_id{}; // Original diagnostic producer.
    int producer_type_version = 0;
};
// Sibling registration preserves CustomNodeType's positional aggregate layout.
// Query callbacks must finish without waiting, allocation or lifecycle mutation.
// Write an owned trivially-copyable report with memcpy, never a cast of output.
struct CustomNodeDiagnosticsDescriptor {
    static constexpr std::size_t kMaximumReportBytes = 4096;
    std::string type_id;
    int type_version = 1;
    std::uint64_t schema = 0;
    std::size_t report_bytes = 0;
    CustomNodeDiagnosticAvailability (*query)(const void* instance, std::span<std::byte> output,
                                              std::uint64_t& preparation_generation) noexcept =
        nullptr;
    // Wrappers retain the original producer identity while registering an alias.
    std::string producer_type_id;
    int producer_type_version = 0;
};

} // namespace pulp::host
