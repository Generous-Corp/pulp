#pragma once

// Blocking mapped readback of a padded texture copy. Shared by Renderer3D and
// GpuTarget so the map/wait/de-pad sequence exists once. Not real-time safe:
// it spins the instance's event loop until the map resolves or times out.

#include "gpu_readback_layout.hpp"

#include "webgpu/webgpu_cpp.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pulp::render::detail {

/// Map `buffer` (which holds `layout.buffer_size` bytes of padded rows) for
/// reading, de-pad it and unmap it. Returns nullopt with `error` set when the
/// map fails, is cancelled, times out or yields a null range.
std::optional<std::vector<std::uint8_t>> read_padded_buffer(const wgpu::Instance& instance,
                                                            const wgpu::Buffer& buffer,
                                                            const PaddedRowLayout& layout,
                                                            std::chrono::milliseconds timeout,
                                                            std::string& error);

} // namespace pulp::render::detail
