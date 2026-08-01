#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <type_traits>

#include <pulp/timeline_editor/scripted_ui_host.hpp>
#include <pulp/timeline_editor/sequencer_ui_host.hpp>

using namespace pulp;
using namespace pulp::timeline_editor;

namespace {

/// Stand-in for the editor's intent vocabulary, which the host never inspects.
/// A host only requires an intent to be copyable, so a test can name its own
/// and prove the seam holds without the editor's real intent type.
struct StandInIntent {
    timeline::ItemId subject{};
    timebase::TickDuration delta{};
    std::int32_t pitch_delta = 0;

    constexpr bool operator==(const StandInIntent&) const = default;
};

using TestHost = ScriptedUiHost<StandInIntent>;

/// Mirror of AuditionRequest's declared fields, in declaration order. It is the
/// field inventory the layering rule was checked against below: if the request
/// grows, loses, or re-types a member, the two sizes diverge and the layering
/// case reddens, rather than the new member quietly reaching a type the editor
/// is not allowed to name.
struct AuditionRequestFieldInventory {
    timeline::ItemId track{};
    timeline::ItemId item{};
    std::uint8_t pitch = 0;
    std::uint16_t velocity = 0;
    std::uint8_t channel = 0;
    timebase::TickDuration duration{};
};

constexpr std::int64_t quarters(std::int64_t count) {
    return count * timebase::kTicksPerQuarter;
}

} // namespace

TEST_CASE("Ui host snapshot is value copied and stable across program swap",
          "[timeline-editor][ui-host]") {
    auto host = std::make_unique<TestHost>();

    ScriptedProgram compiled;
    compiled.position = timebase::TickPosition{quarters(4)};
    compiled.loop = timebase::LoopRegion{true, timebase::TickPosition{0},
                                         timebase::TickPosition{quarters(8)}};
    compiled.state = UiTransportState::Playing;
    compiled.tempo_bpm = 128.0;
    compiled.continuity_epoch = 3;
    host->set_program(compiled);

    const UiPlayhead before = host->playhead();
    REQUIRE(before.position.value == quarters(4));
    REQUIRE(before.tempo_bpm == 128.0);
    REQUIRE(before.state == UiTransportState::Playing);
    REQUIRE(before.continuity_epoch == 3);
    REQUIRE(before.loop.enabled);
    REQUIRE(before.loop.end.value == quarters(8));
    REQUIRE(before.moving());

    // The engine adopts a different compiled program. The previous program's
    // storage is released, so a reading that had borrowed from it would now be
    // reading freed bytes rather than merely disagreeing.
    ScriptedProgram recompiled;
    recompiled.position = timebase::TickPosition{quarters(1)};
    recompiled.loop = timebase::LoopRegion{false, timebase::TickPosition{quarters(2)},
                                           timebase::TickPosition{quarters(3)}};
    recompiled.state = UiTransportState::Stopped;
    recompiled.tempo_bpm = 90.0;
    recompiled.continuity_epoch = 9;
    host->swap_program(recompiled);

    // Every field of the pre-swap reading still describes the world it was
    // taken from.
    REQUIRE(before.position.value == quarters(4));
    REQUIRE(before.tempo_bpm == 128.0);
    REQUIRE(before.state == UiTransportState::Playing);
    REQUIRE(before.loop.enabled);
    REQUIRE(before.loop.start.value == 0);
    REQUIRE(before.loop.end.value == quarters(8));
    REQUIRE(before.continuity_epoch == 3);

    // A view can tell the stale reading from the live one without having held
    // anything the swap invalidated.
    const UiPlayhead after = host->playhead();
    REQUIRE(after.program_generation != before.program_generation);
    REQUIRE(after.sequence > before.sequence);
    REQUIRE(after.position.value == quarters(1));
    REQUIRE(after.tempo_bpm == 90.0);
    REQUIRE(after.continuity_epoch == 9);
    REQUIRE(after.state == UiTransportState::Stopped);
    REQUIRE_FALSE(after.moving());

    // Copying the retained reading needs nothing the host still owns, and the
    // reading outlives the host entirely.
    const UiPlayhead retained = before;
    host.reset();
    REQUIRE(retained == before);
    REQUIRE(retained.position.value == quarters(4));
    REQUIRE(retained.tempo_bpm == 128.0);
    REQUIRE(retained.loop.end.value == quarters(8));
    REQUIRE(retained.continuity_epoch == 3);
}

