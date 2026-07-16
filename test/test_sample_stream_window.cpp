#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/audio/sample_stream_window.hpp>
#include <pulp/audio/sample_stream_scheduler.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>

#include <algorithm>

using Catch::Matchers::WithinAbs;
using pulp::audio::Buffer;
using pulp::audio::BufferView;
using pulp::audio::SampleStreamPageDescriptor;
using pulp::audio::SampleStreamPageRequest;
using pulp::audio::SampleStreamPageState;
using pulp::audio::SampleStreamRequestInbox;
using pulp::audio::SampleStreamScheduleStatus;
using pulp::audio::SampleStreamScheduler;
using pulp::audio::SampleStreamWindow;
using pulp::audio::SampleStreamWindowConfig;
using pulp::audio::SampleStreamWindowReadRequest;

namespace {

void fill_channel(Buffer<float>& buffer, std::size_t channel, float start) {
    for (std::size_t frame = 0; frame < buffer.num_samples(); ++frame) {
        buffer.channel(channel)[frame] = start + static_cast<float>(frame);
    }
}

void fill_all(Buffer<float>& buffer, float value) {
    for (std::size_t channel = 0; channel < buffer.num_channels(); ++channel) {
        std::fill(buffer.channel(channel).begin(), buffer.channel(channel).end(), value);
    }
}

void publish_page(SampleStreamWindow& window,
                  std::uint32_t page_index,
                  std::uint64_t generation,
                  std::uint64_t start_frame,
                  Buffer<float>& source,
                  std::uint64_t valid_frames,
                  bool final_page = false) {
    REQUIRE(window.begin_fill_page(page_index));
    REQUIRE(window.copy_to_filling_page(page_index, source.view(), valid_frames));
    REQUIRE(window.publish_page(page_index,
                                SampleStreamPageDescriptor{
                                    .stream_generation = generation,
                                    .start_frame = start_frame,
                                    .valid_frames = valid_frames,
                                    .final_page = final_page,
                                }));
}

}  // namespace

TEST_CASE("SampleStreamWindow validates config and resets prepared storage",
          "[audio][sampler][stream-window]") {
    SampleStreamWindow window;
    REQUIRE_FALSE(window.prepare({}));
    REQUIRE_FALSE(window.prepared());
    REQUIRE_FALSE(window.prepare(SampleStreamWindowConfig{
        .channels = 1,
        .page_count = 0,
        .page_frames = 8,
    }));

    REQUIRE(window.prepare(SampleStreamWindowConfig{
        .channels = 2,
        .page_count = 2,
        .page_frames = 4,
    }));
    REQUIRE(window.prepared());
    REQUIRE(window.channels() == 2);
    REQUIRE(window.page_count() == 2);
    REQUIRE(window.page_frames() == 4);
    REQUIRE(window.page_state(0) == SampleStreamPageState::Empty);

    Buffer<float> source(2, 4);
    fill_channel(source, 0, 1.0f);
    fill_channel(source, 1, 10.0f);
    publish_page(window, 0, 1, 0, source, 4);
    REQUIRE(window.stats().ready_pages == 1);

    window.reset_when_quiescent();
    REQUIRE(window.page_state(0) == SampleStreamPageState::Empty);
    REQUIRE(window.stats().ready_pages == 0);
    REQUIRE(window.stats().pages_published == 0);

    window.release();
    REQUIRE_FALSE(window.prepared());
}

