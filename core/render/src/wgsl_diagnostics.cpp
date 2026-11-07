#include <pulp/render/wgsl_diagnostics.hpp>

namespace pulp::render {

std::string_view wgsl_severity_name(WgslDiagnostic::Severity severity) {
    switch (severity) {
    case WgslDiagnostic::Severity::error:
        return "error";
    case WgslDiagnostic::Severity::warning:
        return "warning";
    case WgslDiagnostic::Severity::info:
        return "info";
    }
    return "error";
}

std::string format_wgsl_diagnostic(std::string_view source_name, const WgslDiagnostic& diagnostic) {
    std::string out(source_name.empty() ? std::string_view("<wgsl>") : source_name);
    if (diagnostic.line > 0) {
        out += ':';
        out += std::to_string(diagnostic.line);
        if (diagnostic.column > 0) {
            out += ':';
            out += std::to_string(diagnostic.column);
        }
    }
    out += ": ";
    out += wgsl_severity_name(diagnostic.severity);
    out += ": ";
    out += diagnostic.message;
    return out;
}

std::string format_wgsl_diagnostics(std::string_view source_name,
                                    const std::vector<WgslDiagnostic>& diagnostics) {
    std::string out;
    for (const auto& diagnostic : diagnostics) {
        if (!out.empty())
            out += '\n';
        out += format_wgsl_diagnostic(source_name, diagnostic);
    }
    return out;
}

bool has_wgsl_errors(const std::vector<WgslDiagnostic>& diagnostics) {
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.severity == WgslDiagnostic::Severity::error)
            return true;
    }
    return false;
}

} // namespace pulp::render
