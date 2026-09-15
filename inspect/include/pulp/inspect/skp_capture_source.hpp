#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pulp::inspect {

struct InspectorSkpCapture {
    std::vector<std::uint8_t> skp;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t op_count = 0;
    std::string error;
    std::string error_code;  ///< Optional protocol code; defaults to capture_failed.
};

/// Host-owned serialized-frame source. Protocol/session code never depends on
/// the render backend; the host decides how its live frame is re-recorded into
/// an `.skp` blob and whether a GPU context is needed to read back textures.
class SkpCaptureSource {
  public:
    virtual ~SkpCaptureSource() = default;
    virtual InspectorSkpCapture capture_skp() = 0;
};

} // namespace pulp::inspect
