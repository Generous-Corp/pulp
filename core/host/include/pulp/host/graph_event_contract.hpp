#pragma once

/// Bounded event contracts shared by host ingress and graph-authored nodes.
///
/// This header deliberately stops at the event boundary.  Host records are
/// projected into block-relative MIDI/UMP events and an MPE sidecar by the
/// owner of the graph; they are not silently injected into SignalGraph's
/// block-local MIDI mailbox.  Graph-authored events use the same checked
/// epoch/sample/sequence rules as the admitted trigger runtime, while keeping
/// the producer and consumer phases explicit for future graph bindings.

#include <pulp/midi/buffer.hpp>
#include <pulp/midi/mpe_buffer.hpp>
#include <pulp/midi/ump_buffer.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace pulp::host {

namespace detail {
struct GraphEventLaneTestAccess;
} // namespace detail

enum class HostEventResult : std::uint8_t {
    Accepted,
    StaleEpoch,
    Unbound,
    OutOfRange,
    Late,
    Overflow,
    InvalidSequence,
    OutputCapacity,
    Busy,
};

/// One immutable host event in the graph's epoch-local sample domain.
///
/// `absolute_sample` is intentionally signed at the boundary so a negative
/// adapter conversion is refused instead of wrapping into a very large future
/// event.  `producer_sequence` is supplied by the adapter and is used only to
/// order equal-time events from the same producer; graph-owned sequences are
/// minted by `GraphAuthoredEventLane` below.
struct HostEventRecord {
    std::uint64_t epoch = 0;
    std::int64_t absolute_sample = 0;
    std::uint64_t producer_sequence = 0;
    std::uint32_t producer = 0;
    midi::MidiEvent midi{};
    midi::UmpPacket ump{};
    bool has_ump = false;
};