TEST_CASE("Ui playhead reports the continuity break a loop wrap makes",
          "[timeline-editor][ui-host]") {
    TestHost host;

    ScriptedProgram running;
    running.loop = timebase::LoopRegion{true, timebase::TickPosition{0},
                                        timebase::TickPosition{quarters(8)}};
    running.state = UiTransportState::Playing;
    running.position = timebase::TickPosition{quarters(6)};
    running.continuity_epoch = 4;
    host.set_program(running);
    const UiPlayhead first = host.playhead();

    // An ordinary advance inside the loop. Continuity holds, which is what
    // permits a view to interpolate between these two positions at all.
    ScriptedProgram advanced = running;
    advanced.position = timebase::TickPosition{quarters(7)};
    host.set_program(advanced);
    const UiPlayhead second = host.playhead();

    REQUIRE(second.sequence > first.sequence);
    REQUIRE(second.position.value > first.position.value);
    REQUIRE(second.continuity_epoch == first.continuity_epoch);

    // The loop wraps. The position jumps back by nearly the whole loop, so a
    // view that interpolated across this pair would draw the playhead sliding
    // backwards through ground it never covered.
    ScriptedProgram wrapped = running;
    wrapped.position = timebase::TickPosition{quarters(1)};
    wrapped.continuity_epoch = 5;
    host.set_program(wrapped);
    const UiPlayhead third = host.playhead();

    REQUIRE(third.position.value < second.position.value);
    REQUIRE(third.continuity_epoch != second.continuity_epoch);

    // Nothing else in the reading reports it, which is why the field exists.
    // Looping does not recompile, so the program generation is unchanged;
    // publishing is per block, so the sequence advances for an ordinary
    // advance exactly as it does for a wrap; and the transport was playing on
    // both sides of the wrap. Only continuity_epoch separates the two cases.
    REQUIRE(third.program_generation == second.program_generation);
    REQUIRE(third.sequence > second.sequence);
    REQUIRE(second.state == UiTransportState::Playing);
    REQUIRE(third.state == UiTransportState::Playing);
    REQUIRE(third.loop == second.loop);
    REQUIRE(third.tempo_bpm == second.tempo_bpm);
}

TEST_CASE("Audition request carries no playback types",
          "[timeline-editor][layering]") {
    AuditionRequest request;

    // Each member named explicitly. A member that grew into an engine type — a
    // transport snapshot, a compiled-map pointer, a renderer or voice handle —
    // fails here, at the interface, not at some later link step.
    REQUIRE(std::is_same_v<decltype(request.track), timeline::ItemId>);
    REQUIRE(std::is_same_v<decltype(request.item), timeline::ItemId>);
    REQUIRE(std::is_same_v<decltype(request.pitch), std::uint8_t>);
    REQUIRE(std::is_same_v<decltype(request.velocity), std::uint16_t>);
    REQUIRE(std::is_same_v<decltype(request.channel), std::uint8_t>);
    REQUIRE(std::is_same_v<decltype(request.duration), timebase::TickDuration>);

    // The whitelist above only constrains the members that exist, so pin the
    // inventory too: a member added to the request reddens this.
    REQUIRE(sizeof(AuditionRequest) == sizeof(AuditionRequestFieldInventory));
    REQUIRE(alignof(AuditionRequest) == alignof(AuditionRequestFieldInventory));

    // The closure of those types is the document model and the timebase, and
    // none of them borrows storage, so a request can be queued to an audio
    // thread and outlive the gesture that produced it.
    REQUIRE(std::is_trivially_copyable_v<AuditionRequest>);
    REQUIRE(std::is_trivially_copyable_v<timeline::ItemId>);
    REQUIRE(std::is_trivially_copyable_v<timebase::TickDuration>);

    // The reply is a value too: a host that cannot sound anything is described
    // by the same type as one that can.
    REQUIRE(std::is_trivially_copyable_v<AuditionResult>);
    REQUIRE(std::is_trivially_copyable_v<AuditionHandle>);

    // A request survives the request object being reused for the next gesture.
    request.track = timeline::ItemId{7};
    request.pitch = 64;
    request.velocity = 0xfe00;
    request.duration = timebase::TickDuration{quarters(1)};
    const AuditionRequest queued = request;
    request = AuditionRequest{};
    REQUIRE(queued.track.value == 7);
    REQUIRE(queued.pitch == 64);
    REQUIRE(queued.velocity == 0xfe00);
    REQUIRE(queued.duration.value == quarters(1));
}

