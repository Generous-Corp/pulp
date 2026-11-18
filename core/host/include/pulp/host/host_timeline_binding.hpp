#pragma once

/// One audio-owner binding from the host block timeline to the event families
/// a graph-owning Processor or host feeds: MIDI/UMP (with the MPE sidecar the
/// graph derives from it) and graph-authored trigger events in a prepared
/// trigger `SignalGraph`, voice intent in a `VoiceBankBinding`, and
/// sample-accurate parameter records the binding stages itself.
///
/// The families keep their own epochs, capacities and refusal rules. The
/// binding owns one `HostBlockTimeline` and is the only place that maps it
/// onto them:
///
///  * Timeline coordinates. Callers publish every record against the window
///    `begin_block()` returned: the timeline epoch and an epoch-local absolute
///    sample. A record of an older timeline epoch is refused with
///    `StaleEpoch`, one before the window with `Late`.
///  * Graph coordinates. The graph's trigger clock counts rendered samples,
///    so it cannot skip forward with a host gap. Each block the binding reads
///    the graph's current epoch and cursor and maps the window start onto the
///    cursor. A graph record therefore must fall inside the current block; a
///    future one is refused with `OutOfRange` rather than staged, because a
///    later host gap would retime it.
///  * Voice and parameter coordinates. These stay in the timeline sample
///    domain. Future records are staged; a host gap evicts staged records it
///    skipped, never retiming them.
///
/// Reset transaction. The first block, every timeline `NewEpoch` (a seek,
/// loop, backwards clock or clock-source change) and every block after
/// `request_reset()` resets all bound families at once: the graph's host
/// ingress, MPE tracker, projection and trigger state (`try_reset_events()`),
/// the voice bank's notes, tracker, pedals and staged records, and the staged
/// parameter records. Each family moves to a fresh epoch, so every record and
/// deferred voice token published before the reset is refused afterwards.
/// The transaction is all or nothing: the voice epoch is reserved first, the
/// graph reset (the only step that can still fail) runs second, and only then
/// are the infallible voice, parameter and timeline commits applied. A refusal
/// leaves every family, the staged parameters and the timeline exactly as they
/// were, so the same block can be retried.
///
/// Opening the block after the transaction can still be refused (for example
/// when another thread holds the graph's diagnostic gate, or when the block is
/// longer than the frame limit a bound family was prepared for). Such a block is
/// consumed on the timeline as a skipped block: the caller renders silence
/// and does not call `render()`. The next block re-maps the graph cursor, so
/// the skip cannot misplace later records.
///
/// A reset of the graph made elsewhere (a plan install or a direct
/// `try_reset_events()`) is not a binding reset. The binding re-maps onto the
/// new graph epoch at the next block and counts it in `graph_remaps`; the
/// other families are left alone.
///
/// Every audio-owner method is noexcept and neither allocates nor locks.
/// Construction is a control-thread step while audio is stopped.

#include <pulp/audio/buffer.hpp>
#include <pulp/format/process_context.hpp>
#include <pulp/host/graph_event_contract.hpp>
#include <pulp/host/host_event_timestamps.hpp>
#include <pulp/host/trigger_graph.hpp>
#include <pulp/host/voice_bank_binding.hpp>
#include <pulp/state/parameter_event_queue.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace pulp::host {

class SignalGraph;

/// One sample-accurate parameter change in timeline coordinates.
struct HostTimelineParameterRecord {
    std::uint64_t epoch = 0;
    std::int64_t absolute_sample = 0;
    std::uint64_t producer_sequence = 0;
    std::uint32_t producer = 0;
    state::ParamID param_id = 0;
    float value = 0.0f;
};

/// Where the current block sits in each family's own coordinates.
struct HostTimelineBlock {
    HostBlockWindow timeline{};
    HostBlockBoundary boundary = HostBlockBoundary::First;
    /// True when this block opened a reset transaction.
    bool reset = false;
    /// Graph trigger epoch and cursor at the block start, when a graph is bound.
    std::uint64_t graph_epoch = 0;
    std::uint64_t graph_sample_start = 0;
    /// Voice bank epoch, when a voice bank is bound.
    std::uint64_t voice_epoch = 0;
};

struct HostTimelineBindingCounters {
    std::uint64_t blocks = 0;
    std::uint64_t refused_blocks = 0;
    std::uint64_t resets = 0;
    std::uint64_t refused_resets = 0;
    std::uint64_t graph_remaps = 0;
    std::uint64_t parameters_accepted = 0;
    std::uint64_t parameters_refused = 0;
    std::uint64_t parameters_overflow = 0;
    /// Staged parameter records a reset invalidated.
    std::uint64_t parameters_reset = 0;
    /// Staged parameter records a host gap skipped.
    std::uint64_t parameters_skipped = 0;
    /// Blocks the graph failed with `ProcessingFailure`. Their graph records
    /// are dropped rather than delivered late by the next block.
    std::uint64_t graph_failed_blocks = 0;
};

