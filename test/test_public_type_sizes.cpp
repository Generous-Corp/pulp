#include <pulp/host/forge_dynamics_catalog.hpp>
#include <pulp/midi/routing_utility_kernels.hpp>
#include <pulp/playback/program_wire.hpp>
#include <pulp/state/sequencer_state_channel.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>

// Public value types that are large by design, each held under a ceiling a
// little above its current size. A consumer may build any of these on a stack,
// and the MSVC main-thread stack is 1 MB (a std::thread on macOS gets 512 KB),
// so growth must be a deliberate edit here rather than an accident. Types that
// must stay small carry their own static_assert in their header instead (the
// MIDI routing kernels).

namespace {
constexpr std::size_t kib(std::size_t n) { return n * 1024; }
}  // namespace

static_assert(sizeof(pulp::state::Snapshot) <= kib(100));
static_assert(sizeof(pulp::playback::ProgramWireAutomationConsumer) <= kib(100));
static_assert(sizeof(pulp::host::dynamics::true_peak::Instance) <= kib(80));

static_assert(sizeof(pulp::midi::ChannelRouter) <= 4096);
static_assert(sizeof(pulp::midi::NoteRangeFilter) <= 4096);
static_assert(sizeof(pulp::midi::KeyboardSplit) <= 4096);

TEST_CASE("large public value types stay under their stack ceilings",
          "[midi][state][playback][host][rt-safety]") {
    // The bounds are compile-time; this case records the measured sizes.
    CHECK(sizeof(pulp::state::Snapshot) <= kib(100));
    CHECK(sizeof(pulp::playback::ProgramWireAutomationConsumer) <= kib(100));
    CHECK(sizeof(pulp::host::dynamics::true_peak::Instance) <= kib(80));
}
