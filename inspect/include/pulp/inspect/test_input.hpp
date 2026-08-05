#pragma once

#include <pulp/inspect/protocol.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace pulp::inspect {

enum class MidiTestInputKind : std::uint8_t {
    NoteOn,
    NoteOff,
};

struct MidiTestInput {
    MidiTestInputKind kind = MidiTestInputKind::NoteOn;
    std::uint8_t channel = 0;
    std::uint8_t note = 0;
    std::uint8_t velocity = 0;
};

struct StandaloneTransportTestInput {
    std::optional<bool> playing;
    std::optional<std::int64_t> position_samples;
    std::optional<double> tempo_bpm;
};

struct TestInputApplyResult {
    bool applied = false;
    std::string error_code;
    std::string error_message;

    static TestInputApplyResult success() {
        return {true, {}, {}};
    }
    static TestInputApplyResult failure(std::string code, std::string message) {
        return {false, std::move(code), std::move(message)};
    }
};

enum class TestInputReleaseReason : std::uint8_t {
    ControllerReleased,
    ControllerExpired,
    ClientDisconnected,
    SessionTeardown,
};

class InspectorTestInputSource {
  public:
    virtual ~InspectorTestInputSource() = default;

    virtual TestInputApplyResult inject_midi(const MidiTestInput& input) = 0;
    virtual TestInputApplyResult set_transport(const StandaloneTransportTestInput& input) = 0;
    /// End controller-scoped input without bypassing the normal host path.
    /// Cleanup must remain ordered after accepted input. In particular, an
    /// accepted note-on must reach at least one subsequent process quantum
    /// before cleanup-generated note-off or all-notes-off events take effect.
    virtual void release_test_input(TestInputReleaseReason reason) noexcept = 0;
};

class TestInputDomain {
  public:
    void set_source(InspectorTestInputSource* source) {
        source_ = source;
    }
    InspectorTestInputSource* source() const {
        return source_;
    }

    InspectorMessage handle(const InspectorMessage& request) const;
    void release(TestInputReleaseReason reason) const noexcept;

  private:
    InspectorTestInputSource* source_ = nullptr;
};

} // namespace pulp::inspect
