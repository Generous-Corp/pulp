#include "midi_utility_test_support.hpp"

#include <type_traits>
#include <utility>

// The routing kernels own a heap ledger. These tests pin the three properties
// that make that safe: the objects are small, nothing after construction
// allocates, and a moved-from object refuses every entry point.

static_assert(!std::is_copy_constructible_v<midi::ChannelRouter>);
static_assert(!std::is_copy_constructible_v<midi::NoteRangeFilter>);
static_assert(!std::is_copy_constructible_v<midi::KeyboardSplit>);
static_assert(std::is_nothrow_move_constructible_v<midi::ChannelRouter>);
static_assert(std::is_nothrow_move_constructible_v<midi::NoteRangeFilter>);
static_assert(std::is_nothrow_move_constructible_v<midi::KeyboardSplit>);

namespace {

midi::MidiBuffer held_notes() {
    auto input = prepared_buffer();
    input.add(midi::MidiEvent::note_on(0, 60, 100));
    input.add(midi::MidiEvent::note_on(0, 64, 100));
    return input;
}

}  // namespace

TEST_CASE("MIDI routing kernels allocate only in their constructors",
          "[midi][utility][routing][rt-safety]") {
    const auto input = held_notes();
    auto output = prepared_buffer();
    auto lower = prepared_buffer();
    auto upper = prepared_buffer();

    midi::ChannelRouter router;
    midi::NoteRangeFilter range({60, 72});
    midi::KeyboardSplit split({62, true, 2, 14});

    pulp::test::RtAllocationProbe allocations;
    REQUIRE(router.process(input, output).complete);
    REQUIRE(range.process(input, output).complete);
    REQUIRE(split.process(input, lower, upper).complete);
    CHECK_FALSE(router.replace_spec(midi::ChannelRouteSpec{}));
    REQUIRE(router.replace_spec(midi::ChannelRouteSpec{}, output).complete);
    REQUIRE(range.replace_spec({60, 70}, output).complete);
    REQUIRE(split.replace_spec({61, true, 2, 14}, lower, upper).complete);
    REQUIRE(router.reset(output).complete);
    REQUIRE(range.reset(output).complete);
    REQUIRE(split.reset(lower, upper).complete);
    CHECK(allocations.allocation_count() == 0);
}

TEST_CASE("a moved-from MIDI routing kernel refuses every entry point",
          "[midi][utility][routing][ownership]") {
    const auto input = held_notes();
    auto output = prepared_buffer();
    auto lower = prepared_buffer();
    auto upper = prepared_buffer();

    SECTION("channel router") {
        midi::ChannelRouter source;
        REQUIRE(source.process(input, output).complete);
        midi::ChannelRouter moved(std::move(source));
        CHECK(moved.valid());
        REQUIRE(moved.process(input, output).complete);

        CHECK_FALSE(source.valid());
        CHECK_FALSE(source.process(input, output).complete);
        CHECK(output.empty());
        CHECK_FALSE(source.flush(output).complete);
        CHECK_FALSE(source.reset(output).complete);
        CHECK_FALSE(source.replace_spec(midi::ChannelRouteSpec{}));
        CHECK_FALSE(source.replace_spec(midi::ChannelRouteSpec{}, output).complete);
    }
    SECTION("note range filter") {
        midi::NoteRangeFilter source({60, 72});
        midi::NoteRangeFilter moved({0, 127});
        moved = std::move(source);
        CHECK(moved.valid());

        CHECK_FALSE(source.valid());
        CHECK_FALSE(source.process(input, output).complete);
        CHECK(output.empty());
        CHECK_FALSE(source.flush(output).complete);
        CHECK_FALSE(source.reset(output).complete);
        CHECK_FALSE(source.replace_spec({60, 70}));
        CHECK_FALSE(source.replace_spec({60, 70}, output).complete);
    }
    SECTION("keyboard split") {
        midi::KeyboardSplit source({62, true, 2, 14});
        midi::KeyboardSplit moved(std::move(source));
        CHECK(moved.valid());

        CHECK_FALSE(source.valid());
        CHECK_FALSE(source.process(input, lower, upper).complete);
        CHECK(lower.empty());
        CHECK(upper.empty());
        CHECK_FALSE(source.flush(lower, upper).complete);
        CHECK_FALSE(source.reset(lower, upper).complete);
        CHECK_FALSE(source.replace_spec({61, true, 2, 14}));
        CHECK_FALSE(source.replace_spec({61, true, 2, 14}, lower, upper).complete);
    }
}
