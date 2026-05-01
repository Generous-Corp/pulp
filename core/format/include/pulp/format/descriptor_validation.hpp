#pragma once

// PluginDescriptor validation (workstream 01 slice 1.9).
//
// Runs at plugin registration / test time — NEVER on the audio thread.
// Catches the common descriptor mistakes that otherwise surface as
// mysterious host-side "plugin did not appear" / "factory returned
// null" failures:
//   - empty name / manufacturer / bundle_id / version
//   - bundle_id that doesn't look like reverse-DNS
//   - no output bus declared, or invalid main bus channel counts
//   - instrument category with 0-channel output
//   - accepts_midi / supports_mpe / supports_ump on effect category
//     without any MIDI capability setup (legal but usually a mistake)
//
// Errors are returned as a vector — callers (test harnesses, registry
// loaders, `pulp validate`) format them. Pure function, no allocation
// beyond the result vector.

#include <pulp/format/processor.hpp>

#include <string>
#include <vector>

namespace pulp::format {

enum class DescriptorIssueSeverity {
    Error,     ///< Invalid state — plugin will not load correctly
    Warning,   ///< Suspicious — plugin may behave unexpectedly
};

struct DescriptorIssue {
    DescriptorIssueSeverity severity;
    std::string field;       ///< "name", "bundle_id", "input_buses", ...
    std::string message;
};

/// Run the validator. Returns every issue found — never stops at the
/// first problem so a developer sees the full report.
std::vector<DescriptorIssue> validate_descriptor(const PluginDescriptor& d);

/// True iff `issues` contains no `Error` severity entries. Warnings
/// are informational only. Callers gating on "may the plugin load"
/// use this helper.
bool descriptor_is_valid(const std::vector<DescriptorIssue>& issues);

}  // namespace pulp::format
