---
breaking = false
summary = "set_log_sink adds file and capture sinks; logging is documented as not realtime-safe"
---

## What changed
- `set_log_sink` receives `(level, category, message)` for every record, in addition
  to the platform log and stderr.
- `FileLogSink` (size-rotated, owner-only, refuses symlinked or group/world-writable
  targets), `CaptureLogSink` and `ScopedLogSink` live in `pulp/runtime/log_sink.hpp`.
- `log_with_category` tags a record with a subsystem name. Existing helpers use the
  category `pulp`.

## Migration steps
- None required. Existing `log_info`, `log_warn` and `log_error` calls behave as before.
- Do not call any of the log functions from the audio thread; they allocate and lock.
