#pragma once

// Checks for a font container held in memory, applied before any font parser
// sees the bytes, and registration of such a container without a path lookup.
// Panel fonts sealed into a plug-in, design-import fonts read from a manifest,
// and captured page fonts share them.

#include <pulp/canvas/bundled_fonts.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

namespace pulp::view {

/// The largest font container a design or panel ships that Pulp will parse.
/// No real face is near it, and the bytes are copied whole into the registry.
inline constexpr std::size_t kMaxShippedFontBytes = 32u * 1024u * 1024u;

/// Whether `data` starts like a TrueType (0x00010000 or 'true'), OpenType
/// ('OTTO') or WOFF2 ('wOF2') container: the formats the platform font
/// managers and the WOFF2 decoder accept.
inline bool has_font_container_signature(const std::uint8_t* data, std::size_t size) noexcept {
    if (data == nullptr || size < 4)
        return false;
    const auto tag = [&](char a, char b, char c, char d) {
        return data[0] == static_cast<std::uint8_t>(a) && data[1] == static_cast<std::uint8_t>(b) &&
               data[2] == static_cast<std::uint8_t>(c) && data[3] == static_cast<std::uint8_t>(d);
    };
    return tag('\0', '\1', '\0', '\0') || tag('t', 'r', 'u', 'e') || tag('O', 'T', 'T', 'O') ||
           tag('w', 'O', 'F', '2');
}

/// The decompressed size a WOFF2 container declares (its header's
/// totalSfntSize), which is the value the WOFF2 decoder's
/// ComputeWOFF2FinalSize reports; 0 when the header is truncated.
inline std::size_t woff2_declared_sfnt_size(const std::uint8_t* data, std::size_t size) noexcept {
    if (data == nullptr || size < 20)
        return 0;
    return (static_cast<std::size_t>(data[16]) << 24) | (static_cast<std::size_t>(data[17]) << 16) |
           (static_cast<std::size_t>(data[18]) << 8) | static_cast<std::size_t>(data[19]);
}

/// The largest decompressed face a WOFF2 container may declare, the same
/// bound `canvas::register_font_woff2` applies.
inline constexpr std::size_t kMaxShippedWoff2SfntBytes = 30u * 1024u * 1024u;

/// Register an in-memory font container under `family`. Bytes larger than
/// `kMaxShippedFontBytes` or without a font container signature are refused
/// before any parser sees them. TrueType and OpenType bytes are structurally
/// validated first. WOFF2 goes through Pulp's WOFF2 decoder, which bounds and
/// validates the decompressed face; when the decoder rejects the bytes they
/// are refused, never handed to the platform font manager instead. Only a
/// build with no decoder linked passes WOFF2 to the platform font manager,
/// and then only within the same declared-size bound. Never consults the
/// filesystem. Registration is process-global.
inline bool register_font_container(const std::uint8_t* data, std::size_t size,
                                    const std::string& family) {
    if (size > kMaxShippedFontBytes || !has_font_container_signature(data, size))
        return false;
    if (data[0] == 'w') {
        if (pulp::canvas::woff2_decoder_available())
            return pulp::canvas::register_font_woff2(data, size, family);
        const auto declared = woff2_declared_sfnt_size(data, size);
        if (declared == 0 || declared > kMaxShippedWoff2SfntBytes)
            return false;
        return pulp::canvas::register_font(data, size, family);
    }
    if (!pulp::canvas::validate_font_bytes(data, size))
        return false;
    return pulp::canvas::register_font(data, size, family);
}

} // namespace pulp::view