/// Fixed-capacity host ingress staging area.
///
/// Publication and block projection do not allocate.  Future events remain
/// staged for a later block; events before the current block are refused as
/// late, so a caller cannot accidentally retime them to offset zero.  A
/// projection is transactional: insufficient output capacity leaves both the
/// staged records and the destination span untouched.
template <std::size_t Capacity = 256> class HostEventIngress final {
  public:
    static_assert(Capacity > 0);

    HostEventResult begin_block(std::uint64_t epoch, std::uint64_t sample_start,
                                std::size_t frames) noexcept {
        if (active_)
            return HostEventResult::Busy;
        if (epoch == 0 || frames == 0 || frames > kMaxFrames ||
            sample_start > std::numeric_limits<std::uint64_t>::max() - frames)
            return HostEventResult::OutOfRange;
        // Staged records belong to one activation epoch. A new epoch starts
        // with an empty staging area so a future record from the retired
        // epoch cannot be projected into the replacement graph.
        if (epoch != epoch_)
            size_ = 0;
        // A forward jump (seek/loop) can leave staged records behind the new
        // window; they can never be projected and publish() refuses late
        // records, so evict them rather than leak staging capacity.
        if (epoch == epoch_) {
            std::size_t keep = 0;
            for (std::size_t i = 0; i < size_; ++i) {
                if (records_[i].epoch == epoch &&
                    static_cast<std::uint64_t>(records_[i].absolute_sample) >= sample_start)
                    records_[keep++] = records_[i];
            }
            size_ = keep;
        }
        epoch_ = epoch;
        sample_start_ = sample_start;
        frames_ = frames;
        active_ = true;
        return HostEventResult::Accepted;
    }

    HostEventResult publish(const HostEventRecord& event) noexcept {
        if (!active_ || event.epoch != epoch_)
            return HostEventResult::StaleEpoch;
        if (event.producer == 0)
            return HostEventResult::Unbound;
        if (event.absolute_sample < 0)
            return HostEventResult::OutOfRange;
        if (static_cast<std::uint64_t>(event.absolute_sample) < sample_start_)
            return HostEventResult::Late;
        for (std::size_t i = 0; i < size_; ++i) {
            // Sequence numbers are a producer-local identity/tie key.  Host
            // adapters may publish records out of timestamp order within the
            // current block, so reject only a duplicate sequence rather than
            // treating a lower (but unique) sequence as a mutation.
            if (records_[i].producer == event.producer &&
                event.producer_sequence == records_[i].producer_sequence)
                return HostEventResult::InvalidSequence;
        }
        if (size_ == Capacity)
            return HostEventResult::Overflow;
        records_[size_++] = event;
        return HostEventResult::Accepted;
    }

    /// Copy events in `[sample_start, sample_start + frames)` into `out`.
    /// The copied records retain absolute coordinates; callers derive the
    /// block-relative offset only after this preflight succeeds.
    HostEventResult project(std::span<HostEventRecord> out, std::size_t& count) noexcept {
        count = 0;
        if (!active_)
            return HostEventResult::StaleEpoch;
        // A caller may only have one in-flight projection.  Keep the
        // destructive queue operation paired with rollback_projection() so a
        // downstream capacity refusal can retry the same block verbatim.
        if (projection_backup_valid_)
            return HostEventResult::Busy;
        const auto end = sample_start_ + frames_;
        std::size_t matching = 0;
        for (std::size_t i = 0; i < size_; ++i) {
            if (records_[i].epoch != epoch_)
                continue;
            const auto sample = static_cast<std::uint64_t>(records_[i].absolute_sample);
            if (sample >= sample_start_ && sample < end)
                ++matching;
        }
        if (matching > out.size())
            return HostEventResult::OutputCapacity;
        projection_backup_ = records_;
        projection_backup_size_ = size_;
        projection_backup_valid_ = true;
        for (std::size_t i = 0; i < size_; ++i) {
            if (records_[i].epoch != epoch_)
                continue;
            const auto sample = static_cast<std::uint64_t>(records_[i].absolute_sample);
            if (sample >= sample_start_ && sample < end)
                out[count++] = records_[i];
        }
        // Insertion sort is bounded and allocation-free.  Producer identity is
        // the deterministic tie key when two producers use the same sequence.
        for (std::size_t i = 1; i < count; ++i) {
            auto value = out[i];
            std::size_t j = i;
            while (j > 0 && less(value, out[j - 1])) {
                out[j] = out[j - 1];
                --j;
            }
            out[j] = value;
        }
        std::size_t write = 0;
        for (std::size_t i = 0; i < size_; ++i) {
            if (records_[i].epoch != epoch_)
                continue;
            const auto sample = static_cast<std::uint64_t>(records_[i].absolute_sample);
            if (sample >= end)
                records_[write++] = records_[i];
        }
        size_ = write;
        return HostEventResult::Accepted;
    }

    /// Restore the records removed by the most recent successful project().
    /// The caller must invoke this before end_block() when a downstream
    /// projection or publication fails.  The original insertion order and
    /// producer identities are restored exactly for a retry.
    void rollback_projection() noexcept {
        if (!projection_backup_valid_)
            return;
        records_ = projection_backup_;
        size_ = projection_backup_size_;
        projection_backup_valid_ = false;
    }

    /// Finish the current block.  Future records remain staged.
    HostEventResult end_block() noexcept {
        if (!active_)
            return HostEventResult::StaleEpoch;
        active_ = false;
        // Reaching the block boundary commits a successful projection.  A
        // refusal path rolls back before sealing, leaving this flag clear.
        projection_backup_valid_ = false;
        return HostEventResult::Accepted;
    }

    void reset(std::uint64_t epoch) noexcept {
        epoch_ = epoch;
        sample_start_ = 0;
        frames_ = 0;
        size_ = 0;
        active_ = false;
        projection_backup_size_ = 0;
        projection_backup_valid_ = false;
    }
    std::uint64_t epoch() const noexcept {
        return epoch_;
    }
    std::size_t pending() const noexcept {
        return size_;
    }
    bool active() const noexcept {
        return active_;
    }
    /// Frame count of the window fixed by the most recent begin_block().
    std::size_t frames() const noexcept {
        return frames_;
    }

  private:
    static constexpr std::size_t kMaxFrames = 4096;
    static bool less(const HostEventRecord& lhs, const HostEventRecord& rhs) noexcept {
        const auto lsample = static_cast<std::uint64_t>(lhs.absolute_sample);
        const auto rsample = static_cast<std::uint64_t>(rhs.absolute_sample);
        if (lsample != rsample)
            return lsample < rsample;
        if (lhs.producer != rhs.producer)
            return lhs.producer < rhs.producer;
        return lhs.producer_sequence < rhs.producer_sequence;
    }

    std::array<HostEventRecord, Capacity> records_{};
    std::array<HostEventRecord, Capacity> projection_backup_{};
    std::uint64_t epoch_ = 0;
    std::uint64_t sample_start_ = 0;
    std::size_t frames_ = 0;
    std::size_t size_ = 0;
    std::size_t projection_backup_size_ = 0;
    bool projection_backup_valid_ = false;
    bool active_ = false;
};

