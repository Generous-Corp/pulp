#pragma once

#include <pulp/audio/rt_safety_contract.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/timebase/rational_time.hpp>

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>

namespace pulp::playback {

struct TransportSnapshot;

enum class MtcFrameRate : std::uint8_t {
    Fps24,
    Fps25,
    Fps2997Drop,
    Fps30,
};

struct MtcTimecode {
    std::uint8_t hours = 0;
    std::uint8_t minutes = 0;
    std::uint8_t seconds = 0;
    std::uint8_t frames = 0;
    MtcFrameRate frame_rate = MtcFrameRate::Fps30;
    constexpr auto operator<=>(const MtcTimecode&) const = default;
};

bool valid_mtc_timecode(const MtcTimecode& timecode) noexcept;

/// Convert between an MTC wall-clock label and a non-negative timeline sample.
/// The 29.97 mode uses SMPTE drop-frame numbering; no video frames are removed.
timebase::SamplePosition mtc_timecode_to_samples(const MtcTimecode& timecode,
                                                 timebase::RationalRate sample_rate) noexcept;
MtcTimecode samples_to_mtc_timecode(timebase::SamplePosition position,
                                    timebase::RationalRate sample_rate,
                                    MtcFrameRate frame_rate) noexcept;

enum class MtcChaseCode : std::uint8_t {
    Ignored,
    Incomplete,
    Locked,
    Discontinuity,
    Invalid,
};

enum class MtcDirection : std::int8_t {
    Reverse = -1,
    Unknown = 0,
    Forward = 1,
};

struct MtcChaseUpdate {
    MtcChaseCode code = MtcChaseCode::Ignored;
    MtcDirection direction = MtcDirection::Unknown;
    MtcTimecode timecode{};
    timebase::SamplePosition position{};
};

/// Allocation-free MTC decoder. Quarter-frame input locks only after one
/// coherent eight-message cycle. Universal-realtime full-frame messages lock
/// immediately and re-anchor the next quarter-frame cycle.
class MtcChaser {
  public:
    MtcChaseUpdate consume(const midi::MidiEvent& event,
                           timebase::RationalRate sample_rate) noexcept;
    MtcChaseUpdate consume_sysex(std::span<const std::uint8_t> message,
                                 timebase::RationalRate sample_rate) noexcept;
    void reset() noexcept;

  private:
    MtcChaseUpdate complete(timebase::RationalRate sample_rate, MtcDirection direction) noexcept;

    std::array<std::uint8_t, 8> pieces_{};
    std::uint8_t received_mask_ = 0;
    std::uint8_t last_piece_ = 0;
    MtcDirection cycle_direction_ = MtcDirection::Unknown;
    bool have_last_piece_ = false;
};

struct ExternalSyncOutputConfig {
    bool emit_midi_clock = true;
    bool emit_mtc = true;
    MtcFrameRate mtc_frame_rate = MtcFrameRate::Fps30;
    std::uint8_t mtc_device_id = 0x7f;
    std::uint32_t max_messages_per_block = 4'096;
};

enum class ExternalSyncOutputCode : std::uint8_t {
    Complete,
    InvalidTransport,
    InvalidSampleRate,
    InvalidFrameRate,
    InvalidOutputLimit,
    OutputOverflow,
};

struct ExternalSyncOutputResult {
    ExternalSyncOutputCode code = ExternalSyncOutputCode::Complete;
    std::uint32_t short_messages = 0;
    std::uint32_t sysex_messages = 0;
};

/// Stateless block projection from the musical transport to MIDI sync.
/// The caller owns and pre-reserves the output buffer. process() does not clear
/// it, so sync can be combined with note/controller output before one stable
/// sort at the adapter boundary.
class ExternalSyncOutput {
  public:
    static constexpr audio::RtSafetyClass process_rt_safety_class =
        audio::RtSafetyClass::AudioCallbackSafeAfterPrepare;

    explicit ExternalSyncOutput(ExternalSyncOutputConfig config = {}) noexcept : config_(config) {}

    ExternalSyncOutputResult process(const TransportSnapshot& transport,
                                     midi::MidiBuffer& output) const noexcept;

  private:
    ExternalSyncOutputConfig config_{};
};

} // namespace pulp::playback
