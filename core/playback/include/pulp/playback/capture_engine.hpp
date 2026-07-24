#pragma once

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/rt_safety_contract.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/playback/transport.hpp>
#include <pulp/runtime/spsc_queue.hpp>
#include <pulp/timeline/item_id.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace pulp::playback {

struct CaptureTrackConfig {
    timeline::ItemId track_id;
    timeline::ItemId take_lane_id;
    std::uint32_t input_channel = 0;
    std::uint32_t output_channel = 0;
    std::uint32_t channel_count = 0;
    bool armed = false;
    bool monitor = false;
    bool capture_midi = false;
};

struct CaptureEngineConfig {
    static constexpr std::uint64_t kDefaultMaximumPreallocatedBytes = 512ull * 1024ull * 1024ull;

    timebase::RationalRate sample_rate;
    std::uint32_t maximum_block_size = 0;
    std::uint64_t maximum_take_frames = 0;
    std::uint32_t take_slots_per_track = 0;
    std::uint32_t midi_events_per_take = 0;
    std::uint64_t maximum_preallocated_bytes = kDefaultMaximumPreallocatedBytes;
    std::vector<CaptureTrackConfig> tracks;
};

struct CaptureSession {
    timebase::SamplePosition count_in_start;
    timebase::SamplePosition punch_in;
    bool has_punch_out = false;
    timebase::SamplePosition punch_out;
    bool loop_enabled = false;
    timebase::SamplePosition loop_start;
    timebase::SamplePosition loop_end;
    bool metronome_enabled = false;
    std::uint32_t metronome_output_channel = 0;
    timebase::TickDuration metronome_interval{timebase::kTicksPerQuarter};
    float metronome_level = 0.25f;
};

struct CaptureTakeHandle {
    std::uint32_t slot = 0;
    std::uint32_t generation = 0;
    constexpr auto operator<=>(const CaptureTakeHandle&) const = default;
};

struct CapturedMidiEvent {
    midi::MidiEvent event;
    std::uint64_t take_frame = 0;
};

enum class CaptureCommandType : std::uint8_t {
    Start,
    Stop,
    Cancel,
    ReleaseTake,
};

struct CaptureCommand {
    CaptureCommandType type = CaptureCommandType::Start;
    std::uint64_t sequence = 0;
    CaptureSession session;
    CaptureTakeHandle take;
};

enum class CaptureEventType : std::uint8_t {
    Started,
    TakeCompleted,
    Stopped,
    Cancelled,
    CommandRejected,
    NoFreeTakeSlot,
    TakeCapacityExceeded,
    MidiCapacityExceeded,
    TakeCancelled,
};

struct CaptureEvent {
    CaptureEventType type = CaptureEventType::Started;
    std::uint64_t sequence = 0;
    timeline::ItemId track_id;
    timeline::ItemId take_lane_id;
    CaptureTakeHandle take;
    timebase::SamplePosition placement_start;
    std::uint64_t frame_count = 0;
    std::uint32_t channel_count = 0;
    std::uint32_t midi_event_count = 0;
};

enum class CaptureProcessResult : std::uint8_t {
    Ok,
    NotPrepared,
    InvalidBuffers,
    InvalidTransport,
};

struct CaptureEngineStats {
    std::uint64_t completed_takes = 0;
    std::uint64_t dropped_commands = 0;
    std::uint64_t dropped_events = 0;
    std::uint64_t capacity_failures = 0;
};

/// Fixed-capacity input recorder. prepare() owns every allocation; process()
/// only mutates preallocated take slots and lock-free command/event queues.
/// Completed slots remain immutable until ReleaseTake reaches process().
class CaptureEngine {
  public:
    static constexpr std::size_t kMaximumTracks = 32;
    static constexpr std::size_t kMaximumTakeSlotsPerTrack = 64;
    static constexpr std::size_t kCommandQueueCapacity = 128;
    static constexpr std::size_t kEventQueueCapacity = 256;
    static constexpr audio::RtSafetyClass process_rt_safety_class =
        audio::RtSafetyClass::AudioCallbackSafeAfterPrepare;

    bool prepare(const CaptureEngineConfig& config);
    void release() noexcept;

