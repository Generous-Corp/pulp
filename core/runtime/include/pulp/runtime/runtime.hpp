#pragma once

// Convenience header — includes all runtime utilities
//
// ── Thread synchronization strategy ─────────────────────────────────────
//
// Pulp uses different primitives depending on the data flow pattern:
//
// | Pattern                    | Primitive        | Example                      |
// |----------------------------|------------------|------------------------------|
// | Single value, latest-wins  | std::atomic<T>   | Parameter values, flags      |
// | Multi-field coherent read  | SeqLock<T>       | Transport state (tempo+beat) |
// | Large data swap            | TripleBuffer<T>  | Wavetables, IR buffers       |
// | Ordered event stream       | SPSC FIFO        | MIDI events, UI commands     |
// | Latest-value metering      | TripleBuffer<T>  | Audio→UI meter data          |
//
// NEVER use on the audio thread:
//   std::mutex, std::condition_variable, heap allocation, I/O
//
// Memory ordering:
//   - ParamValue uses relaxed atomics (independent single values, no ordering
//     dependency between parameters)
//   - SeqLock uses acquire/release (ensures coherent multi-field snapshots)
//   - TripleBuffer uses acquire/release on the flag word
//   - EventLoop uses acquire/release + condition_variable (UI thread only)

#include <pulp/runtime/assert.hpp>
#include <pulp/runtime/log.hpp>
#include <pulp/runtime/scope_guard.hpp>
#include <pulp/runtime/spsc_queue.hpp>
#include <pulp/runtime/system.hpp>
#include <pulp/runtime/seqlock.hpp>
#include <pulp/runtime/triple_buffer.hpp>
