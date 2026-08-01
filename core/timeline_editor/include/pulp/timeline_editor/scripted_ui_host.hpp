#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <pulp/timeline_editor/sequencer_ui_host.hpp>

/// ScriptedUiHost is a SequencerUiHost whose playhead is written by the caller
/// instead of an engine, and which keeps what a view emitted so a test can read
/// it back. It is the seam that makes an editor view testable with no audio, no
/// transport, and no mocking framework — the same job PumpResult does for a
/// hostless StepGridView, done from the other side of the interface.
///
/// It is not only a test fixture. A host that has no audio yet is a legitimate
/// deployment: an editor embedded in a tool that only writes files gets a
/// stopped playhead and Unsupported auditions, and stays fully usable.
///
/// The scripted program is deliberately heap-owned and genuinely replaced by
/// swap_program(). A UiPlayhead is required to be a self-contained value, and
/// that requirement is only worth stating if something can catch a violation:
/// after a swap the previous program's storage is freed, so a reading that
/// borrowed from it reads freed memory rather than quietly disagreeing.
namespace pulp::timeline_editor {

/// What the caller scripts. Held by value inside the host's current program.
struct ScriptedProgram {
    timebase::TickPosition position{};
    UiLoopRegion loop{};
    UiTransportState state = UiTransportState::Stopped;
    double tempo_bpm = 120.0;
};

template <class Intent>
class ScriptedUiHost final : public SequencerUiHostT<Intent> {
  public:
    /// One audition the host was asked for, with the outcome it gave back.
    struct AuditionRecord {
        AuditionRequest request{};
        AuditionResult result{};
    };

    ScriptedUiHost() : program_(std::make_unique<ScriptedProgram>()) {}

    // ── Scripting (caller side) ──────────────────────────────────────────────

    /// Update the current program in place. Advances the publish sequence, not
    /// the program generation: this is the engine moving, not swapping.
    void set_program(const ScriptedProgram& program) {
        *program_ = program;
        ++sequence_;
    }

    /// Adopt a different compiled program, as an engine does when a recompile
    /// lands. The previous program's storage is released before the new one is
    /// allocated, so anything that borrowed from it is now reading freed bytes.
    void swap_program(const ScriptedProgram& program) {
        program_.reset();
        program_ = std::make_unique<ScriptedProgram>(program);
        ++generation_;
        ++sequence_;
    }

    std::uint64_t program_generation() const { return generation_; }

    /// What the host does with the next audition request. Defaults to starting
    /// it; set Unsupported to exercise a view against a host with no audio.
    void set_audition_status(AuditionStatus status) { audition_status_ = status; }

    /// What the host does with the next intent.
    void set_intent_status(IntentStatus status) { intent_status_ = status; }

    // ── What the view emitted (test side) ────────────────────────────────────

    const std::vector<AuditionRecord>& auditions() const { return auditions_; }
    const std::vector<AuditionHandle>& ended_auditions() const { return ended_; }
    const std::vector<Intent>& intents() const { return intents_; }

    void clear_emissions() {
        auditions_.clear();
        ended_.clear();
        intents_.clear();
    }

    // ── SequencerUiHost ──────────────────────────────────────────────────────

    UiPlayhead playhead() const noexcept override {
        const ScriptedProgram& current = *program_;
        UiPlayhead reading;
        reading.program_generation = generation_;
        reading.sequence = sequence_;
        reading.position = current.position;
        reading.loop = current.loop;
        reading.state = current.state;
        reading.tempo_bpm = current.tempo_bpm;
        return reading;
    }

    AuditionResult begin_audition(const AuditionRequest& request) noexcept override {
        // The recording vectors allocate, which is why this host belongs on the
        // UI side of a real implementation's queue and never on an audio thread.
        AuditionResult result;
        result.status = audition_status_;
        if (audition_status_ == AuditionStatus::Started && request.duration.value == 0)
            result.handle = AuditionHandle{++next_handle_};
        auditions_.push_back(AuditionRecord{request, result});
        return result;
    }

    void end_audition(AuditionHandle handle) noexcept override {
        ended_.push_back(handle);
    }

    IntentResult submit_intent(const Intent& intent) noexcept override {
        intents_.push_back(intent);
        IntentResult result;
        result.status = intent_status_;
        if (intent_status_ == IntentStatus::Accepted)
            result.sequence = ++next_intent_sequence_;
        return result;
    }

  private:
    std::unique_ptr<ScriptedProgram> program_;
    std::uint64_t generation_ = 1;
    std::uint64_t sequence_ = 1;
    std::uint64_t next_handle_ = 0;
    std::uint64_t next_intent_sequence_ = 0;
    AuditionStatus audition_status_ = AuditionStatus::Started;
    IntentStatus intent_status_ = IntentStatus::Accepted;

    std::vector<AuditionRecord> auditions_;
    std::vector<AuditionHandle> ended_;
    std::vector<Intent> intents_;
};

} // namespace pulp::timeline_editor
