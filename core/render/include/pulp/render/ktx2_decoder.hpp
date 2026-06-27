#pragma once

#include <cstdint>
#include <vector>
#include <string>

namespace pulp::render {

/// Parsed texture data from a KTX2 container.
struct DecodedTexture {
    std::vector<uint8_t> pixels;   ///< Pixel data when a transcoding backend provides it.
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mip_levels = 1;
    std::string format;            ///< Preferred GPU format hint.
    bool compressed = false;       ///< Whether pixels are GPU-compressed (BC7/ASTC/ETC2)
    bool success = false;
};

/// Detect the optimal GPU texture compression format for the current platform.
/// Returns "astc" on Apple targets, "bc7" on Windows/Linux-family targets,
/// and "rgba8" for the fallback path.
std::string optimal_gpu_format();

/// Validate and parse KTX2 header metadata.
/// The current built-in path does not transcode Basis Universal payloads, so
/// `pixels` can be empty even when `success` is true.
DecodedTexture decode_ktx2(const uint8_t* data, size_t size);

/// Check if a KTX2 transcoding backend was compiled in.
bool ktx2_available();

} // namespace pulp::render
