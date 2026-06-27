#include <pulp/render/ktx2_decoder.hpp>

namespace pulp::render {

std::string optimal_gpu_format() {
#if defined(__APPLE__)
    // Apple Silicon supports ASTC natively
    return "astc";
#elif defined(_WIN32) || defined(__linux__)
    // Desktop GPUs support BC7 (DXT)
    return "bc7";
#else
    // Fallback: uncompressed
    return "rgba8";
#endif
}

// KTX2 metadata parser. Full Basis Universal payload transcoding would use a
// real KTX2 backend such as libktx's ktxTexture2_TranscodeBasis().

DecodedTexture decode_ktx2(const uint8_t* data, size_t size) {
    DecodedTexture result;
    if (!data || size < 12) return result;

    // Verify KTX2 magic: 0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A
    static const uint8_t ktx2_magic[] = {0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};
    for (int i = 0; i < 12; ++i) {
        if (data[i] != ktx2_magic[i]) return result;  // Not a KTX2 file
    }

    // Parse KTX2 header (minimal — width, height from fixed offsets)
    // Full implementation would use libktx for proper Basis Universal transcoding.
    if (size < 80) return result;

    auto read_u32 = [data](size_t offset) -> uint32_t {
        return static_cast<uint32_t>(data[offset]) |
               (static_cast<uint32_t>(data[offset + 1]) << 8) |
               (static_cast<uint32_t>(data[offset + 2]) << 16) |
               (static_cast<uint32_t>(data[offset + 3]) << 24);
    };

    // KTX2 header layout: after 12-byte magic, vkFormat(4), typeSize(4), width(4), height(4)
    result.width = read_u32(20);   // pixelWidth
    result.height = read_u32(24);  // pixelHeight
    result.mip_levels = std::max(1u, read_u32(32)); // levelCount
    result.format = optimal_gpu_format();

    // Without a KTX2 transcoder, return header info but no decoded pixels.
    result.success = (result.width > 0 && result.height > 0);

    return result;
}

bool ktx2_available() {
#ifdef PULP_HAS_KTX2
    return true;
#else
    return false;
#endif
}

} // namespace pulp::render
