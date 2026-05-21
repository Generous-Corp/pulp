#pragma once

// skp_capture.hpp — Skia `.skp` (SkPicture) frame capture.
//
// Phase 6.4 of the inspector GPU-perf roadmap
// (planning/2026-05-19-inspector-phase6-gpu-perf-spike.md § Phase 6.4).
//
// The capture writes a rendered frame as a Skia `.skp` artifact that
// Skia's own `skiadebugger` can replay op-by-op. This is the bridge
// from "a frame looked weird in Pulp" to the best-in-class external
// frame-debugging tool — for free, because Skia ships the format.
//
// Design (from the Phase 6.4 spike, which de-risked this in PR #2506):
//
//   * `.skp` is the legacy `SkPicture` serialization format. It is
//     fully backend-independent — an `SkPictureRecorder` records draw
//     ops into a backend-agnostic `SkRecord`, and nothing in the
//     Graphite headers references `SkPicture`. So a capture works
//     regardless of which GPU backend (Graphite/Dawn) the process runs
//     and even with no live GPU context at all. We do NOT serialize a
//     `skgpu::graphite::Recording` (that is the GPU command bundle, not
//     a `.skp`).
//
//   * Capture re-runs the frame's draw ops into a fresh recording
//     canvas. `SkpFrameCapture` owns an `SkPictureRecorder` and exposes
//     a `pulp::canvas::Canvas` over its recording `SkCanvas`, so
//     existing view-paint code paints into the capture exactly as it
//     paints a live frame. Capture is a user-triggered action, never a
//     per-frame cost.
//
//   * The one caveat the spike pinned: `SkPicture::serialize()` drops
//     embedded `SkImage`s to `nullptr` by default. `SkpFrameCapture`
//     always supplies `SkSerialProcs::fImageProc` (PNG-encode) so
//     atlas/image draws survive the round trip. A replayer (skiadebugger,
//     or a test) reads the artifact with the matching
//     `SkDeserialProcs::fImageProc` that decodes the PNG payload back to
//     an `SkImage`.
//
// Graceful degradation: when Skia is not compiled in, `SkpFrameCapture`
// reports `available() == false`, `canvas()` returns `nullptr`, and the
// capture functions return a failed `SkpCaptureResult` with a reason
// string — no crash, no partial file.

#include <pulp/canvas/canvas.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

namespace pulp::render {

/// Outcome of a `.skp` capture attempt. A capture either produces a
/// valid `.skp` artifact (`ok == true`, `bytes_written > 0`) or fails
/// with a human-readable `reason` — never a half-written file.
struct SkpCaptureResult {
    bool ok = false;             ///< True when a `.skp` artifact was produced.
    std::string path;            ///< Filesystem path of the written `.skp`, when ok.
    std::size_t bytes_written = 0; ///< Size of the serialized `.skp` blob.
    std::size_t op_count = 0;    ///< Approximate recorded draw-op count.
    std::string reason;          ///< Failure explanation when `!ok`.

    explicit operator bool() const { return ok; }
};

/// Records a single frame's draw ops into an `SkPicture` and serializes
/// it to a `.skp` artifact.
///
/// Usage:
///   SkpFrameCapture capture(width, height);
///   if (capture.available()) {
///       root_view.paint_all(*capture.canvas());   // re-run the paint
///       auto result = capture.finish_to_file(path);
///   }
///
/// The capture canvas is a normal `pulp::canvas::Canvas`, so any code
/// that paints a live frame paints a capture frame unchanged. After
/// `finish_to_file` / `finish_to_memory` the capture is consumed and
/// `canvas()` returns `nullptr`.
class SkpFrameCapture {
public:
    /// Begin a capture sized to `width` x `height` logical pixels.
    /// The cull rect of the recorded `SkPicture` is `[0,0,width,height]`.
    SkpFrameCapture(int width, int height);
    ~SkpFrameCapture();

    SkpFrameCapture(const SkpFrameCapture&) = delete;
    SkpFrameCapture& operator=(const SkpFrameCapture&) = delete;

    /// True when Skia is compiled in and the recording canvas exists.
    /// False builds degrade gracefully — `canvas()` is `nullptr` and
    /// `finish_*` return a failed result.
    bool available() const;

    /// The canvas to paint the frame into. Valid until `finish_*` is
    /// called or the capture is destroyed; `nullptr` when unavailable.
    canvas::Canvas* canvas();

    /// Finish recording and write the `.skp` to `path`.
    /// Embedded images are PNG-encoded via `SkSerialProcs::fImageProc`.
    /// Consumes the capture: a second call returns a failed result.
    SkpCaptureResult finish_to_file(const std::string& path);

    /// Finish recording and serialize the `.skp` into `out_blob`
    /// (raw `.skp` bytes). Useful for tests and in-memory transport.
    /// Consumes the capture.
    SkpCaptureResult finish_to_memory(std::string& out_blob);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// One-shot helper: capture a frame by handing a paint callback the
/// capture canvas, then writing the `.skp` to `path`.
///
/// The callback paints whatever should appear in the frame. When Skia
/// is unavailable, or `width`/`height` are non-positive, this returns a
/// failed `SkpCaptureResult` with a reason and writes nothing.
SkpCaptureResult capture_skp_to_file(
    int width, int height, const std::string& path,
    const std::function<void(canvas::Canvas&)>& paint);

/// `true` when this build can produce `.skp` captures (Skia compiled
/// in). Lets callers (inspector / CLI) report an honest "unavailable"
/// instead of attempting a doomed capture.
bool skp_capture_supported();

} // namespace pulp::render