TEST_CASE("SampleStreamWindow publishes and reads an exact ready page",
          "[audio][sampler][stream-window]") {
    SampleStreamWindow window;
    REQUIRE(window.prepare(SampleStreamWindowConfig{
        .channels = 2,
        .page_count = 2,
        .page_frames = 4,
    }));

    Buffer<float> source(2, 4);
    fill_channel(source, 0, 1.0f);
    fill_channel(source, 1, 10.0f);
    publish_page(window, 0, 7, 16, source, 4);

    const auto view = window.ready_page_for_frame(7, 18);
    REQUIRE(view.valid);
    REQUIRE(view.page_index == 0);
    REQUIRE(view.local_offset == 2);
    REQUIRE(window.ready_channel_data(view, 0)[0] == 3.0f);
    auto forged_view = view;
    forged_view.stream_generation = 99;
    REQUIRE(window.ready_channel_data(forged_view, 0) == nullptr);
    forged_view = view;
    forged_view.local_offset = view.valid_frames;
    REQUIRE(window.ready_channel_data(forged_view, 0) == nullptr);

    Buffer<float> destination(2, 4);
    fill_all(destination, -1.0f);
    const auto result = window.read_frames(
        destination.view(),
        SampleStreamWindowReadRequest{
            .stream_generation = 7,
            .start_frame = 16,
            .frames = 4,
        });

    REQUIRE(result.requested_frames == 4);
    REQUIRE(result.copied_frames == 4);
    REQUIRE(result.missed_frames == 0);
    REQUIRE(result.complete);
    REQUIRE_THAT(destination.channel(0)[0], WithinAbs(1.0f, 1.0e-6f));
    REQUIRE_THAT(destination.channel(0)[3], WithinAbs(4.0f, 1.0e-6f));
    REQUIRE_THAT(destination.channel(1)[0], WithinAbs(10.0f, 1.0e-6f));
    REQUIRE_THAT(destination.channel(1)[3], WithinAbs(13.0f, 1.0e-6f));

    const auto stats = window.stats();
    REQUIRE(stats.pages_published == 1);
    REQUIRE(stats.ready_read_chunks == 1);
    REQUIRE(stats.ready_frames_read == 4);
}

TEST_CASE("SampleStreamWindow reads across pages and deterministic gaps",
          "[audio][sampler][stream-window][miss]") {
    SampleStreamWindow window;
    REQUIRE(window.prepare(SampleStreamWindowConfig{
        .channels = 1,
        .page_count = 3,
        .page_frames = 4,
    }));

    Buffer<float> first(1, 4);
    Buffer<float> second(1, 4);
    fill_channel(first, 0, 1.0f);
    fill_channel(second, 0, 9.0f);
    publish_page(window, 0, 3, 0, first, 4);
    publish_page(window, 1, 3, 8, second, 4);

    Buffer<float> destination(1, 12);
    fill_all(destination, 99.0f);
    const auto result = window.read_frames(
        destination.view(),
        SampleStreamWindowReadRequest{
            .stream_generation = 3,
            .start_frame = 0,
            .frames = 12,
        });

    REQUIRE(result.copied_frames == 8);
    REQUIRE(result.missed_frames == 4);
    REQUIRE(result.zero_filled_frames == 4);
    REQUIRE_FALSE(result.complete);
    REQUIRE_THAT(destination.channel(0)[0], WithinAbs(1.0f, 1.0e-6f));
    REQUIRE_THAT(destination.channel(0)[3], WithinAbs(4.0f, 1.0e-6f));
    REQUIRE_THAT(destination.channel(0)[4], WithinAbs(0.0f, 1.0e-6f));
    REQUIRE_THAT(destination.channel(0)[7], WithinAbs(0.0f, 1.0e-6f));
    REQUIRE_THAT(destination.channel(0)[8], WithinAbs(9.0f, 1.0e-6f));
    REQUIRE_THAT(destination.channel(0)[11], WithinAbs(12.0f, 1.0e-6f));

    fill_all(destination, 5.0f);
    const auto no_zero = window.read_frames(
        destination.view(),
        SampleStreamWindowReadRequest{
            .stream_generation = 3,
            .start_frame = 4,
            .frames = 2,
            .zero_fill_misses = false,
        });
    REQUIRE(no_zero.copied_frames == 0);
    REQUIRE(no_zero.missed_frames == 2);
    REQUIRE(no_zero.zero_filled_frames == 0);
    REQUIRE_THAT(destination.channel(0)[0], WithinAbs(5.0f, 1.0e-6f));
    REQUIRE_THAT(destination.channel(0)[1], WithinAbs(5.0f, 1.0e-6f));
}

