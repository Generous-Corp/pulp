#pragma once

namespace pulp::canvas {

/// Opaque handle to a subtree's recorded draw commands, produced by
/// `Canvas::record_scene()` and replayed by `Canvas::draw_scene()`.
///
/// It is deliberately empty at this layer: each backend subclasses it with
/// its own recording payload (the Skia backend wraps an `SkPicture`) and
/// `draw_scene()` downcasts to recognise its own type, returning false for a
/// foreign recording. Callers hold it through a `std::shared_ptr` so a
/// recording survives across frames until the owner invalidates it — that
/// cross-frame lifetime is the whole point of the scene-cache seam (FU-3).
///
/// Lives in its own tiny header (included by canvas.hpp for back-compat) so
/// the type can be named by cache holders without dragging in the full Canvas
/// interface.
class SceneRecording {
public:
    virtual ~SceneRecording() = default;
};

}  // namespace pulp::canvas
