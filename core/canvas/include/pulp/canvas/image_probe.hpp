#pragma once

#include <string>

namespace pulp::canvas {

/// Whether the renderer can decode an encoded raster image file.
enum class ImageFileProbe {
    decodes,     ///< Every pixel decoded.
    undecodable, ///< Unreadable, not an image the renderer knows, or truncated.
    unavailable, ///< No raster decoder in this build (no Skia); nothing was checked.
};

/// Decode @p path fully with the codec the Skia canvas draws file images
/// through. A decode that reports success is not enough on its own: Skia fills
/// the rows of a truncated PNG with transparent pixels and still succeeds, so a
/// PNG must also have every chunk whole, every CRC correct, and an IEND.
/// Raster formats only; an SVG is not an encoded raster and reports
/// undecodable. File I/O — never call it on the audio or render thread.
ImageFileProbe probe_image_file(const std::string& path);

} // namespace pulp::canvas
