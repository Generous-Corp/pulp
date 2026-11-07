---
breaking = false
summary = "FixedFunction adds an allocation-free callable; BackgroundTaskLane accepts a capturing handler"
---

## What changed
- `pulp/runtime/fixed_function.hpp` adds `FixedFunction<Signature, Capacity>`, a move-only
  callable with inline storage and compile-time size checks.
- `format::BackgroundTaskLane::start` has a new overload taking a `HandlerFunction`
  (a `FixedFunction`), so a capturing lambda replaces the `void*` context.

## Migration steps
- None required. `start(handler, context, policy)` with a function pointer still works.
- The lane no longer exposes its stored handler or context members; they were private.
