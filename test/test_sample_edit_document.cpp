#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/sample_edit_document.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>

using pulp::audio::EditableSampleDocument;
using pulp::audio::EditableSampleSource;
using pulp::audio::PublishedSampleView;
using pulp::audio::SampleEditHistory;
using pulp::audio::SampleEditOperationKind;

namespace {

EditableSampleSource sample_source(std::uint64_t frames = 48000) {
    return EditableSampleSource{
        .sample_id = 42,
        .generation = 7,
        .num_channels = 2,
        .num_frames = frames,
        .sample_rate = 48000.0,
    };
}

}  // namespace

TEST_CASE("EditableSampleSource imports published sample metadata", "[audio][sampler][edit]") {
    PublishedSampleView view{
        .valid = true,
        .slot_index = 3,
        .generation = 12,
        .num_channels = 2,
        .num_frames = 96000,
        .sample_rate = 48000.0,
    };

    auto source = EditableSampleSource::from_published_sample(99, view);
    REQUIRE(source.valid());
    REQUIRE(source.sample_id == 99);
    REQUIRE(source.generation == 12);
    REQUIRE(source.num_channels == 2);
    REQUIRE(source.num_frames == 96000);
    REQUIRE(source.sample_rate == 48000.0);

    REQUIRE_FALSE(EditableSampleSource::from_published_sample(99, PublishedSampleView{}).valid());
    REQUIRE_FALSE(EditableSampleSource{.sample_id = 99}.valid());
}

TEST_CASE("EditableSampleDocument builds validated metadata operations", "[audio][sampler][edit]") {
    EditableSampleDocument document;
    REQUIRE(document.reset(sample_source(1000)));

    REQUIRE(document.valid());
    REQUIRE(document.state().revision == 1);
    REQUIRE(document.state().trim_start == 0);
    REQUIRE(document.state().trim_end == 1000);
    REQUIRE(document.state().trimmed_frames() == 1000);

    auto trim = document.make_set_trim(100, 900);
    REQUIRE(trim.valid());
    REQUIRE(trim.kind == SampleEditOperationKind::set_trim);
    REQUIRE(document.apply(trim));
    REQUIRE(document.state().revision == 2);
    REQUIRE(document.state().trim_start == 100);
    REQUIRE(document.state().trim_end == 900);
    REQUIRE(document.state().trimmed_frames() == 800);

    auto fades = document.make_set_fades(64, 128);
    REQUIRE(fades.valid());
    REQUIRE(document.apply(fades));
    REQUIRE(document.state().fade_in_frames == 64);
    REQUIRE(document.state().fade_out_frames == 128);

    auto loop = document.make_set_loop(200, 800);
    REQUIRE(loop.valid());
    REQUIRE(document.apply(loop));
    REQUIRE(document.state().has_loop);
    REQUIRE(document.state().loop_start == 200);
    REQUIRE(document.state().loop_end == 800);

    auto reverse = document.make_set_reverse(true);
    REQUIRE(reverse.valid());
    REQUIRE(document.apply(reverse));
    REQUIRE(document.state().reversed);

    auto normalize = document.make_set_normalize_gain(0.5f);
    REQUIRE(normalize.valid());
    REQUIRE(document.apply(normalize));
    REQUIRE(document.state().normalize_gain == 0.5f);

    REQUIRE_FALSE(document.make_set_trim(900, 100).valid());
    REQUIRE_FALSE(document.make_set_fades(900, 1).valid());
    REQUIRE_FALSE(document.make_set_loop(50, 120).valid());
    REQUIRE_FALSE(document.make_set_normalize_gain(0.0f).valid());

    auto forged = document.make_set_trim(120, 700);
    REQUIRE(forged.valid());
    forged.kind = SampleEditOperationKind::set_reverse;
    REQUIRE_FALSE(forged.valid());

    auto tighter_trim = document.make_set_trim(300, 600);
    REQUIRE(tighter_trim.valid());
    REQUIRE_FALSE(tighter_trim.after.has_loop);
    REQUIRE(document.apply(tighter_trim));
    REQUIRE_FALSE(document.state().has_loop);
}

TEST_CASE("SampleEditHistory performs bounded undo redo", "[audio][sampler][edit]") {
    EditableSampleDocument document;
    REQUIRE(document.reset(sample_source(1000)));

    SampleEditHistory history;
    REQUIRE(history.prepare(2));
    REQUIRE(history.capacity() == 2);

    {
        pulp::runtime::ScopedNoAlloc no_alloc;
        REQUIRE(history.perform(document, document.make_set_trim(100, 900)));
        REQUIRE(history.perform(document, document.make_set_reverse(true)));
    }

    REQUIRE(history.undo_count() == 2);
    REQUIRE(history.redo_count() == 0);
    REQUIRE(document.state().reversed);

    REQUIRE(history.undo(document));
    REQUIRE_FALSE(document.state().reversed);
    REQUIRE(document.state().trim_start == 100);
    REQUIRE(history.undo(document));
    REQUIRE(document.state().trim_start == 0);
    REQUIRE(document.state().trim_end == 1000);
    REQUIRE_FALSE(history.can_undo());
    REQUIRE(history.redo_count() == 2);

    REQUIRE(history.redo(document));
    REQUIRE(document.state().trim_start == 100);
    REQUIRE(document.state().trim_end == 900);

    auto fades = document.make_set_fades(10, 10);
    REQUIRE(history.perform(document, fades));
    REQUIRE(history.redo_count() == 0);
    REQUIRE(document.state().fade_in_frames == 10);
}

TEST_CASE("SampleEditHistory rejects untracked over-capacity edits before applying",
          "[audio][sampler][edit]") {
    EditableSampleDocument document;
    REQUIRE(document.reset(sample_source(1000)));

    SampleEditHistory history;
    REQUIRE(history.prepare(1));

    REQUIRE(history.perform(document, document.make_set_trim(100, 900)));
    REQUIRE(document.state().trim_start == 100);

    auto over_capacity = document.make_set_reverse(true);
    REQUIRE(over_capacity.valid());
    REQUIRE_FALSE(history.perform(document, over_capacity));
    REQUIRE_FALSE(document.state().reversed);
    REQUIRE(history.undo_count() == 1);
}

TEST_CASE("EditableSampleDocument rejects stale operations", "[audio][sampler][edit]") {
    EditableSampleDocument document;
    REQUIRE(document.reset(sample_source(1000)));

    auto trim = document.make_set_trim(100, 900);
    auto stale = document.make_set_reverse(true);

    REQUIRE(document.apply(trim));
    REQUIRE_FALSE(document.apply(stale));
    REQUIRE_FALSE(document.state().reversed);
}
