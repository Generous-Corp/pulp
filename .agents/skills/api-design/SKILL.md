---
name: api-design
description: >-
  Design rules for new public headers and APIs in Pulp: layering of the audio
  core, strong types for quantities, small composable processors, separation of
  processing, model and presentation, lean includes, and docs order. Consult
  before adding or reshaping a public header under core/*/include/, a new
  pulp::signal processor, or a new top-level API.
---

# Public API and Header Design

The full guide is [docs/guides/api-design.md](../../../docs/guides/api-design.md).
Read it before adding a public header or API. The design lessons draw on
cycfi/q (MIT), credited as inspiration only; copy no code from it.

## Checklist

- **Layering.** The header includes only the standard library and small infra
  (`pulp/runtime`, `pulp/simd`) plus its own layer. No format, I/O or UI types
  in a DSP header.
- **Strong types.** A new API passes frequency, time and gain as strong types
  or literals, not adjacent bare floats. Leave stable signatures alone.
- **Small processors.** One job per class, composed inside `Processor::process()`
  from `pulp::signal` helpers. Reach for `SignalGraph` only for runtime routing.
- **Separation of concerns.** `Processor` processes, `StateStore` and
  parameters hold the model, `editor_factory()` and `format::Editor` present it.
- **Expose the pipeline.** Anything a widget or test could draw or verify is a
  public pure function (see "Expose the Pipeline, Not Just the Endpoints" in
  `CLAUDE.md`).
- **Lean headers.** Minimal includes, forward declarations where enough, no
  reliance on transitive includes. `tools/scripts/header_fanout_guard.py` checks
  fan-out growth.
- **Docs.** Fundamentals first, then graduated examples, then reference.
  Examples compile or are marked illustrative.

## When to deviate

A stable signature keeps its shape. A genuine layering exception needs a
stated reason in the PR description.