TEST_CASE("SampleStreamWindow channel mapping follows sampler policy",
          "[audio][sampler][stream-window][channels]") {
    SampleStreamWindow mono_window;
    REQUIRE(mono_window.prepare(SampleStreamWindowConfig{
        .channels = 1,
        .page_count = 1,
        .page_frames = 3,
    }));
    Buffer<float> mono(1, 3);
    fill_channel(mono, 0, 2.0f);
    publish_page(mono_window, 0, 1, 0, mono, 3);

    Buffer<float> mono_destination(3, 3);
    mono_window.read_frames(mono_destination.view(),
                            SampleStreamWindowReadRequest{
                                .stream_generation = 1,
                                .start_frame = 0,
                                .frames = 3,
                            });
    for (std::size_t channel = 0; channel < mono_destination.num_channels(); ++channel) {
        REQUIRE_THAT(mono_destination.channel(channel)[2], WithinAbs(4.0f, 1.0e-6f));
    }

    SampleStreamWindow stereo_window;
    REQUIRE(stereo_window.prepare(SampleStreamWindowConfig{
        .channels = 2,
        .page_count = 1,
        .page_frames = 2,
    }));
    Buffer<float> stereo(2, 2);
    fill_channel(stereo, 0, 1.0f);
    fill_channel(stereo, 1, 10.0f);
    publish_page(stereo_window, 0, 2, 0, stereo, 2);

    Buffer<float> stereo_destination(4, 2);
    fill_all(stereo_destination, 7.0f);
    stereo_window.read_frames(stereo_destination.view(),
                              SampleStreamWindowReadRequest{
                                  .stream_generation = 2,
                                  .start_frame = 0,
                                  .frames = 2,
                              });
    REQUIRE_THAT(stereo_destination.channel(0)[1], WithinAbs(2.0f, 1.0e-6f));
    REQUIRE_THAT(stereo_destination.channel(1)[1], WithinAbs(11.0f, 1.0e-6f));
    REQUIRE_THAT(stereo_destination.channel(2)[1], WithinAbs(0.0f, 1.0e-6f));
    REQUIRE_THAT(stereo_destination.channel(3)[1], WithinAbs(0.0f, 1.0e-6f));
}

TEST_CASE("SampleStreamWindow rejects stale generations and overlapping ready pages",
          "[audio][sampler][stream-window][generation]") {
    SampleStreamWindow window;
    REQUIRE(window.prepare(SampleStreamWindowConfig{
        .channels = 1,
        .page_count = 3,
        .page_frames = 4,
    }));

    Buffer<float> source(1, 4);
    fill_channel(source, 0, 1.0f);
    publish_page(window, 0, 10, 0, source, 4);

    Buffer<float> stereo_source(2, 4);
    REQUIRE(window.begin_fill_page(2));
    REQUIRE_FALSE(window.copy_to_filling_page(2, stereo_source.view(), 4));
    REQUIRE(window.cancel_fill_page(2));

    Buffer<float> destination(1, 4);
    fill_all(destination, 5.0f);
    const auto stale = window.read_frames(
        destination.view(),
        SampleStreamWindowReadRequest{
            .stream_generation = 11,
            .start_frame = 0,
            .frames = 4,
        });
    REQUIRE(stale.copied_frames == 0);
    REQUIRE(stale.missed_frames == 4);
    REQUIRE_THAT(destination.channel(0)[0], WithinAbs(0.0f, 1.0e-6f));

    REQUIRE(window.begin_fill_page(1));
    REQUIRE(window.copy_to_filling_page(1, source.view(), 4));
    REQUIRE_FALSE(window.publish_page(1,
                                      SampleStreamPageDescriptor{
                                          .stream_generation = 10,
                                          .start_frame = 2,
                                          .valid_frames = 4,
                                      }));
    REQUIRE(window.publish_page(1,
                                SampleStreamPageDescriptor{
                                    .stream_generation = 12,
                                    .start_frame = 2,
                                    .valid_frames = 4,
                                }));
}