TEST_CASE("Ui host without audio reports audition unsupported and stays usable",
          "[timeline-editor][ui-host]") {
    TestHost host;
    host.set_audition_status(AuditionStatus::Unsupported);

    AuditionRequest request;
    request.pitch = 72;
    const AuditionResult result = host.begin_audition(request);

    REQUIRE(result.status == AuditionStatus::Unsupported);
    REQUIRE_FALSE(result.handle.valid());
    // The request still reached the host, so a view is not required to know in
    // advance whether its host has audio.
    REQUIRE(host.auditions().size() == 1);
    REQUIRE(host.auditions().front().request.pitch == 72);

    // Ending a handle that was never issued is a no-op, so a view may end
    // unconditionally on mouse-up.
    host.end_audition(result.handle);
    REQUIRE(host.ended_auditions().size() == 1);
    REQUIRE_FALSE(host.ended_auditions().front().valid());
}

TEST_CASE("Sustained audition is issued a handle and a one shot is not",
          "[timeline-editor][ui-host]") {
    TestHost host;

    AuditionRequest sustained;
    sustained.pitch = 60;
    const AuditionResult held = host.begin_audition(sustained);
    REQUIRE(held.status == AuditionStatus::Started);
    REQUIRE(held.handle.valid());

    AuditionRequest one_shot;
    one_shot.pitch = 61;
    one_shot.duration = timebase::TickDuration{quarters(1)};
    const AuditionResult fired = host.begin_audition(one_shot);
    REQUIRE(fired.status == AuditionStatus::Started);
    // A one-shot ends itself, so there is nothing for the view to hold.
    REQUIRE_FALSE(fired.handle.valid());

    host.end_audition(held.handle);
    REQUIRE(host.ended_auditions().size() == 1);
    REQUIRE(host.ended_auditions().front() == held.handle);
}

TEST_CASE("Emitted intents cross the host by value and report their outcome",
          "[timeline-editor][ui-host]") {
    TestHost host;

    StandInIntent intent;
    intent.subject = timeline::ItemId{42};
    intent.delta = timebase::TickDuration{quarters(2)};
    intent.pitch_delta = -3;

    const IntentResult accepted = host.submit_intent(intent);
    REQUIRE(accepted.status == IntentStatus::Accepted);
    REQUIRE(accepted.sequence == 1);

    // The host kept its own copy: mutating the emitter's intent afterwards does
    // not reach what was submitted.
    intent.pitch_delta = 99;
    REQUIRE(host.intents().size() == 1);
    REQUIRE(host.intents().front().pitch_delta == -3);
    REQUIRE(host.intents().front().subject.value == 42);

    // A host that is not accepting right now is an ordinary outcome, not an
    // error, and carries no identity to correlate.
    host.set_intent_status(IntentStatus::Deferred);
    const IntentResult deferred = host.submit_intent(intent);
    REQUIRE(deferred.status == IntentStatus::Deferred);
    REQUIRE(deferred.sequence == 0);
    REQUIRE(host.intents().size() == 2);
}

TEST_CASE("Ui host is usable through the interface it declares",
          "[timeline-editor][ui-host]") {
    TestHost concrete;
    ScriptedProgram program;
    program.position = timebase::TickPosition{quarters(3)};
    program.state = UiTransportState::Scrubbing;
    concrete.set_program(program);

    // A view holds the interface, never the implementation.
    SequencerUiHostT<StandInIntent>& host = concrete;
    const UiPlayhead reading = host.playhead();
    REQUIRE(reading.position.value == quarters(3));
    // A scrub moves the playhead while the musical transport is stopped, so a
    // view asking whether to repaint the ruler needs no scrub-specific branch.
    REQUIRE(reading.state == UiTransportState::Scrubbing);
    REQUIRE(reading.moving());

    REQUIRE(host.submit_intent(StandInIntent{}).status == IntentStatus::Accepted);
    REQUIRE(host.begin_audition(AuditionRequest{}).status == AuditionStatus::Started);
}
