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
// | Occurrence signals/counts  | ActivityChannel  | Pad flashes, UI triggers     |
// | Latest-value metering      | TripleBuffer<T>  | Audio→UI meter data          |
// | Prepared read-only pointer | RealtimeResourceSlot<T,N> | prepared samples/IRs |
//
// RealtimeResourceSlot is a raw-pointer publication helper, not reader-pinned
// RCU: one non-RT owner serializes publish/reclaim and reclaims only after a
// block-boundary grace point. Use the Slot/Handoff taxonomy where a site needs
// multi-reader pins or audio-thread ownership transfer.
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
//   - RealtimeResourceSlot publishes a prepared pointer with release/acquire;
//     reclaim retired resources away from the audio thread after a grace point

#include <pulp/runtime/assert.hpp>
#include <pulp/runtime/activity_channel.hpp>
#include <pulp/runtime/alive_token.hpp>
#include <pulp/runtime/background_job.hpp>
#include <pulp/runtime/log.hpp>
#include <pulp/runtime/node_abi.hpp>
#include <pulp/runtime/scope_guard.hpp>
#include <pulp/runtime/spsc_queue.hpp>
#include <pulp/runtime/system.hpp>
#include <pulp/runtime/seqlock.hpp>
#include <pulp/runtime/triple_buffer.hpp>
