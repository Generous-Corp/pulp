#pragma once

// Processor-facing helper for sample-accurate MIDI work.
//
// for_each_midi_subblock() splits a process block at MIDI-event sample
// offsets and invokes a callback once per contiguous audio sub-block, handing
// the callback the events that fall at that sub-block's leading boundary. It is
// the MIDI counterpart to for_each_subblock() in param_processing.hpp: instead
// of splitting at parameter-event offsets it splits at MIDI-event offsets, so a
// Processor can apply events, render up to the next event, apply those, and so
// on, without hand-rolling a per-sample event cursor.
//
// Realtime contract: the helper does not allocate, lock, or copy event
// payloads on the audio thread. It walks the MidiBuffer by index and hands the
// callback a std::span over the buffer's existing, contiguous, sorted storage.
// Callers must sort() the MidiBuffer before calling (the buffer's sort() is
// itself allocation-free once its scratch is reserved). prepare()/reserve()
// work belongs off the audio thread.

#include <pulp/audio/buffer.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/midi/message.hpp>
#include <cstddef>
#include <span>
#include <utility>

namespace pulp::format {

/// Split a process block at MIDI-event boundaries.
///
/// The block @p output / @p input is divided into contiguous sub-blocks at the
/// sample offsets carried by @p midi (which must already be sorted by
/// sample_offset). For each sub-block @p fn is invoked with:
///   - an output BufferView slice covering the sub-block,
///   - the matching input BufferView slice,
///   - a std::span<const midi::MidiEvent> of the events whose offset coincides
///     with the sub-block's leading sample.
///
/// Boundary semantics, matching the parameter helper:
///   - Events at or before sample 0 (offset <= 0) are grouped onto the first
///     sub-block's leading boundary, so the callback can apply them before any
///     audio is rendered.
///   - Events sharing a sample offset are grouped into a single boundary span;
///     a coincident offset is not re-split into zero-length sub-blocks.
///   - Events at or past the block end (offset >= num_samples) are ignored;
///     there is no audio left to render after them this block, so a host that
///     time-stamps an event at the block boundary sees it next block. Clamping
///     them into the final sub-block would reorder them relative to the audio,
///     so ignoring is the safe choice.
///   - A zero-frame block invokes the callback zero times (nothing to render),
///     mirroring for_each_subblock().
///   - An empty buffer (or one with only out-of-range events) invokes the
///     callback exactly once with the full block and an empty event span.
///
/// The output/input slices and the event span handed to @p fn are borrowed
/// views into the caller's buffers, valid only for the duration of that single
/// callback invocation. Do not retain them past the call, and do not mutate
/// @p midi while iterating.
template <typename Fn>
void for_each_midi_subblock(audio::BufferView<float>& output,
                            const audio::BufferView<const float>& input,
                            const midi::MidiBuffer& midi,
                            Fn&& fn)
{
    const auto total = output.num_samples();
    if (total == 0) return;

    const std::size_t event_count = midi.size();

    // The boundary walk assumes nondecreasing sample offsets (the caller must
    // sort() first). Unsorted input is not UB here — it only mis-times events —
    // so this guard is debug-only and compiles out of the release audio path.
#ifndef NDEBUG
    for (std::size_t i = 1; i < event_count; ++i) {
        assert(midi[i].sample_offset >= midi[i - 1].sample_offset
               && "for_each_midi_subblock requires a sorted MidiBuffer");
    }
#endif

    auto emit = [&](std::size_t block_start,
                    std::size_t block_end,
                    std::size_t event_first,
                    std::size_t event_last) {
        if (block_end <= block_start) return;
        auto out_slice = output.slice(block_start, block_end - block_start);
        auto in_slice = input.slice(block_start, block_end - block_start);
        const auto boundary = event_last > event_first
            ? std::span<const midi::MidiEvent>(&midi[event_first],
                                               event_last - event_first)
            : std::span<const midi::MidiEvent>{};
        fn(out_slice, in_slice, boundary);
    };

    std::size_t start = 0;       // leading sample of the current sub-block
    std::size_t idx = 0;         // next unconsumed event index
    std::size_t boundary_first = 0;  // event range applied at `start`

    // Group all events with offset <= 0 onto the first sub-block boundary.
    while (idx < event_count
           && midi[idx].sample_offset <= 0) {
        ++idx;
    }
    std::size_t boundary_last = idx;

    while (idx < event_count) {
        const auto offset = midi[idx].sample_offset;
        if (static_cast<std::size_t>(offset) >= total) {
            // Remaining events are at/after the block end: ignored this block.
            break;
        }

        const auto split = static_cast<std::size_t>(offset);
        if (split > start) {
            emit(start, split, boundary_first, boundary_last);
            start = split;
            boundary_first = idx;
        }

        // Group coincident-offset events into this boundary.
        while (idx < event_count
               && midi[idx].sample_offset == offset) {
            ++idx;
        }
        boundary_last = idx;
    }

    if (start < total) {
        emit(start, total, boundary_first, boundary_last);
    }
}

} // namespace pulp::format