TEST_CASE("SampleStreamWindow rejects malformed channel pointers",
          "[audio][sampler][stream-window][edge]") {
    SampleStreamWindow window;
    REQUIRE(window.prepare(SampleStreamWindowConfig{
        .channels = 1,
        .page_count = 1,
        .page_frames = 4,
    }));

    float* missing_channel[] = {nullptr};
    BufferView<float> malformed(missing_channel, 1, 4);
    REQUIRE(window.begin_fill_page(0));
    REQUIRE_FALSE(window.copy_to_filling_page(0, malformed, 4));
    REQUIRE(window.cancel_fill_page(0));

    Buffer<float> source(1, 4);
    fill_channel(source, 0, 1.0f);
    publish_page(window, 0, 8, 0, source, 4);

    const auto result = window.read_frames(
        malformed,
        SampleStreamWindowReadRequest{
            .stream_generation = 8,
            .start_frame = 0,
            .frames = 4,
        });
    REQUIRE(result.requested_frames == 0);
    REQUIRE(result.copied_frames == 0);
    REQUIRE(result.missed_frames == 0);
}

TEST_CASE("SampleStreamWindow gates ready-page reuse through retire generations",
          "[audio][sampler][stream-window][generation]") {
    SampleStreamWindow window;
    REQUIRE(window.prepare(SampleStreamWindowConfig{
        .channels = 1,
        .page_count = 1,
        .page_frames = 4,
    }));

    Buffer<float> source(1, 4);
    fill_channel(source, 0, 1.0f);
    publish_page(window, 0, 1, 0, source, 4);

    REQUIRE_FALSE(window.begin_fill_page(0));
    REQUIRE_FALSE(window.retire_page(0, 0));
    REQUIRE(window.page_state(0) == SampleStreamPageState::Ready);
    REQUIRE(window.retire_page(0, 9));
    REQUIRE_FALSE(window.retire_page(0, 0));
    REQUIRE(window.page_state(0) == SampleStreamPageState::Retired);
    REQUIRE_FALSE(window.begin_fill_page(0, 0));
    REQUIRE_FALSE(window.begin_fill_page(0, 8));
    REQUIRE(window.begin_fill_page(0, 9));
    REQUIRE(window.page_state(0) == SampleStreamPageState::Filling);

    fill_channel(source, 0, 20.0f);
    REQUIRE(window.copy_to_filling_page(0, source.view(), 4));
    REQUIRE(window.publish_page(0,
                                SampleStreamPageDescriptor{
                                    .stream_generation = 2,
                                    .start_frame = 0,
                                    .valid_frames = 4,
                                }));
    REQUIRE(window.stats().pages_retired == 1);
}

TEST_CASE("SampleStreamWindow treats partial final pages as bounded data",
          "[audio][sampler][stream-window][edge]") {
    SampleStreamWindow window;
    REQUIRE(window.prepare(SampleStreamWindowConfig{
        .channels = 1,
        .page_count = 1,
        .page_frames = 4,
    }));

    Buffer<float> source(1, 4);
    fill_channel(source, 0, 5.0f);
    publish_page(window, 0, 4, 20, source, 2, true);
    REQUIRE(window.ready_page_for_frame(4, 21).final_page);

    Buffer<float> destination(1, 4);
    fill_all(destination, -1.0f);
    const auto result = window.read_frames(
        destination.view(),
        SampleStreamWindowReadRequest{
            .stream_generation = 4,
            .start_frame = 20,
            .frames = 4,
        });

    REQUIRE(result.copied_frames == 2);
    REQUIRE(result.missed_frames == 2);
    REQUIRE(result.zero_filled_frames == 2);
    REQUIRE_THAT(destination.channel(0)[0], WithinAbs(5.0f, 1.0e-6f));
    REQUIRE_THAT(destination.channel(0)[1], WithinAbs(6.0f, 1.0e-6f));
    REQUIRE_THAT(destination.channel(0)[2], WithinAbs(0.0f, 1.0e-6f));
    REQUIRE_THAT(destination.channel(0)[3], WithinAbs(0.0f, 1.0e-6f));
}

