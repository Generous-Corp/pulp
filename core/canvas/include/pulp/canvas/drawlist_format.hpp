#pragma once

// Drawlist formatting — a `RecordingCanvas` drawlist as readable text.
//
// A recorded drawlist is already a value (`RecordingCanvas::commands()`), but
// reading one means walking a vector and switching on an enum. Every test that
// asserts on drawing does that by hand today, indexing into `commands()` and
// comparing `.type` — which says what the Nth command was, and nothing about
// what the drawing IS.
//
//   CHECK(format_commands(rc.commands()).find("fill_rect 10.000 20.000") != npos);
//
// ── Why this exists next to the pixel comparisons ────────────────────────
// Pulp's visual checks compare pixels, and `docs/status/tools.yaml` records
// what each of them cannot see: the histogram scorer is position-blind (a
// design with every element in the wrong place scores the same as a correct
// one), and the block-mean scorer is material-blind (a flattened gradient
// matches its own mean exactly). A drawlist is neither. The coordinates, the
// colours, the blur radii and the blend modes are all in the ops, so a text
// golden over this catches exactly the two defect classes a pixel diff waves
// through — and it catches them without a GPU, a font, or a reference image.
//
// It is NOT a replacement for pixel comparison: a drawlist says what was
// ASKED for, never what was drawn. A backend that silently ignores a blur
// still records the blur. Use both; they fail on different things.
//
// ── Round-tripping is deliberately absent ────────────────────────────────
// There is no parser here and there should not be one. Pulp already has two
// round-trippable draw formats — SKP capture (`render/skp_capture.hpp`, which
// the Skia debugger reads) and the canvas2d API the ops were authored in. A
// third format nobody authors in would be weight without a reader.
//
// ── Stability ────────────────────────────────────────────────────────────
// Floats print at fixed precision so a golden does not flap on the last bit
// of a float, and ALL SIX are always printed. Printing only the ones a
// command "uses" would need a per-command arity table — 83 chances to encode
// a wrong one, and a wrong one hides a parameter rather than failing. The
// command name tells a reader which slots carry meaning.

#include <string>
#include <vector>

#include <pulp/canvas/recording_canvas.hpp>

namespace pulp::canvas {

/// The command's name, exactly as its enumerator is spelled.
///
/// The switch behind this has no `default`, so adding a `DrawCommand::Type`
/// without naming it here fails to compile rather than formatting as
/// something unhelpful at run time.
const char* draw_command_name(DrawCommand::Type type);

/// One command as a single line, with no trailing newline.
///
/// Shape: `name f0 f1 f2 f3 f4 f5` followed by `#RRGGBBAA` when the colour is
/// not the default, `"text"` when the text field is set, and `+N floats` when
/// the variable-length payload is non-empty.
std::string format_command(const DrawCommand& command);

/// A whole drawlist, one command per line, newline-separated with a trailing
/// newline. An empty drawlist formats as an empty string.
std::string format_commands(const std::vector<DrawCommand>& commands);

} // namespace pulp::canvas