/// Convert projected records to the existing MPE sidecar.  The tracker and
/// sidecar are caller-owned and must be prepared with their realtime capacity
/// before a callback uses this helper.
inline HostEventResult derive_mpe(std::span<const HostEventRecord> events,
                                  std::uint64_t block_start, midi::MpeVoiceTracker& tracker,
                                  midi::MpeBuffer& out) noexcept {
    bool dropped = false;
    int32_t offset = 0;
    midi::bind_tracker_to_buffer(tracker, out, offset, &dropped);
    // A bounded tracker can defer note-offs when the previous block's sidecar
    // was full. Flush that lifecycle queue at block start before consuming new
    // host records, matching the tracker/sidecar contract and preserving note
    // retirement order across blocks.
    if (!tracker.flush_pending_note_offs())
        return HostEventResult::Overflow;
    for (const auto& event : events) {
        if (event.absolute_sample < 0)
            return HostEventResult::OutOfRange;
        const auto absolute = static_cast<std::uint64_t>(event.absolute_sample);
        if (absolute < block_start ||
            absolute - block_start >
                static_cast<std::uint64_t>(std::numeric_limits<int32_t>::max()))
            return HostEventResult::OutOfRange;
        offset = static_cast<int32_t>(absolute - block_start);
        if (event.has_ump)
            (void)tracker.process(event.ump);
        else
            (void)tracker.process(event.midi);
    }
    return dropped ? HostEventResult::Overflow : HostEventResult::Accepted;
}

/// Project an already-sorted absolute host-event block into the fixed MIDI,
/// UMP, and optional MPE sidecars consumed by SignalGraph's existing ingress
/// mailbox. The caller owns and prepares the destination buffers before the
/// realtime boundary. Every record is checked before any destination is
/// mutated, so an out-of-window record or insufficient sidecar capacity leaves
/// all three destinations unchanged.
///
/// This is deliberately an adapter, not a scheduler: `block_start` and
/// `frames` define the one block being delivered, and records outside that
/// window are refused rather than retimed. The resulting buffers can be
/// passed directly to `SignalGraph::inject_midi(node, midi, mpe)`; that API
/// preserves the same payload on both the routed and legacy execution paths.
inline HostEventResult project_host_event_block(std::span<const HostEventRecord> records,
                                                std::uint64_t block_start, std::size_t frames,
                                                midi::MidiBuffer& midi_out,
                                                midi::MpeVoiceTracker* tracker = nullptr,
                                                midi::MpeBuffer* mpe_out = nullptr) noexcept {
    if (frames == 0 || frames > 4096 ||
        block_start > std::numeric_limits<std::uint64_t>::max() - frames)
        return HostEventResult::OutOfRange;
    if ((tracker == nullptr) != (mpe_out == nullptr))
        return HostEventResult::Unbound;

    std::size_t midi_count = 0;
    std::size_t ump_count = 0;
    for (const auto& record : records) {
        if (record.absolute_sample < 0)
            return HostEventResult::OutOfRange;
        const auto absolute = static_cast<std::uint64_t>(record.absolute_sample);
        if (absolute < block_start || absolute >= block_start + frames ||
            absolute - block_start >
                static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()))
            return HostEventResult::Late;
        ++midi_count;
        if (record.has_ump)
            ++ump_count;
    }
    if (midi_out.realtime_capacity_limited() &&
        midi_count >
            midi_out.event_capacity() - std::min(midi_out.event_capacity(), midi_out.size()))
        return HostEventResult::OutputCapacity;
    auto* ump = midi_out.ump();
    if (ump_count != 0 && ump == nullptr)
        return HostEventResult::OutputCapacity;
    if (ump != nullptr && ump->realtime_capacity_limited() &&
        ump_count > ump->capacity() - std::min(ump->capacity(), ump->size()))
        return HostEventResult::OutputCapacity;
    if (mpe_out != nullptr) {
        // Channel-level MPE expression can fan out to every active note. A
        // note lifecycle contributes at most two records (retire + replace),
        // so this is the exact fixed upper bound for one source record. Check
        // the bound before deriving into the caller's sidecar; otherwise a
        // full 128-note pitch-bend fan-out could partially mutate it before
        // reporting overflow.
        constexpr auto kMpeRecordsPerSource = midi::MpeVoiceTracker::kMaxNotes + 2;
        const auto event_mpe =
            records.size() > std::numeric_limits<std::size_t>::max() / kMpeRecordsPerSource
                ? std::numeric_limits<std::size_t>::max()
                : records.size() * kMpeRecordsPerSource;
        const auto pending_mpe = tracker->pending_note_off_count();
        const auto required_mpe = pending_mpe > std::numeric_limits<std::size_t>::max() - event_mpe
                                      ? std::numeric_limits<std::size_t>::max()
                                      : pending_mpe + event_mpe;
        if (required_mpe > mpe_out->capacity() - std::min(mpe_out->capacity(), mpe_out->size()))
            return HostEventResult::OutputCapacity;
    }

    for (const auto& record : records) {
        auto event = record.midi;
        event.sample_offset = static_cast<std::int32_t>(
            static_cast<std::uint64_t>(record.absolute_sample) - block_start);
        if (!midi_out.add(event))
            return HostEventResult::OutputCapacity;
        if (record.has_ump && !ump->add(record.ump, event.sample_offset))
            return HostEventResult::OutputCapacity;
    }
    midi_out.sort();
    if (ump != nullptr)
        ump->sort();
    if (tracker != nullptr && mpe_out != nullptr) {
        const auto result = derive_mpe(records, block_start, *tracker, *mpe_out);
        if (result != HostEventResult::Accepted)
            return result;
    }
    return HostEventResult::Accepted;
}

