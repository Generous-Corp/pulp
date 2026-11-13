#pragma once

// Internal seam between the platform hooks and the shared recording code.

#include <pulp/testing/texture_upload_recorder.hpp>

#include <cstdint>
#include <string>

namespace pulp::testing::detail {

struct UploadDescription {
    const char* api = "";
    const char* entry = "";
    std::uint64_t texture_id = 0;
    std::uint32_t width = 0, height = 0, depth = 1;
    std::uint32_t x = 0, y = 0, z = 0;
    std::uint32_t mip = 0, slice = 0;
    std::string pixel_format;
    std::uint32_t pixel_format_value = 0;
    std::uint32_t bytes_per_pixel = 0;  ///< 0 when unknown: rows are kept as given
    const std::uint8_t* data = nullptr; ///< null when the source is not CPU-readable
    std::uint64_t source_bytes_per_row = 0;
    std::uint64_t source_bytes_per_image = 0;
    std::string caller_image;
};

/// Whether install() is in effect.
bool recording();

/// Records one upload. Safe from any thread.
void record_upload(const UploadDescription& upload);

/// Installs the platform's hooks once per process. False with a reason when
/// the platform has no supported API.
bool install_platform_hooks(std::string* error);

/// Copies the uploaded pixels without row or image padding. Returns false
/// (leaving `out` holding the bytes as given) when the pixel size is unknown.
bool pack_rows(const UploadDescription& upload, std::string& out);

} // namespace pulp::testing::detail