TEST_CASE("SampleStreamWindow hot lookup read and stats do not allocate",
          "[audio][sampler][stream-window][rt]") {
    SampleStreamWindow window;
    REQUIRE(window.prepare(SampleStreamWindowConfig{
        .channels = 1,
        .page_count = 2,
        .page_frames = 8,
    }));

    Buffer<float> source(1, 8);
    fill_channel(source, 0, 1.0f);
    publish_page(window, 0, 42, 0, source, 8);
    Buffer<float> destination(1, 8);

    pulp::audio::SampleStreamPageView page;
    const float* data = nullptr;
    {
        pulp::runtime::ScopedNoAlloc no_alloc;
        page = window.ready_page_for_frame(42, 3);
        data = window.ready_channel_data(page, 0);
        window.read_frames(destination.view(),
                           SampleStreamWindowReadRequest{
                               .stream_generation = 42,
                               .start_frame = 0,
                               .frames = 8,
                           });
        static_cast<void>(window.stats());
    }

    REQUIRE(page.valid);
    REQUIRE(data != nullptr);
    REQUIRE_THAT(destination.channel(0)[7], WithinAbs(8.0f, 1.0e-6f));
}

TEST_CASE("Sample stream request inbox is bounded and allocation-free",
          "[audio][sampler][stream-scheduler][rt]") {
    SampleStreamRequestInbox<2> inbox;
    const SampleStreamPageRequest first{
        .source_id = 1,
        .source_generation = 2,
        .requester_id = 1,
        .requester_generation = 1,
        .start_frame = 0,
        .frame_count = 256,
        .resident_source_frames = 128,
        .consumption_frames_per_second = 48000.0,
    };
    auto second = first;
    second.start_frame = 256;

    bool pushed_first = false;
    bool pushed_second = false;
    bool pushed_overflow = true;
    std::optional<SampleStreamPageRequest> popped;
    {
        pulp::runtime::ScopedNoAlloc no_alloc;
        pushed_first = inbox.try_push(first);
        pushed_second = inbox.try_push(second);
        pushed_overflow = inbox.try_push(first);
        popped = inbox.try_pop();
    }

    REQUIRE(pushed_first);
    REQUIRE(pushed_second);
    REQUIRE_FALSE(pushed_overflow);
    REQUIRE(popped.has_value());
    REQUIRE(popped->start_frame == 0);
    REQUIRE(inbox.telemetry().overflow_count == 1);
}

TEST_CASE("Sample stream scheduler orders by time to starvation deterministically",
          "[audio][sampler][stream-scheduler]") {
    SampleStreamScheduler scheduler;
    REQUIRE(scheduler.prepare(4));

    REQUIRE(scheduler.submit_or_refresh({
        .source_id = 1,
        .source_generation = 1,
        .requester_id = 1,
        .requester_generation = 1,
        .start_frame = 0,
        .frame_count = 256,
        .resident_source_frames = 4800,
        .consumption_frames_per_second = 48000.0,
    }) == SampleStreamScheduleStatus::Inserted);
    REQUIRE(scheduler.submit_or_refresh({
        .source_id = 2,
        .source_generation = 1,
        .requester_id = 2,
        .requester_generation = 1,
        .start_frame = 0,
        .frame_count = 256,
        .resident_source_frames = 9600,
        .consumption_frames_per_second = 192000.0,
    }) == SampleStreamScheduleStatus::Inserted);
    REQUIRE(scheduler.submit_or_refresh({
        .source_id = 3,
        .source_generation = 1,
        .requester_id = 3,
        .requester_generation = 1,
        .start_frame = 0,
        .frame_count = 256,
        .resident_source_frames = 2400,
        .consumption_frames_per_second = 48000.0,
        .demand_class = pulp::audio::SampleStreamDemandClass::Attack,
    }) == SampleStreamScheduleStatus::Inserted);
    REQUIRE(scheduler.submit_or_refresh({
        .source_id = 4,
        .source_generation = 1,
        .requester_id = 4,
        .requester_generation = 1,
        .start_frame = 0,
        .frame_count = 256,
        .resident_source_frames = 2400,
        .consumption_frames_per_second = 48000.0,
    }) == SampleStreamScheduleStatus::Inserted);

    const auto attack = scheduler.pop_most_urgent();
    const auto pitched = scheduler.pop_most_urgent();
    const auto same_deadline_later = scheduler.pop_most_urgent();
    const auto unpitched = scheduler.pop_most_urgent();
    REQUIRE(attack->source_id == 3);
    REQUIRE(pitched->source_id == 2);
    REQUIRE(same_deadline_later->source_id == 4);
    REQUIRE(unpitched->source_id == 1);
}

