// text_run_planner.hpp
//
// Pulp #2163 follow-up — Phase 1 / Slice 1.2.a of the font-subsystem-
// hardening v2 roadmap.
//
// `TextRunPlanner` is the entry point that converts an input string +
// `FontOptions` into a `ShapedText` artifact. Both measurement and paint
// pipelines consume the same artifact; that's how v2 closes the v1 doc's
// "measurement ≠ paint" killer gap.
//
// Slice 1.2.a skeleton (this file): the planner is a thin wrapper over
// the existing TextShaper / SkShaper path. It emits one ShapedRun for
// the input (no real bidi/script split) and a per-codepoint cluster
// table (no UAX #29 grouping yet). Downstream consumers can target the
// declared API today; the bidi/script/cluster correctness work happens
// in 1.2.a finish without changing the API surface.

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

    /// pulp #2163 / font v2 Slice 3.7 — parallel shaping (opportunistic).
    /// Shape N independent inputs in parallel and return the artifacts
    /// in input order. Same output as N sequential `shape()` calls.
    /// Uses `std::async(launch::async, ...)` to fan out work; the
    /// resolver, FontShapedTextResult, and FontFlightRecorder are all
    /// thread-safe (internal mutexes). The cache lookup happens once
    /// per future, so duplicate inputs across the batch coalesce as
    /// expected.
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
