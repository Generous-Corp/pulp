#pragma once

#include <cstdint>
#include <vector>
#include <string>

namespace pulp::render {

/// Decoded texture data from KTX2 container.
struct DecodedTexture {
    std::vector<uint8_t> pixels;   ///< RGBA8 pixel data (after transcoding)
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mip_levels = 1;
    std::string format;            ///< GPU format string (e.g., "rgba8unorm", "bc7-rgba-unorm")
    bool compressed = false;       ///< Whether pixels are GPU-compressed (BC7/ASTC/ETC2)
    bool success = false;
};

/// Detect the optimal GPU texture compression format for the current platform.
/// Returns "bc7" on Windows/Linux desktop, "astc" on Apple Silicon, "etc2" on mobile.
std::string optimal_gpu_format();

/// Decode a KTX2 texture to the optimal GPU format.
/// If the KTX2 contains Basis Universal supercompressed data, transcodes
/// to the platform's preferred GPU format.
/// If no native KTX2 library is available, returns uncompressed RGBA8.
DecodedTexture decode_ktx2(const uint8_t* data, size_t size);

/// Check if KTX2 decoding is available (native library linked).
bool ktx2_available();

} // namespace pulp::render
