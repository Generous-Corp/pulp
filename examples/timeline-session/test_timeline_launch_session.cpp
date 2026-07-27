#include "timeline_launch_session.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <vector>

using namespace pulp;
using namespace pulp::examples::timeline_session;

namespace {

constexpr double kSampleRate = 48'000.0;
constexpr std::uint32_t kBlock = 256;
constexpr timeline::ItemId kSequenceId{3};
constexpr timeline::ItemId kSceneAId{20};
constexpr timeline::ItemId kSceneBId{21};

std::size_t clip_count(const timeline::Project& project, timeline::ItemId track_id) {
    const auto* sequence = project.find_sequence(kSequenceId);
    REQUIRE(sequence != nullptr);
    const auto* track = sequence->find_track(track_id);
    REQUIRE(track != nullptr);
    return track->clips().size();
}

} // namespace

TEST_CASE("launch session resolves a scene launch on its authored quantization boundary",
          "[timeline][examples][launch]") {
    TimelineLaunchSession session;
    REQUIRE(session.prepare(kSampleRate, kBlock));

    // Scene A is authored to launch on the bar with zero phase. Arming exactly
    // on a boundary is defined to fire there, so run the transport a little way
    // past bar 0 first: the interesting assertion is that a mid-bar arm waits
    // for the NEXT bar rather than sounding at once.
    const auto blocks_per_bar = static_cast<std::uint32_t>(2.0 * kSampleRate / kBlock);
    for (std::uint32_t block = 0; block < blocks_per_bar / 4; ++block)
        session.process(kBlock);

    REQUIRE(session.launch_scene(kSceneAId));
    REQUIRE(session.sounding_slot_count() == 0);

    // Everything up to just short of the bar line must stay silent.
    for (std::uint32_t block = 0; block + 2 < blocks_per_bar - blocks_per_bar / 4; ++block) {
        session.process(kBlock);
        REQUIRE(session.sounding_slot_count() == 0);
    }
    // And once the bar line is crossed, every row in the scene is sounding.
    for (std::uint32_t block = 0; block < 4; ++block)
        session.process(kBlock);
    REQUIRE(session.sounding_slot_count() == 3);
}

TEST_CASE("launch session fires a launch armed exactly on a grid boundary without delay",
          "[timeline][examples][launch]") {
    TimelineLaunchSession session;
    REQUIRE(session.prepare(kSampleRate, kBlock));

    // The boundary set is { phase + k*grid }; arming at the origin is already on
    // it, so the launch resolves in the first block rather than waiting a whole
    // bar. This is the companion to the mid-bar case above.
    REQUIRE(session.launch_scene(kSceneAId));
    session.process(kBlock);
    REQUIRE(session.sounding_slot_count() == 3);
}

TEST_CASE("launch session honours immediate quantization distinctly from a bar grid",
          "[timeline][examples][launch]") {
    TimelineLaunchSession session;
    REQUIRE(session.prepare(kSampleRate, kBlock));

    // Scene B's slots are authored launch_immediate(). One block is enough.
    REQUIRE(session.launch_scene(kSceneBId));
    session.process(kBlock);
    REQUIRE(session.sounding_slot_count() == 3);
}

TEST_CASE("capturing a launch performance flattens into ordinary clip-insert commands",
          "[timeline][examples][launch][flatten]") {
    TimelineLaunchSession session;
    REQUIRE(session.prepare(kSampleRate, kBlock));

    // A launcher row starts with a genuinely empty arrangement lane, so the
    // captured clip count is exact rather than "more than before".
    const auto track_id = timeline::ItemId{10};
    REQUIRE(clip_count(session.project(), track_id) == 0);

    REQUIRE(session.launch_scene(kSceneBId));
    session.process(kBlock);
    REQUIRE(session.sounding_slot_count() == 3);

    // Let the launched clips sound for a while, then stop them on the bar so the
    // captured spans have a real, non-zero duration.
    for (std::uint32_t block = 0; block < 200; ++block)
        session.process(kBlock);
    session.stop_all({timebase::TickDuration{4 * timebase::kTicksPerQuarter},
                      timebase::TickPosition{0}});
    for (std::uint32_t block = 0; block < 500; ++block)
        session.process(kBlock);

    REQUIRE(session.sounding_slot_count() == 0);
    REQUIRE_FALSE(session.history().empty());
    for (const auto& record : session.history())
        REQUIRE(record.duration.value > 0);

    const auto* sequence = session.project().find_sequence(kSequenceId);
    REQUIRE(sequence != nullptr);
    std::uint64_t next_item_id = 500;
    const auto commands = flatten_launch_history(*sequence, session.history(), next_item_id);
    REQUIRE(commands.size() == session.history().size());
    REQUIRE(next_item_id == 500 + commands.size());

    // Every emitted command is an ordinary InsertClip against an arrangement
    // lane — the flatten is expressed in the model, not in a launcher-specific
    // command or a subsystem hop.
    for (const auto& command : commands)
        REQUIRE(std::holds_alternative<timeline::InsertClip>(command));

    // Committing the capture through the ordinary transaction path leaves each
    // launcher row holding exactly the one clip its slot performed.
    REQUIRE(session.apply(commands));
    REQUIRE(clip_count(session.project(), track_id) == 1);
    REQUIRE(clip_count(session.project(), {11}) == 1);
    REQUIRE(clip_count(session.project(), {12}) == 1);
}

TEST_CASE("launch session selects providers per track through the arbitration primitive",
          "[timeline][examples][launch][arbitration]") {
    TimelineLaunchSession session;
    REQUIRE(session.prepare(kSampleRate, kBlock));

    // The authored session starts hybrid: two launcher tracks and one that keeps
    // playing its arrangement. That per-track split is the point of the
    // primitive — a session is not globally in "launcher mode".
    REQUIRE(session.track_provider({10}) == playback::ProviderKind::Launcher);
    REQUIRE(session.track_provider({11}) == playback::ProviderKind::Launcher);
    REQUIRE(session.track_provider({12}) == playback::ProviderKind::Arrangement);

    REQUIRE(session.set_track_provider({12}, playback::ProviderKind::Launcher));
    REQUIRE(session.track_provider({12}) == playback::ProviderKind::Launcher);
    REQUIRE(session.set_track_provider({10}, playback::ProviderKind::Arrangement));
    REQUIRE(session.track_provider({10}) == playback::ProviderKind::Arrangement);

    REQUIRE_FALSE(session.set_track_provider({999}, playback::ProviderKind::Launcher));
}