TEST_CASE("Prepared sample stream scheduler hot operations do not allocate",
          "[audio][sampler][stream-scheduler][rt]") {
    SampleStreamScheduler scheduler;
    REQUIRE(scheduler.prepare(2));
    SampleStreamScheduleStatus status = SampleStreamScheduleStatus::Invalid;
    std::optional<SampleStreamPageRequest> selected;
    {
        pulp::runtime::ScopedNoAlloc no_alloc;
        status = scheduler.submit_or_refresh({
            .source_id = 11,
            .source_generation = 2,
            .requester_id = 1,
            .requester_generation = 1,
            .start_frame = 512,
            .frame_count = 256,
            .resident_source_frames = 64,
            .consumption_frames_per_second = 96000.0,
        });
        selected = scheduler.pop_most_urgent();
    }

    REQUIRE(status == SampleStreamScheduleStatus::Inserted);
    REQUIRE(selected.has_value());
    REQUIRE(selected->source_id == 11);
}

TEST_CASE("Sample stream scheduler deduplicates shared pages at the highest urgency",
          "[audio][sampler][stream-scheduler]") {
    SampleStreamScheduler scheduler;
    REQUIRE(scheduler.prepare(2));

    SampleStreamPageRequest request{
        .source_id = 9,
        .source_generation = 4,
        .requester_id = 1,
        .requester_generation = 1,
        .page_index = 2,
        .start_frame = 1024,
        .frame_count = 512,
        .resident_source_frames = 4096,
        .consumption_frames_per_second = 48000.0,
        .demand_class = pulp::audio::SampleStreamDemandClass::Release,
    };
    REQUIRE(scheduler.submit_or_refresh(request) == SampleStreamScheduleStatus::Inserted);

    request.resident_source_frames = 256;
    request.consumption_frames_per_second = 96000.0;
    request.demand_class = pulp::audio::SampleStreamDemandClass::Attack;
    REQUIRE(scheduler.submit_or_refresh(request) == SampleStreamScheduleStatus::Refreshed);

    auto less_urgent = request;
    less_urgent.resident_source_frames = 10000;
    less_urgent.demand_class = pulp::audio::SampleStreamDemandClass::Release;
    REQUIRE(scheduler.submit_or_refresh(less_urgent) == SampleStreamScheduleStatus::Refreshed);

    const auto selected = scheduler.pop_most_urgent();
    REQUIRE(selected.has_value());
    REQUIRE(selected->resident_source_frames == 256);
    REQUIRE(selected->demand_class == pulp::audio::SampleStreamDemandClass::Attack);
    REQUIRE(scheduler.stats().inserted == 1);
    REQUIRE(scheduler.stats().refreshed == 2);
}

