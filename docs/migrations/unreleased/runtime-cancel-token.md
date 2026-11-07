---
breaking = false
summary = "CancelSource and CancelToken replace CancellationToken"
---

## What changed
- `pulp/runtime/cancel.hpp` adds `CancelSource` (move-only owner that cancels on
  destruction), `CancelToken` (copyable observer) and `CancelRegistration`.
- `CancelToken::is_cancelled()` is lock-free and allocation-free, so it can be
  polled from the audio thread. `wait_for_cancellation(timeout)` and
  `register_callback(fn)` are for non-realtime threads.
- `AsyncStream`, `BackgroundJobContext`, `download_file`, `ModelStore` and
  `SearchIndex` now take or hand out `CancelToken`.

## Migration steps
- `CancellationToken` still exists and still converts to `CancelToken`, so
  existing callers compile unchanged. Prefer holding a `CancelSource` and
  passing its `token()`.
- `AsyncStream::cancellation_token()` and `BackgroundJobContext::cancellation_token()`
  now return `CancelToken`. Code that called `cancel()` on the returned value
  must keep its own `CancelSource` (or `CancellationToken`) instead.
- Include `<pulp/runtime/cancel.hpp>`. `background_job.hpp` no longer pulls in
  `async_stream.hpp`.