enum class GraphEventResult : std::uint8_t {
    Accepted,
    StaleEpoch,
    Unbound,
    OutOfRange,
    Future,
    Late,
    Overflow,
    AttemptLimit,
    InvalidPhase,
    Busy,
};

struct GraphAuthoredEvent {
    std::uint64_t epoch = 0;
    std::uint64_t sample = 0;
    std::uint64_t sequence = 0;
    std::uint32_t producer = 0;
    std::uint16_t payload = 0;
    std::uint8_t source_port = 0;
    std::uint8_t kind = 1;
    friend bool operator==(const GraphAuthoredEvent&, const GraphAuthoredEvent&) = default;
};
static_assert(sizeof(GraphAuthoredEvent) == 32);

struct GraphEventCounters {
    std::uint64_t cursor = 0;
    std::uint64_t sequence = 0;
    std::uint64_t epoch = 0;
    std::uint64_t overflow = 0;
    std::uint64_t diagnostic_refusals = 0;
    std::uint8_t attempts = 0;
    std::uint8_t accepted = 0;
};

/// Explicit producer/consumer phases for a bounded, single-producer event
/// lane.  This is a contract primitive; SignalGraph remains the owner of
/// admission, topology and publication of a concrete node pair.
template <std::size_t MaxFrames = 4096, std::size_t MaxAcceptedPerSample = 8>
class GraphAuthoredEventLane final {
  public:
    static constexpr std::size_t kMaxAttemptsPerSample = 16;
    static constexpr std::size_t kRecordBytes =
        sizeof(GraphAuthoredEvent) * MaxFrames * MaxAcceptedPerSample;
    static_assert(kRecordBytes <= 1'048'576);

    GraphEventResult reset(std::uint64_t epoch) noexcept {
        if (epoch == 0 || active_)
            return active_ ? GraphEventResult::Busy : GraphEventResult::OutOfRange;
        epoch_ = epoch;
        counters_ = {};
        counters_.epoch = epoch;
        cursor_ = 0;
        sequence_ = 0;
        trace_size_ = 0;
        consumed_frontier_ = 0;
        return GraphEventResult::Accepted;
    }

    GraphEventResult begin_block(std::uint64_t epoch, std::uint64_t sample_start,
                                 std::size_t frames) noexcept {
        if (active_)
            return GraphEventResult::Busy;
        if (epoch == 0 || epoch != epoch_ || frames == 0 || frames > MaxFrames ||
            sample_start > std::numeric_limits<std::uint64_t>::max() - frames)
            return GraphEventResult::OutOfRange;
        block_start_ = sample_start;
        block_frames_ = frames;
        sample_offset_ = 0;
        trace_size_ = 0;
        active_ = true;
        return GraphEventResult::Accepted;
    }

    GraphEventResult begin_sample(std::size_t offset) noexcept {
        if (!active_ || offset != sample_offset_ || offset >= block_frames_)
            return GraphEventResult::InvalidPhase;
        current_sample_ = block_start_ + offset;
        attempts_ = accepted_ = 0;
        attempt_closed_ = false;
        sample_begin_ = trace_size_;
        phase_ = 1;
        return GraphEventResult::Accepted;
    }

