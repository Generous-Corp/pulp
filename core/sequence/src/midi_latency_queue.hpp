#pragma once

#include <pulp/midi/buffer.hpp>
#include <pulp/midi/ump_buffer.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pulp::sequence::detail {

/// Prepared, allocation-free audio-callback queue that applies one fixed
/// latency to paired MIDI 1 and UMP block streams.
class MidiLatencyQueue {
  public:
    bool prepare(std::size_t aggregate_event_capacity, std::uint32_t latency_samples,
                 std::size_t maximum_delayed_events);
    void reset() noexcept;
    void clear_pending() noexcept;
    bool process(const midi::MidiBuffer& source, midi::MidiBuffer& destination,
                 std::uint32_t frames) noexcept;

  private:
    struct DelayedMidiEvent {
        midi::MidiEvent event;
        std::uint64_t due_sample = 0;
    };
    struct DelayedUmpEvent {
        midi::UmpEvent event;
        std::uint64_t due_sample = 0;
    };

    std::vector<DelayedMidiEvent> midi_;
    std::vector<DelayedUmpEvent> ump_;
    std::size_t midi_read_ = 0;
    std::size_t midi_write_ = 0;
    std::size_t midi_count_ = 0;
    std::size_t ump_read_ = 0;
    std::size_t ump_write_ = 0;
    std::size_t ump_count_ = 0;
    std::uint64_t output_frontier_ = 0;
    std::uint32_t latency_samples_ = 0;
};

} // namespace pulp::sequence::detail
