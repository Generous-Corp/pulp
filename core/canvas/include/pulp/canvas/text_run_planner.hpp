// text_run_planner.hpp
//
// `TextRunPlanner` is the entry point that converts an input string +
// `FontOptions` into a `ShapedText` artifact. Both measurement and paint
// pipelines are intended to consume the same artifact, keeping text metrics,
// run segmentation, and cluster boundaries aligned across layout and rendering.
// The planner resolves the font cascade, splits text into bidi/script runs when
// ICU-backed SkShaper iterators are available, builds grapheme-cluster and
// Unicode index maps, and falls back to a single compatible run on non-Skia
// builds.

#pragma once

#include "pulp/canvas/font_options.hpp"
#include "pulp/canvas/shaped_text.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pulp::canvas {

class TextRunPlanner {
public:
    /// Process-wide singleton. Thread-safe.
    static TextRunPlanner& instance();

    /// Produce a `ShapedText` for `text` under `options`. The planner
    /// is responsible for: resolving the typeface cascade
    /// (delegating to `FontResolver`), shaping each run via SkShaper
    /// (or falling back to the width-estimator on non-Skia builds),
    /// computing per-run metrics, populating the cluster table +
    /// line-break opportunities + Unicode index map, and recording
    /// fallback / synthesis traces on the runs.
    ///
    /// Calling with the same arguments is cache-hit cheap — the
    /// internal cache is keyed on the full `FontOptions` blob plus
    /// the text (interned). The cache invalidates automatically when
    /// `FontScope::generation()` advances on any consulted scope,
    /// because `options.registry_generation` is part of the key.
    ShapedText shape(std::string_view text, const FontOptions& options);

    /// Shape N independent inputs in parallel and return the artifacts
    /// in input order. Same output as N sequential `shape()` calls.
    /// Uses `std::async(launch::async, ...)` to fan out work; the
    /// resolver, FontFlightRecorder, and per-planner cache are all
    /// synchronized internally. The cache lookup happens once per future,
    /// so duplicate inputs across the batch coalesce as expected.
    ///
    /// Shapes SERIALLY, in the same input order and with identical output,
    /// where there is no thread to fan out to: a non-pthread wasm build
    /// (`__EMSCRIPTEN__`, where `std::async(launch::async, ...)` throws), and
    /// natively under `PULP_TEXT_SHAPE_SERIAL` so that arm stays testable.
    ///
    /// The serial `shape()` API is preferred for one-off labels; this
    /// batch entry point is intended for design-tool panels, docs
    /// views, or any other surface that needs to lay out many
    /// independent paragraphs at startup. Inputs are owned by the
    /// caller via `std::string` for thread safety; small allocations
    /// are cheap relative to the shaping cost we parallelise.
    std::vector<ShapedText> shape_batch(
        const std::vector<std::pair<std::string, FontOptions>>& inputs);

    /// Test-only: discard the internal cache. Production code never
    /// calls this — invalidation flows through the scope generation
    /// counter automatically.
    void clear_cache();

private:
    TextRunPlanner();
    ~TextRunPlanner();
    TextRunPlanner(const TextRunPlanner&) = delete;
    TextRunPlanner& operator=(const TextRunPlanner&) = delete;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pulp::canvas