struct HostTimelineRenderResult {
    TriggerGraphResult graph = TriggerGraphResult::Accepted;
    VoiceBankBindingCode voice = VoiceBankBindingCode::Ok;
    explicit operator bool() const noexcept {
        return graph == TriggerGraphResult::Accepted && voice == VoiceBankBindingCode::Ok;
    }
};

class HostTimelineBinding final {
  public:
    static constexpr std::size_t kParameterCapacity = 256;

    /// Bind a prepared trigger graph, a voice bank, or both. Either may be
    /// null; parameter staging is always available. The bound objects must
    /// outlive the binding and must be driven only through it on the audio
    /// thread.
    HostTimelineBinding(SignalGraph* graph, VoiceBankBinding* voices) noexcept;

    HostTimelineBinding(const HostTimelineBinding&) = delete;
    HostTimelineBinding& operator=(const HostTimelineBinding&) = delete;

    /// Place the next host block, run the reset transaction when one is due,
    /// and open the block in every bound family. `Busy` means a block is
    /// already open. A refused reset changes nothing (see the header comment);
    /// a block refused after the reset committed is a skipped block.
    HostEventResult begin_block(const HostBlockClock& clock, HostTimelineBlock& out) noexcept;
    HostEventResult begin_block(const format::ProcessContext& context,
                                HostTimelineBlock& out) noexcept {
        return begin_block(clock_from_process_context(context), out);
    }

    /// Route one MIDI record (with its full UMP packet when `has_ump`) to the
    /// graph's host ingress. Timeline coordinates; current block only.
    HostEventResult publish_graph_event(const HostEventRecord& record) noexcept;
    /// Route one MIDI 1.0 record to the voice bank. Timeline coordinates.
    HostEventResult publish_voice_event(const HostEventRecord& record) noexcept;
    /// Stage one parameter record. Timeline coordinates. Refuses a stale
    /// epoch, a late or negative sample, producer zero, a duplicate
    /// (producer, sequence) pair, and a full staging area, without change.
    HostEventResult publish_parameter(const HostTimelineParameterRecord& record) noexcept;

    /// Move the staged parameter records of the current block into `out` as
    /// block-relative events, ordered by sample, then producer, then
    /// sequence. Refuses with `OutputCapacity`, leaving both untouched, when
    /// `out` cannot hold them all.
    HostEventResult take_parameters(state::ParameterEventQueue& out) noexcept;

    /// Render the open block: the graph through `process_checked()` and the
    /// voice bank into `voice_stereo`. Each pointer must be non-null exactly
    /// when that family is bound.
    ///
    /// A refused call leaves the block open so it can be retried: missing or
    /// short buffers, and a graph refusal made before the graph touched the
    /// block (for example `Busy` while another thread holds its diagnostic
    /// gate). Any other result closes the block. A graph `ProcessingFailure`
    /// closes it as a failed graph block: the binding drops the block's graph
    /// records (counted in `graph_failed_blocks`), zeroes `graph_output`, and
    /// still renders the voice bank. Routed execution consumes a partially
    /// executed failed block so the next block maps onto the coherent graph
    /// cursor; pre-projection failures leave that cursor unmoved.
    HostTimelineRenderResult render(audio::BufferView<float>* graph_output,
                                    const audio::BufferView<const float>* graph_input,
                                    audio::BufferView<float>* voice_stereo) noexcept;

    /// Make the next `begin_block()` reset every family even when the host
    /// block is continuous. The timeline opens a fresh epoch at sample zero.
    void request_reset() noexcept {
        reset_requested_ = true;
    }

    const HostTimelineBlock& current() const noexcept {
        return block_;
    }
    bool block_open() const noexcept {
        return open_;
    }
    const HostTimelineBindingCounters& counters() const noexcept {
        return counters_;
    }
    std::size_t staged_parameters() const noexcept {
        return parameter_count_;
    }

  private:
    HostEventResult check_window_(const HostEventRecord& record) const noexcept;
    void evict_parameters_before_(std::uint64_t sample) noexcept;
    void drop_failed_graph_block_(audio::BufferView<float>& output) noexcept;

    SignalGraph* graph_ = nullptr;
    VoiceBankBinding* voices_ = nullptr;
    HostBlockTimeline timeline_{};
    HostTimelineBlock block_{};
    HostTimelineBindingCounters counters_{};
    std::array<HostTimelineParameterRecord, kParameterCapacity> parameters_{};
    std::array<std::size_t, kParameterCapacity> parameter_order_{};
    std::size_t parameter_count_ = 0;
    std::uint64_t mapped_graph_epoch_ = 0;
    bool has_block_ = false;
    bool open_ = false;
    bool reset_requested_ = false;
    bool graph_discard_pending_ = false;
};

} // namespace pulp::host
