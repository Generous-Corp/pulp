#pragma once

#include <pulp/midi/message.hpp>
#include <cstdint>
#include <vector>
#include <algorithm>
#include <limits>

namespace pulp::midi {

/// Collection of timestamped MIDI events within a single audio buffer period.
///
/// Events are appended via add() and should be sorted by sample_offset
/// (call sort()) before iterating in the audio callback. Supports
/// range-based for loops.
///
/// @code
/// MidiBuffer buf;
/// buf.add(MidiEvent::note_on(0, 60, 100));
/// buf.sort();
/// for (const auto& ev : buf) { /* process */ }
/// @endcode
class MidiBuffer {
public:
    MidiBuffer() = default;

    /// Append a MIDI event to the buffer.
    bool add(const MidiEvent& event) {
        if (!can_append_event()) {
            record_event_drop();
            return false;
        }
        events_.push_back(event);
        return true;
    }

    /// @copydoc add(const MidiEvent&)
    bool add(MidiEvent&& event) {
        if (!can_append_event()) {
            record_event_drop();
            return false;
        }
        events_.push_back(std::move(event));
        return true;
    }

    /// Remove all events.
    void clear() {
        events_.clear();
        dropped_events_ = 0;
    }
    bool empty() const { return events_.empty(); }
    std::size_t size() const { return events_.size(); }

    /// Preallocate storage for realtime callers that append during process().
    void reserve(std::size_t event_capacity, std::size_t sysex_capacity = 0) {
        events_.reserve(event_capacity);
        sysex_.reserve(sysex_capacity);
    }

    /// When enabled, add()/add_sysex() drop once reserved capacity is full
    /// instead of growing vectors. Intended for adapter-owned realtime buffers.
    void set_realtime_capacity_limit(bool enabled = true) {
        limit_to_reserved_capacity_ = enabled;
    }
    bool realtime_capacity_limited() const { return limit_to_reserved_capacity_; }
    std::size_t event_capacity() const { return events_.capacity(); }
    std::size_t sysex_capacity() const { return sysex_.capacity(); }
    std::uint32_t dropped_event_count() const { return dropped_events_; }
    std::uint32_t dropped_sysex_count() const { return dropped_sysex_events_; }

    /// Sort events by sample_offset for sample-accurate processing.
    /// Call this before iterating in the audio callback. Sorting is
    /// in-place over the existing event storage; realtime callers must not
    /// append while sorting and should rely on adapters to have bounded the
    /// event count before process().
    void sort() {
        std::sort(events_.begin(), events_.end(),
            [](const MidiEvent& a, const MidiEvent& b) {
                return a.sample_offset < b.sample_offset;
            });
    }

    auto begin() { return events_.begin(); }
    auto end() { return events_.end(); }
    auto begin() const { return events_.begin(); }
    auto end() const { return events_.end(); }

    const MidiEvent& operator[](std::size_t index) const { return events_[index]; }

    /// Attach a UmpBuffer sidecar carrying MIDI 2.0 packets that can't be
    /// represented as choc::midi::ShortMessage (UMP type-4 channel voice,
    /// type-3/5 data, type-0 utility, etc.). Format adapters set this
    /// before process(); plugins opting in to
    /// PluginDescriptor::supports_ump read it via ump(). Ownership stays
    /// with the caller; the buffer must outlive the process() block.
    /// Workstream 02 slice 2.6.
    void attach_ump(class UmpBuffer* ump) { ump_ = ump; }

    /// Attached UmpBuffer or nullptr. A null return means "no UMP events
    /// this block", not "UMP unsupported" — that's declared at descriptor
    /// time via PluginDescriptor::supports_ump.
    const class UmpBuffer* ump() const { return ump_; }
    class UmpBuffer* ump() { return ump_; }

    // ── SysEx sidecar (workstream 01 — full MIDI vocabulary) ──────────────
    //
    // choc::midi::ShortMessage is fixed 3 bytes; system-exclusive payloads
    // can run to kilobytes. Sysex therefore travels in a parallel vector
    // whose entries are referenced by sample_offset the same way MidiEvent
    // entries are. Format adapters that carry sysex (CoreMIDI, VST3 event
    // list with kData type, CLAP CLAP_EVENT_MIDI_SYSEX) populate this
    // alongside the short-message stream; plugins that don't care can
    // ignore it.
    struct SysexEvent {
        std::vector<uint8_t> data;   ///< full F0 .. F7 payload
        int32_t sample_offset = 0;   ///< sample position within the block
        double  timestamp = 0.0;     ///< absolute time in seconds
    };

    bool add_sysex(std::vector<uint8_t> data, int32_t sample_offset = 0, double ts = 0.0) {
        if (!can_append_sysex()) {
            record_sysex_drop();
            return false;
        }
        sysex_.push_back({std::move(data), sample_offset, ts});
        return true;
    }
    bool add_sysex(SysexEvent&& event) {
        if (!can_append_sysex()) {
            record_sysex_drop();
            return false;
        }
        sysex_.push_back(std::move(event));
        return true;
    }
    bool add_sysex_copy(const uint8_t* data,
                        std::size_t size,
                        int32_t sample_offset = 0,
                        double ts = 0.0) {
        if (!can_append_sysex()) {
            record_sysex_drop();
            return false;
        }
        sysex_.push_back({
            std::vector<uint8_t>(data, data + size),
            sample_offset,
            ts,
        });
        return true;
    }
    void clear_sysex() {
        sysex_.clear();
        dropped_sysex_events_ = 0;
    }
    std::size_t sysex_size() const { return sysex_.size(); }
    const std::vector<SysexEvent>& sysex() const { return sysex_; }
    std::vector<SysexEvent>& sysex() { return sysex_; }

private:
    bool can_append_event() const {
        return !limit_to_reserved_capacity_ || events_.size() < events_.capacity();
    }
    bool can_append_sysex() const {
        return !limit_to_reserved_capacity_ || sysex_.size() < sysex_.capacity();
    }
    static void saturating_increment(std::uint32_t& value) {
        if (value < std::numeric_limits<std::uint32_t>::max()) {
            ++value;
        }
    }
    void record_event_drop() { saturating_increment(dropped_events_); }
    void record_sysex_drop() { saturating_increment(dropped_sysex_events_); }

    std::vector<MidiEvent> events_;
    std::vector<SysexEvent> sysex_;
    class UmpBuffer* ump_ = nullptr;
    bool limit_to_reserved_capacity_ = false;
    std::uint32_t dropped_events_ = 0;
    std::uint32_t dropped_sysex_events_ = 0;
};

} // namespace pulp::midi