    GraphEventResult emit(std::uint64_t epoch, std::uint32_t producer, std::uint16_t payload,
                          std::uint64_t target_sample, std::uint8_t source_port = 0,
                          std::uint8_t kind = 1) noexcept {
        if (attempt_closed_)
            return GraphEventResult::AttemptLimit;
        if (phase_ != 1) {
            ++counters_.diagnostic_refusals;
            return GraphEventResult::InvalidPhase;
        }
        if (attempts_ == kMaxAttemptsPerSample) {
            attempt_closed_ = true;
            return GraphEventResult::AttemptLimit;
        }
        ++attempts_;
        counters_.attempts = attempts_;
        const auto sequence = sequence_++;
        // Expose the next producer-local sequence alongside the per-sample
        // attempt counters. Invalid and overflowed attempts consume sequence
        // identity, while the latched AttemptLimit refusal below does not.
        counters_.sequence = sequence_;
        auto refuse = [&](GraphEventResult result) {
            ++counters_.diagnostic_refusals;
            return result;
        };
        if (epoch != epoch_)
            return refuse(GraphEventResult::StaleEpoch);
        if (producer == 0 || source_port != 0 || kind != 1)
            return refuse(GraphEventResult::Unbound);
        if (target_sample < current_sample_)
            return refuse(GraphEventResult::Late);
        if (target_sample > current_sample_)
            return refuse(GraphEventResult::Future);
        if (accepted_ == MaxAcceptedPerSample) {
            if (counters_.overflow != std::numeric_limits<std::uint64_t>::max())
                ++counters_.overflow;
            return GraphEventResult::Overflow;
        }
        records_[trace_size_++] = {epoch_,  current_sample_, sequence, producer,
                                   payload, source_port,     kind};
        ++accepted_;
        counters_.accepted = accepted_;
        return GraphEventResult::Accepted;
    }

    GraphEventResult begin_consumer() noexcept {
        if (!active_ || phase_ != 1)
            return GraphEventResult::InvalidPhase;
        phase_ = 2;
        return GraphEventResult::Accepted;
    }
    std::span<const GraphAuthoredEvent> received() const noexcept {
        if (phase_ != 2)
            return {};
        return {records_.data() + sample_begin_, trace_size_ - sample_begin_};
    }
    GraphEventResult end_sample() noexcept {
        if (!active_ || phase_ != 2)
            return GraphEventResult::InvalidPhase;
        phase_ = 0;
        consumed_frontier_ = current_sample_ + 1;
        ++sample_offset_;
        return GraphEventResult::Accepted;
    }
    GraphEventResult end_block() noexcept {
        if (!active_ || phase_ != 0 || sample_offset_ != block_frames_)
            return GraphEventResult::InvalidPhase;
        if (cursor_ > std::numeric_limits<std::uint64_t>::max() - block_frames_) {
            // The current block has completed its consumer phase. Retire the
            // active state even when the monotonic cursor cannot advance, so
            // the owner can reset to a new epoch instead of remaining wedged.
            active_ = false;
            return GraphEventResult::OutOfRange;
        }
        cursor_ += block_frames_;
        counters_.cursor = cursor_;
        active_ = false;
        return GraphEventResult::Accepted;
    }

    const GraphEventCounters& counters() const noexcept {
        return counters_;
    }
    std::span<const GraphAuthoredEvent> trace() const noexcept {
        return {records_.data(), trace_size_};
    }
    std::uint64_t epoch() const noexcept {
        return epoch_;
    }
    bool active() const noexcept {
        return active_;
    }

  private:
    friend struct detail::GraphEventLaneTestAccess;

    void test_set_cursor_(std::uint64_t cursor) noexcept {
        cursor_ = cursor;
    }

    std::array<GraphAuthoredEvent, MaxFrames * MaxAcceptedPerSample> records_{};
    GraphEventCounters counters_{};
    std::uint64_t epoch_ = 0;
    std::uint64_t cursor_ = 0;
    std::uint64_t sequence_ = 0;
    std::uint64_t block_start_ = 0;
    std::uint64_t current_sample_ = 0;
    std::uint64_t consumed_frontier_ = 0;
    std::size_t block_frames_ = 0;
    std::size_t sample_offset_ = 0;
    std::size_t sample_begin_ = 0;
    std::size_t trace_size_ = 0;
    std::uint8_t attempts_ = 0;
    std::uint8_t accepted_ = 0;
    std::uint8_t phase_ = 0;
    bool attempt_closed_ = false;
    bool active_ = false;
};

} // namespace pulp::host
