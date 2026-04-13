#pragma once

/// @file ump_buffer.hpp
/// Sample-accurate MIDI 2.0 UMP packet stream.
///
/// `UmpBuffer` parallels `MidiBuffer`: while `MidiBuffer` carries MIDI 1.0
/// short messages, `UmpBuffer` carries native Universal MIDI Packets with
/// full MIDI 2.0 resolution (16-bit velocity, per-note pitch bend,
/// per-note CCs). Processors that set `PluginDescriptor::supports_ump`
/// access the buffer via `Processor::ump_input()` during `process()`.
///
/// Packets carry a `sample_offset` so hosts can deliver sample-accurate
/// timing (same semantics as `MidiEvent::sample_offset`).

#include <pulp/midi/ump.hpp>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace pulp::midi {

/// One UMP packet with a block-relative sample offset.
struct UmpEvent {
    UmpPacket packet;
    int32_t sample_offset = 0;
};

/// A time-ordered list of UMP events for one audio block.
///
/// Ownership and lifetime mirror `MidiBuffer`/`MpeBuffer`: the format
/// adapter owns the buffer, fills it before `process()`, passes a
/// pointer to the processor, and clears it afterwards. Not thread-safe.
class UmpBuffer {
public:
    UmpBuffer() { events_.reserve(kInitialCapacity); }

    void add(const UmpEvent& e) { events_.push_back(e); }
    void add(UmpEvent&& e) { events_.push_back(std::move(e)); }
    void add(const UmpPacket& p, int32_t sample_offset = 0) {
        events_.push_back({p, sample_offset});
    }

    void clear() { events_.clear(); }
    bool empty() const { return events_.empty(); }
    std::size_t size() const { return events_.size(); }

    void sort() {
        std::sort(events_.begin(), events_.end(),
            [](const UmpEvent& a, const UmpEvent& b) {
                return a.sample_offset < b.sample_offset;
            });
    }

    auto begin()       { return events_.begin(); }
    auto end()         { return events_.end(); }
    auto begin() const { return events_.begin(); }
    auto end() const   { return events_.end(); }

    const UmpEvent& operator[](std::size_t i) const { return events_[i]; }

private:
    static constexpr std::size_t kInitialCapacity = 128;
    std::vector<UmpEvent> events_;
};

} // namespace pulp::midi
