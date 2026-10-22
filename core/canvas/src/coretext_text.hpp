#pragma once

// CoreText face resolution and measurement shared by the CoreGraphics canvas
// (paint) and TextShaper's non-Skia path (measure) on Apple platforms.
//
// Both sides must resolve the same face for a family, or a label measured
// with one face is painted with another and sits at the wrong baseline. The
// registry here holds Pulp's bundled fonts and every face passed to
// register_font(), keyed by family, so "Inter" resolves to the embedded Inter
// rather than to CoreText's Helvetica fallback for a name it does not know.
// Faces are kept per process in Pulp's own table rather than registered with
// CTFontManager, so a plug-in never adds fonts to its host's font list.

#if defined(__APPLE__)

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <pulp/canvas/canvas.hpp>

namespace pulp::canvas::detail {

/// Register a font file's bytes (copied). Returns the family it was filed
/// under: `family_override` when non-empty, otherwise the face's own family
/// name. Returns an empty string when CoreText cannot read the bytes.
std::string coretext_register_font(const std::uint8_t* data, std::size_t size,
                                   const std::string& family_override);

/// True when `family` names a face in the registry (bundled or registered).
bool coretext_has_registered_family(const std::string& family);

/// A +1 retained CTFontRef (returned as `const void*` so C++ callers need no
/// CoreText headers; release with CFRelease). `family_stack` is a CSS family
/// list; the first entry the registry or the system knows wins. Weight is
/// the CSS weight (100..900); slant is 0 upright, 1 italic, 2 oblique.
/// `features` are OpenType feature settings (tag, value) applied to the face.
/// Never null: an unknown stack resolves to the system UI font.
const void* coretext_create_font(std::string_view family_stack, float size, int weight, int slant,
                                 const std::vector<Canvas::FontFeature>& features = {});

struct CoreTextFaceMetrics {
    float ascent = 0.0f;  ///< Recommended ascent (positive, above baseline).
    float descent = 0.0f; ///< Recommended descent (positive, below baseline).
    float leading = 0.0f;
    float top = 0.0f;    ///< Worst-case glyph extent above the baseline.
    float bottom = 0.0f; ///< Worst-case glyph extent below the baseline.
};

/// Face metrics of the font coretext_create_font() resolves, using the same
/// definitions as Skia's CoreText backend (fTop/fBottom from the font
/// bounding box, fAscent/fDescent/fLeading from the font).
CoreTextFaceMetrics coretext_face_metrics(std::string_view family_stack, float size, int weight,
                                          int slant);

/// Typographic advance of `text` laid out as one CTLine in that font.
float coretext_advance(std::string_view text, std::string_view family_stack, float size, int weight,
                       int slant, const std::vector<Canvas::FontFeature>& features = {});

} // namespace pulp::canvas::detail

#endif // __APPLE__