    bool enqueue_command(const CaptureCommand& command) noexcept;
    bool pop_event(CaptureEvent& event) noexcept;

    CaptureProcessResult process(const audio::BufferView<const float>& input,
                                 audio::BufferView<float>& monitor_output,
                                 const midi::MidiBuffer& midi_input,
                                 const TransportSnapshot& transport) noexcept;

    bool copy_audio(CaptureTakeHandle take, audio::BufferView<float> destination) const noexcept;
    bool copy_midi(CaptureTakeHandle take, std::span<CapturedMidiEvent> destination,
                   std::size_t& copied) const noexcept;

    float input_peak(std::size_t track_index) const noexcept;
    CaptureEngineStats stats() const noexcept;
    bool recording() const noexcept;

  private:
    enum class SlotState : std::uint8_t { Free, Recording, Completed };

    struct TakeSlot {
        SlotState state = SlotState::Free;
        std::uint32_t generation = 0;
        std::uint32_t track_index = 0;
        timebase::SamplePosition placement_start;
        std::uint64_t frame_count = 0;
        std::uint32_t midi_event_count = 0;
        bool audio_capacity_reported = false;
        bool midi_capacity_reported = false;
        std::vector<float> audio;
        std::vector<CapturedMidiEvent> midi;
    };

    struct TrackRuntime {
        std::int32_t active_slot = -1;
        bool capture_disabled = false;
    };

    static bool valid_config(const CaptureEngineConfig& config) noexcept;
    static bool valid_session(const CaptureSession& session) noexcept;
    static timebase::SamplePosition add_frames(timebase::SamplePosition start,
                                               std::uint32_t frames) noexcept;

    void apply_commands() noexcept;
    void apply_command(const CaptureCommand& command) noexcept;
    void start_session(const CaptureCommand& command) noexcept;
    void stop_session(const CaptureCommand& command, bool cancelled) noexcept;
    void release_take(CaptureTakeHandle take) noexcept;
    void push_event(const CaptureEvent& event) noexcept;
    void push_session_event(CaptureEventType type, std::uint64_t sequence) noexcept;
    void monitor_inputs(const audio::BufferView<const float>& input,
                        audio::BufferView<float>& output, std::uint32_t frames) noexcept;
    void render_metronome(audio::BufferView<float>& output,
                          const TransportSnapshot& transport) noexcept;
    void process_range(const audio::BufferView<const float>& input,
                       const midi::MidiBuffer& midi_input, const TransportRange& range) noexcept;
    void handle_range_boundary(const TransportRange& range) noexcept;
    void finalize_active_takes() noexcept;
    void cancel_active_takes() noexcept;
    TakeSlot* begin_take(std::size_t track_index,
                         timebase::SamplePosition placement_start) noexcept;
    void append_audio(TakeSlot& slot, const CaptureTrackConfig& track,
                      const audio::BufferView<const float>& input, std::uint32_t source_offset,
                      std::uint32_t frames) noexcept;
    void append_midi(TakeSlot& slot, const midi::MidiBuffer& input, std::uint32_t source_offset,
                     std::uint32_t frames) noexcept;
    bool valid_handle(CaptureTakeHandle take) const noexcept;

    CaptureEngineConfig config_;
    std::vector<TrackRuntime> track_runtime_;
    std::vector<TakeSlot> slots_;
    std::array<std::atomic<float>, kMaximumTracks> input_peaks_{};
    runtime::SpscQueue<CaptureCommand, kCommandQueueCapacity> commands_;
    runtime::SpscQueue<CaptureEvent, kEventQueueCapacity> events_;
    CaptureSession session_;
    std::uint64_t active_sequence_ = 0;
    timebase::SamplePosition expected_timeline_sample_;
    bool has_expected_timeline_sample_ = false;
    bool recording_ = false;
    bool prepared_ = false;
    std::atomic<bool> recording_snapshot_{false};
    std::atomic<std::uint64_t> completed_takes_{0};
    std::atomic<std::uint64_t> dropped_commands_{0};
    std::atomic<std::uint64_t> dropped_events_{0};
    std::atomic<std::uint64_t> capacity_failures_{0};
};

static_assert(std::atomic<float>::is_always_lock_free);

} // namespace pulp::playback
