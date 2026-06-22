// Cross-format parity harness support.
//
// Reference-processor concept plus the event-case matrix the future harness
// will run identically across VST3 / AU / CLAP / LV2. This keeps fixture
// identity stable before per-format execution exists. There is intentionally NO
// format execution here yet.
#pragma once

#include <cstdint>
#include <string_view>

namespace pulp::native_components::test {

// The matrix of behaviours a single reference processor must exhibit identically
// across every format. The harness asserts each case; a format that genuinely
// cannot represent a case records `Unsupported` (an explicit skip), never a
// silent pass.
enum class EventCase : std::uint8_t {
    Effect,            // stereo in/out, no MIDI
    InstrumentMidi,    // MIDI in -> audio out
    Sidechain,         // primary + sidechain bus
    MultiBus,          // >1 input/output bus
    MidiOut,           // produces MIDI
    MpeUmp,            // MPE / UMP events
    Bypass,            // bypassed passthrough
    LatencyTailChange, // reports a latency/tail change
    ParamEventOverflow,// dense automation exceeding the 1024-cap queue
    NullVsEmptyQueue,  // NULL queue vs present-but-empty queue
    MalformedState,    // load_state given bad bytes
};

// Outcome of running one case against one format.
enum class CaseOutcome : std::uint8_t {
    Match,        // behaviour identical to the reference
    Mismatch,     // behaviour diverged (a parity bug)
    Unsupported,  // format cannot represent the case (explicit, recorded skip)
};

// Identifies a reference processor in the matrix.
struct ReferenceProcessorSpec {
    std::string_view id;
    std::string_view display_name;
    EventCase primary_case;
};

// The set of reference processors the full harness will eventually drive.
// Per-format runners are wired separately.
inline constexpr ReferenceProcessorSpec kReferenceProcessors[] = {
    {"ref.gain", "Reference Gain", EventCase::Effect},
    {"ref.synth", "Reference Synth", EventCase::InstrumentMidi},
    {"ref.sidechain", "Reference Sidechain", EventCase::Sidechain},
};

}  // namespace pulp::native_components::test
