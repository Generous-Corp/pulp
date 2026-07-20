#include <pulp/timeline/model.hpp>

#include <catch2/catch_test_macros.hpp>

namespace {

template <typename T, typename E> T take(pulp::runtime::Result<T, E> result) {
    REQUIRE(result);
    return std::move(result).value();
}

using namespace pulp::timeline;
using pulp::timebase::RationalRate;
using pulp::timebase::TickDuration;

SequenceMarker marker(std::uint64_t id, std::int64_t tick, std::string name = {}) {
    return {{id}, MarkerTypeId::cue(), std::move(name), MusicalSequencePoint{{tick}}};
}

SequenceRegion region(std::uint64_t id, std::int64_t tick, std::int64_t duration,
                      std::string name = {}) {
    return {{id}, std::move(name), MusicalSequenceRange{{tick}, {duration}}};
}

} // namespace

TEST_CASE("Marker type identifiers are validated namespaced values", "[timeline][annotation]") {
    REQUIRE(MarkerTypeId::create("vendor.marker.chord"));
    CHECK_FALSE(MarkerTypeId::create("cue"));
    CHECK_FALSE(MarkerTypeId::create("Vendor.marker"));
    CHECK_FALSE(MarkerTypeId::create("vendor..marker"));
}

TEST_CASE("Sequence annotations are canonical", "[timeline][annotation]") {
    auto sequence = take(Sequence::create(SequenceInput{
        .id = {1},
        .name = "root",
        .musical_duration = TickDuration{100},
        .markers = {marker(4, 50), marker(3, 10)},
        .regions = {region(6, 40, 20), region(5, 5, 10)},
    }));

    REQUIRE(sequence.markers().size() == 2);
    CHECK(sequence.markers()[0].id == ItemId{3});
    CHECK(sequence.markers()[1].id == ItemId{4});
    REQUIRE(sequence.regions().size() == 2);
    CHECK(sequence.regions()[0].id == ItemId{5});
    CHECK(sequence.regions()[1].id == ItemId{6});
    CHECK(sequence.find_marker({4}) != nullptr);
    CHECK(sequence.find_region({6}) != nullptr);

}

TEST_CASE("Sequence annotations validate typed time against their sequence", "[timeline][annotation]") {
    auto past_end = Sequence::create(SequenceInput{.id = {1},
                                                   .name = "root",
                                                   .musical_duration = TickDuration{10},
                                                   .markers = {marker(2, 11)}});
    REQUIRE_FALSE(past_end);
    CHECK(past_end.error().code == ModelErrorCode::InvalidDuration);

    auto empty_region = Sequence::create(SequenceInput{.id = {1},
                                                       .name = "root",
                                                       .regions = {region(2, 0, 0)}});
    REQUIRE_FALSE(empty_region);
    CHECK(empty_region.error().code == ModelErrorCode::InvalidDuration);

    const auto type = MarkerTypeId::cue();
    auto invalid_marker_rate = Sequence::create(SequenceInput{
        .id = {1},
        .name = "root",
        .markers = {{{2}, type, "cue", AbsoluteSequencePoint{{0}, RationalRate{48'000, 0}}}},
    });
    REQUIRE_FALSE(invalid_marker_rate);
    CHECK(invalid_marker_rate.error().code == ModelErrorCode::InvalidSampleRate);

    auto invalid_region_rate = Sequence::create(SequenceInput{
        .id = {1},
        .name = "root",
        .regions = {{{2}, "verse", AbsoluteSequenceRange{{0}, 100, RationalRate{0, 1}}}},
    });
    REQUIRE_FALSE(invalid_region_rate);
    CHECK(invalid_region_rate.error().code == ModelErrorCode::InvalidSampleRate);

    auto mixed_rates = Sequence::create(SequenceInput{
        .id = {1},
        .name = "root",
        .markers = {
            {{2}, type, "a", AbsoluteSequencePoint{{48'000}, RationalRate{48'000, 1}}},
            {{3}, type, "b", AbsoluteSequencePoint{{44'100}, RationalRate{44'100, 1}}},
        },
    });
    REQUIRE_FALSE(mixed_rates);
    CHECK(mixed_rates.error().code == ModelErrorCode::IncompatibleSampleRate);

    auto mixed_annotation_rates = Sequence::create(SequenceInput{
        .id = {1},
        .name = "root",
        .markers = {
            {{2}, type, "cue", AbsoluteSequencePoint{{48'000}, RationalRate{48'000, 1}}},
        },
        .regions = {
            {{3}, "verse", AbsoluteSequenceRange{{44'100}, 100, RationalRate{44'100, 1}}},
        },
    });
    REQUIRE_FALSE(mixed_annotation_rates);
    CHECK(mixed_annotation_rates.error().code == ModelErrorCode::IncompatibleSampleRate);
}

TEST_CASE("Sequence annotation edits preserve identity and participate in remap",
          "[timeline][annotation]") {
    auto sequence = take(Sequence::create(SequenceInput{.id = {2},
                                                        .name = "root",
                                                        .musical_duration = TickDuration{100},
                                                        .markers = {marker(3, 10, "cue")},
                                                        .regions = {region(4, 20, 10, "verse")}}));
    auto project = take(Project::create({{1}, "project", 5, {2}, {}, {sequence}}));
    REQUIRE(project.locate({3}));
    CHECK(project.locate({3})->kind == ItemKind::SequenceMarker);
    REQUIRE(project.locate({4}));
    CHECK(project.locate({4})->kind == ItemKind::SequenceRegion);

    sequence = take(sequence.replace_marker(marker(3, 30, "moved")));
    CHECK(sequence.find_marker({3})->name == "moved");
    sequence = take(sequence.erase_region({4}));
    CHECK(sequence.regions().empty());

    ItemIdAllocator allocator(20);
    auto source = take(Sequence::create(SequenceInput{.id = {2},
                                                      .name = "root",
                                                      .musical_duration = TickDuration{100},
                                                      .markers = {marker(3, 10)},
                                                      .regions = {region(4, 20, 10)}}));
    auto remapped = take(remap_ids(source, allocator));
    REQUIRE(remapped.ids.find({3}));
    CHECK(remapped.sequence.find_marker(*remapped.ids.find({3})) != nullptr);
    REQUIRE(remapped.ids.find({4}));
    CHECK(remapped.sequence.find_region(*remapped.ids.find({4})) != nullptr);
}
