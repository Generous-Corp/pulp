---
breaking = true
summary = "A contract editor that declares no minimum size is now fixed-size"
---

## Breaking changes
- An editor reached through `Processor::editor_factory()` is resizable only when
  `Editor::size_hints()` sets both minimums and the bounds leave room. Before,
  every editor except one with `min == max` was resizable and unbounded. An
  editor that keeps the default hints (a preferred size only) is now fixed-size
  in VST3, CLAP, AU v2 and the macOS standalone window, as ViewBridge editors
  already were.
- A locked `aspect_ratio` is now enforced for these editors in every format. A
  size the host sets without asking first is brought inside the limits and
  aspect, and the host is asked for the constrained size once its sizes settle.

## Migration steps
- If the editor should resize, return `min_width` and `min_height` (both above
  zero) from `size_hints()`, with `max_width` / `max_height` as needed and
  `aspect_ratio` to lock the proportions.
- If the editor should stay fixed-size, nothing changes.
