#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pulp::inspect {

struct InspectorCapture {
    std::vector<std::uint8_t> png;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::string error;
};

/// Host-owned image source. Protocol/session code never depends on platform
/// window types; the standalone adapter decides how its selected window is read.
class InspectorCaptureSource {
  public:
    virtual ~InspectorCaptureSource() = default;
    virtual InspectorCapture capture_png() = 0;
};

} // namespace pulp::inspect
