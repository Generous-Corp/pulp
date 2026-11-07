#pragma once

/// @file wgsl_diagnostics.hpp
/// WGSL compiler messages as data.
///
/// Dawn reports a shader's compile messages through `GetCompilationInfo`.
/// `GpuDevice::create_shader_module()` collects them into `WgslDiagnostic`
/// records, and the build-time validator behind `pulp_add_wgsl_shaders()`
/// prints them in the `file:line:column: severity: message` form editors and
/// CI logs already link.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::render {

struct WgslDiagnostic {
    enum class Severity : std::uint8_t { error, warning, info };

    Severity severity = Severity::error;
    /// 1-based line, or 0 when the message has no source position (a
    /// pipeline-level validation error, for example).
    std::uint32_t line = 0;
    /// 1-based column, or 0 with `line`.
    std::uint32_t column = 0;
    std::string message;
};

std::string_view wgsl_severity_name(WgslDiagnostic::Severity severity);

/// `name:line:column: severity: message`, or `name: severity: message`
/// without a position.
std::string format_wgsl_diagnostic(std::string_view source_name, const WgslDiagnostic& diagnostic);

/// One formatted diagnostic per line.
std::string format_wgsl_diagnostics(std::string_view source_name,
                                    const std::vector<WgslDiagnostic>& diagnostics);

bool has_wgsl_errors(const std::vector<WgslDiagnostic>& diagnostics);

} // namespace pulp::render
