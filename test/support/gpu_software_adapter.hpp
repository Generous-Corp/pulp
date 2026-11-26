#pragma once

// Recognises a compute adapter that executes on the CPU. Tests whose
// tolerances or deadlines are sized for a hardware GPU skip there and name
// the adapter, so a hosted runner reports a skip rather than a failure that
// measures the rasterizer.

#include <pulp/render/gpu_compute.hpp>

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace pulp::test {

namespace detail {

inline std::string lowered(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

inline bool contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

} // namespace detail

/// True when `caps` describes a software rasterizer: Dawn's CPU adapter class,
/// Windows WARP (which D3D12 reports as an integrated GPU, so the class alone
/// does not identify it), llvmpipe, or SwiftShader.
inline bool is_software_adapter(const render::GpuCompute::CapabilityReport& caps) {
    if (caps.adapter_type == "cpu")
        return true;
    const auto identity = detail::lowered(caps.name + " " + caps.vendor + " " + caps.architecture +
                                          " " + caps.description);
    if (detail::contains(identity, "basic render driver") ||
        detail::contains(identity, "llvmpipe") || detail::contains(identity, "swiftshader"))
        return true;
    return detail::lowered(caps.vendor) == "microsoft" &&
           detail::lowered(caps.architecture) == "warp";
}

/// Empty on a hardware adapter; otherwise a sentence naming the software
/// adapter, ready to pass to `SKIP`, followed by `why_it_matters`.
inline std::string software_adapter_skip_reason(const render::GpuCompute::CapabilityReport& caps,
                                                const std::string& why_it_matters) {
    if (!is_software_adapter(caps))
        return {};
    return "software adapter " + (caps.name.empty() ? std::string("(unnamed)") : caps.name) + " (" +
           caps.backend + "): " + why_it_matters;
}

} // namespace pulp::test