TEST_CASE("Sample stream scheduler drains, rejects overflow, and cancels generations",
          "[audio][sampler][stream-scheduler]") {
    SampleStreamRequestInbox<4> inbox;
    for (std::uint64_t page = 0; page < 3; ++page) {
        REQUIRE(inbox.try_push({
            .source_id = page < 2 ? 7u : 8u,
            .source_generation = 3,
            .requester_id = page + 1,
            .requester_generation = 1,
            .page_index = page,
            .start_frame = page * 256,
            .frame_count = 256,
            .resident_source_frames = page * 64,
            .consumption_frames_per_second = 48000.0,
        }));
    }

    SampleStreamScheduler scheduler;
    REQUIRE(scheduler.prepare(2));
    REQUIRE(scheduler.drain(inbox) == 3);
    REQUIRE(scheduler.stats().pending == 2);
    REQUIRE(scheduler.stats().rejected_full == 1);
    REQUIRE(scheduler.cancel_source_generation(7, 3) == 2);
    REQUIRE(scheduler.stats().pending == 0);
    REQUIRE(scheduler.stats().cancelled == 2);

    REQUIRE(scheduler.submit_or_refresh({}) == SampleStreamScheduleStatus::Invalid);
    REQUIRE(scheduler.stats().rejected_invalid == 1);
}

TEST_CASE("Sample stream scheduler coalesces a canonical page without sharing voice lifetime",
          "[audio][sampler][stream-scheduler]") {
    SampleStreamScheduler scheduler;
    REQUIRE(scheduler.prepare(4));

    SampleStreamPageRequest first{
        .source_id = 5,
        .source_generation = 2,
        .requester_id = 10,
        .requester_generation = 3,
        .page_index = 7,
        .start_frame = 1792,
        .frame_count = 256,
        .resident_source_frames = 128,
        .consumption_frames_per_second = 48000.0,
    };
    auto second = first;
    second.requester_id = 11;
    second.start_frame = 1700;
    second.frame_count = 400;
    second.resident_source_frames = 64;

    REQUIRE(scheduler.submit_or_refresh(first) == SampleStreamScheduleStatus::Inserted);
    REQUIRE(scheduler.submit_or_refresh(second) == SampleStreamScheduleStatus::Inserted);
    REQUIRE(scheduler.cancel_requester(10, 3) == 1);
    const auto surviving = scheduler.pop_most_urgent();
    REQUIRE(surviving.has_value());
    REQUIRE(surviving->requester_id == 11);

    REQUIRE(scheduler.submit_or_refresh(first) == SampleStreamScheduleStatus::Inserted);
    REQUIRE(scheduler.submit_or_refresh(second) == SampleStreamScheduleStatus::Inserted);
    const auto shared = scheduler.pop_most_urgent();
    REQUIRE(shared.has_value());
    REQUIRE(shared->requester_id == 11);
    REQUIRE(scheduler.stats().coalesced == 1);
    REQUIRE(scheduler.stats().pending == 0);
}

TEST_CASE("Sample stream scheduler displaces only for a more urgent demand",
          "[audio][sampler][stream-scheduler]") {
    SampleStreamScheduler scheduler;
    REQUIRE(scheduler.prepare(1));
    SampleStreamPageRequest release{
        .source_id = 1,
        .source_generation = 1,
        .requester_id = 1,
        .requester_generation = 1,
        .page_index = 1,
        .start_frame = 256,
        .frame_count = 256,
        .resident_source_frames = 9600,
        .consumption_frames_per_second = 48000.0,
        .demand_class = pulp::audio::SampleStreamDemandClass::Release,
    };
    REQUIRE(scheduler.submit_or_refresh(release) == SampleStreamScheduleStatus::Inserted);

    auto slower = release;
    slower.requester_id = 2;
    slower.page_index = 2;
    slower.resident_source_frames = 19200;
    REQUIRE(scheduler.submit_or_refresh(slower) == SampleStreamScheduleStatus::Full);

    auto attack = release;
    attack.requester_id = 3;
    attack.page_index = 3;
    attack.resident_source_frames = 64;
    attack.demand_class = pulp::audio::SampleStreamDemandClass::Attack;
    REQUIRE(scheduler.submit_or_refresh(attack) == SampleStreamScheduleStatus::Inserted);
    REQUIRE(scheduler.stats().displaced_less_urgent == 1);
    REQUIRE(scheduler.pop_most_urgent()->requester_id == 3);
}
