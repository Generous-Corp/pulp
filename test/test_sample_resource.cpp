#include "harness/rt_allocation_probe.hpp"

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/sample_resource.hpp>

#include <atomic>
#include <chrono>
#include <thread>

using namespace pulp::audio;

namespace {

AudioFileData make_sample(uint32_t channels, uint64_t frames, uint32_t sample_rate = 48000) {
    AudioFileData data;
    data.sample_rate = sample_rate;
    data.channels.resize(channels);
    for (uint32_t ch = 0; ch < channels; ++ch) {
        data.channels[ch].resize(static_cast<std::size_t>(frames));
        for (uint64_t i = 0; i < frames; ++i) {
            data.channels[ch][static_cast<std::size_t>(i)] =
                static_cast<float>(ch + 1) * 0.1f + static_cast<float>(i) * 0.001f;
        }
    }
    return data;
}

SampleResourcePage make_page(uint64_t start_frame,
                             std::initializer_list<float> samples,
                             uint32_t sample_rate = 48000) {
    AudioFileData data;
    data.sample_rate = sample_rate;
    data.channels.resize(1);
    data.channels[0].assign(samples.begin(), samples.end());
    return {
        .start_frame = start_frame,
        .data = std::move(data),
    };
}

} // namespace

TEST_CASE("SampleResourceHandle publishes loaded sample snapshots",
          "[audio][sample-resource][phase3]") {
    SampleResourceHandle handle;
    REQUIRE_FALSE(handle.snapshot().ready());

    auto data = make_sample(2, 16, 44100);
    REQUIRE(handle.publish_loaded(std::move(data), "kick.wav", 1024));

    const auto snap = handle.snapshot();
    REQUIRE(snap.status == SampleResourceStatus::Loaded);
    REQUIRE(snap.ready());
    REQUIRE(snap.generation == 1);
    REQUIRE(snap.channel_count == 2);
    REQUIRE(snap.frame_count == 16);
    REQUIRE(snap.sample_rate == 44100);
    REQUIRE(snap.byte_size == 2 * 16 * sizeof(float));
    REQUIRE(snap.memory_budget_bytes == 1024);
    REQUIRE(snap.data != nullptr);
    REQUIRE(snap.data->channels[1][3] > snap.data->channels[0][3]);

    const auto diag = handle.diagnostics();
    REQUIRE(diag.status == SampleResourceStatus::Loaded);
    REQUIRE(diag.path == "kick.wav");
    REQUIRE(diag.reason.empty());
    REQUIRE(diag.byte_size == snap.byte_size);
}

TEST_CASE("SampleResourceHandle records missing resources without sample data",
          "[audio][sample-resource][missing][phase3]") {
    SampleResourceHandle handle;
    handle.publish_missing("missing/snare.wav", "file not found");

    const auto snap = handle.snapshot();
    REQUIRE(snap.status == SampleResourceStatus::Missing);
    REQUIRE_FALSE(snap.ready());
    REQUIRE(snap.generation == 1);
    REQUIRE(snap.data == nullptr);

    const auto diag = handle.diagnostics();
    REQUIRE(diag.status == SampleResourceStatus::Missing);
    REQUIRE(diag.path == "missing/snare.wav");
    REQUIRE(diag.reason == "file not found");
}

TEST_CASE("SampleResourceHandle rejects decoded samples over memory budget",
          "[audio][sample-resource][budget][phase3]") {
    SampleResourceHandle handle;
    auto data = make_sample(2, 64);
    const auto bytes = SampleResourceHandle::decoded_byte_size(data);

    REQUIRE_FALSE(handle.publish_loaded(std::move(data), "oversized.wav", bytes - 1));

    const auto snap = handle.snapshot();
    REQUIRE(snap.status == SampleResourceStatus::Oversized);
    REQUIRE_FALSE(snap.ready());
    REQUIRE(snap.byte_size == bytes);
    REQUIRE(snap.memory_budget_bytes == bytes - 1);
    REQUIRE(snap.data == nullptr);

    const auto diag = handle.diagnostics();
    REQUIRE(diag.status == SampleResourceStatus::Oversized);
    REQUIRE(diag.path == "oversized.wav");
    REQUIRE(diag.reason == "decoded sample exceeds memory budget");
}

TEST_CASE("SampleResourceHandle relink advances generation and replaces state",
          "[audio][sample-resource][relink][phase3]") {
    SampleResourceHandle handle;
    handle.publish_missing("old.wav", "file not found");
    const auto missing = handle.snapshot();

    auto data = make_sample(1, 8);
    REQUIRE(handle.publish_loaded(std::move(data), "new.wav"));
    const auto loaded = handle.snapshot();

    REQUIRE(missing.generation == 1);
    REQUIRE(loaded.generation == 2);
    REQUIRE(loaded.status == SampleResourceStatus::Loaded);
    REQUIRE(loaded.ready());
    REQUIRE(loaded.channel_count == 1);
    REQUIRE(loaded.frame_count == 8);
    REQUIRE(handle.diagnostics().path == "new.wav");
}

TEST_CASE("SampleResourceHandle snapshot is allocation-free after publish",
          "[audio][sample-resource][rt-safety][phase3]") {
    SampleResourceHandle handle;
    auto data = make_sample(2, 32);
    REQUIRE(handle.publish_loaded(std::move(data), "rt.wav"));

    SampleResourceSnapshot snap;
    {
        pulp::test::RtAllocationProbe probe;
        snap = handle.snapshot();
        REQUIRE_FALSE(probe.saw_allocation());
    }

    REQUIRE(snap.ready());
    REQUIRE(snap.data != nullptr);
    REQUIRE(snap.frame_count == 32);
}

TEST_CASE("SampleResourceCache records cache misses without publishing data",
          "[audio][sample-resource][cache][miss][phase3]") {
    SampleResourceCache cache({.max_entries = 2});
    REQUIRE(cache.get("missing.wav") == nullptr);

    const auto stats = cache.stats();
    REQUIRE(stats.entries == 0);
    REQUIRE(stats.hits == 0);
    REQUIRE(stats.misses == 1);
    REQUIRE(stats.evictions == 0);
}

TEST_CASE("SampleResourceCache publishes cached sample data to a resource handle",
          "[audio][sample-resource][cache][phase3]") {
    SampleResourceCache cache({.max_entries = 2});
    auto data = make_sample(2, 12, 44100);
    REQUIRE(cache.put("loop.wav", std::move(data)));

    auto cached = cache.get("loop.wav");
    REQUIRE(cached != nullptr);

    SampleResourceHandle handle;
    REQUIRE(handle.publish_loaded(cached, "loop.wav"));

    const auto snap = handle.snapshot();
    REQUIRE(snap.status == SampleResourceStatus::Loaded);
    REQUIRE(snap.ready());
    REQUIRE(snap.data == cached);
    REQUIRE(snap.frame_count == 12);
    REQUIRE(snap.channel_count == 2);
    REQUIRE(snap.sample_rate == 44100);

    const auto stats = cache.stats();
    REQUIRE(stats.entries == 1);
    REQUIRE(stats.hits == 1);
    REQUIRE(stats.misses == 0);
}

TEST_CASE("SampleResourceCache evicts least recently used decoded samples",
          "[audio][sample-resource][cache][eviction][phase3]") {
    SampleResourceCache cache({.max_entries = 2});
    REQUIRE(cache.put("a.wav", make_sample(1, 4)));
    REQUIRE(cache.put("b.wav", make_sample(1, 4)));
    REQUIRE(cache.get("a.wav") != nullptr);

    REQUIRE(cache.put("c.wav", make_sample(1, 4)));
    REQUIRE(cache.get("b.wav") == nullptr);
    REQUIRE(cache.get("a.wav") != nullptr);
    REQUIRE(cache.get("c.wav") != nullptr);

    const auto stats = cache.stats();
    REQUIRE(stats.entries == 2);
    REQUIRE(stats.evictions == 1);
    REQUIRE(stats.misses == 1);
}

TEST_CASE("SampleResourceCache rejects oversized decoded samples without eviction",
          "[audio][sample-resource][cache][budget][phase3]") {
    auto small = make_sample(1, 4);
    const auto small_bytes = SampleResourceHandle::decoded_byte_size(small);
    SampleResourceCache cache({
        .max_entries = 2,
        .max_decoded_bytes = small_bytes * 2,
    });

    REQUIRE(cache.put("small.wav", std::move(small)));
    REQUIRE_FALSE(cache.put("too-large.wav", make_sample(2, 16)));

    REQUIRE(cache.get("small.wav") != nullptr);
    REQUIRE(cache.get("too-large.wav") == nullptr);

    const auto stats = cache.stats();
    REQUIRE(stats.entries == 1);
    REQUIRE(stats.rejections == 1);
    REQUIRE(stats.evictions == 0);
}

TEST_CASE("SampleResourcePageHandoff reads resident pages without file callbacks",
          "[audio][sample-resource][page-handoff][rt-safety][phase3]") {
    std::atomic<int> file_reads{0};
    auto decode_page = [&](uint64_t start_frame) {
        ++file_reads;
        return make_page(start_frame, {1.0f, 2.0f, 3.0f, 4.0f});
    };

    SampleResourcePageWindow window;
    window.sample_rate = 48000;
    window.channel_count = 1;
    window.generation = 7;
    window.pages.push_back(decode_page(8));
    REQUIRE(file_reads.load() == 1);

    SampleResourcePageHandoff handoff;
    handoff.publish(std::move(window));

    float out[4]{};
    {
        pulp::test::RtAllocationProbe probe;
        handoff.read_channel(out, 0, 8, 4);
        REQUIRE_FALSE(probe.saw_allocation());
    }

    REQUIRE(out[0] == 1.0f);
    REQUIRE(out[1] == 2.0f);
    REQUIRE(out[2] == 3.0f);
    REQUIRE(out[3] == 4.0f);
    REQUIRE(file_reads.load() == 1);

    const auto diag = handoff.diagnostics();
    REQUIRE(diag.generation == 7);
    REQUIRE(diag.page_count == 1);
    REQUIRE(diag.covered_frame_count == 4);
    REQUIRE(diag.page_misses == 0);
}

TEST_CASE("SampleResourcePageHandoff reports cache misses as silence",
          "[audio][sample-resource][page-handoff][miss][phase3]") {
    SampleResourcePageWindow window;
    window.sample_rate = 48000;
    window.channel_count = 1;
    window.generation = 2;
    window.pages.push_back(make_page(16, {0.25f, 0.5f}));

    SampleResourcePageHandoff handoff;
    handoff.publish(std::move(window));

    float out[4]{1.0f, 1.0f, 1.0f, 1.0f};
    handoff.read_channel(out, 0, 14, 4);

    REQUIRE(out[0] == 0.0f);
    REQUIRE(out[1] == 0.0f);
    REQUIRE(out[2] == 0.25f);
    REQUIRE(out[3] == 0.5f);

    const auto diag = handoff.diagnostics();
    REQUIRE(diag.page_misses == 2);
    REQUIRE(diag.last_miss_frame == 15);
}

TEST_CASE("SampleResourcePageHandoff window replacement resets page misses",
          "[audio][sample-resource][page-handoff][phase3]") {
    SampleResourcePageHandoff handoff;
    REQUIRE(handoff.sample_or_zero(0, 99) == 0.0f);
    REQUIRE(handoff.diagnostics().page_misses == 1);

    SampleResourcePageWindow first;
    first.generation = 1;
    first.channel_count = 1;
    first.sample_rate = 48000;
    first.pages.push_back(make_page(0, {0.1f}));
    handoff.publish(std::move(first));
    REQUIRE(handoff.diagnostics().page_misses == 0);
    REQUIRE(handoff.sample_or_zero(0, 0) == 0.1f);

    SampleResourcePageWindow second;
    second.generation = 2;
    second.channel_count = 1;
    second.sample_rate = 48000;
    second.pages.push_back(make_page(4, {0.4f}));
    handoff.publish(std::move(second));

    REQUIRE(handoff.sample_or_zero(0, 0) == 0.0f);
    REQUIRE(handoff.sample_or_zero(0, 4) == 0.4f);
    const auto diag = handoff.diagnostics();
    REQUIRE(diag.generation == 2);
    REQUIRE(diag.page_misses == 1);
    REQUIRE(diag.last_miss_frame == 0);
}

TEST_CASE("SampleResourceService loads decoded samples on a background job",
          "[audio][sample-resource][service][background][phase3]") {
    std::atomic<int> decode_count{0};
    std::string decoded_path;
    SampleResourceService service(
        {},
        [&](const std::string& path, pulp::runtime::BackgroundJobContext&) -> std::optional<AudioFileData> {
            decoded_path = path;
            ++decode_count;
            return make_sample(2, 10, 44100);
        });

    SampleResourceHandle handle;
    auto job = service.load_async("kick.wav", handle);
    REQUIRE(job.valid());
    job.wait();
    REQUIRE(service.drain_completions() == 1);

    const auto snap = handle.snapshot();
    REQUIRE(snap.status == SampleResourceStatus::Loaded);
    REQUIRE(snap.ready());
    REQUIRE(snap.frame_count == 10);
    REQUIRE(snap.channel_count == 2);
    REQUIRE(snap.sample_rate == 44100);
    REQUIRE(decoded_path == "kick.wav");
    REQUIRE(decode_count.load() == 1);

    const auto cache_stats = service.cache_stats();
    REQUIRE(cache_stats.entries == 1);
    REQUIRE(cache_stats.puts == 1);

    const auto stats = service.stats();
    REQUIRE(stats.submitted_loads == 1);
    REQUIRE(stats.decode_successes == 1);
    REQUIRE(stats.drained_completions == 1);
}

TEST_CASE("SampleResourceService prefetch caches decoded data for later publish",
          "[audio][sample-resource][service][prefetch][cache][phase3]") {
    std::atomic<int> decode_count{0};
    std::string decoded_path;
    SampleResourceService service(
        {},
        [&](const std::string& path, pulp::runtime::BackgroundJobContext&) -> std::optional<AudioFileData> {
            decoded_path = path;
            ++decode_count;
            return make_sample(1, 6, 48000);
        });

    auto prefetch = service.prefetch_async("loop.wav");
    REQUIRE(prefetch.valid());
    prefetch.wait();
    REQUIRE(service.drain_completions() == 1);

    SampleResourceHandle handle;
    REQUIRE(service.publish_cached("loop.wav", handle));

    const auto snap = handle.snapshot();
    REQUIRE(snap.status == SampleResourceStatus::Loaded);
    REQUIRE(snap.ready());
    REQUIRE(snap.frame_count == 6);
    REQUIRE(snap.channel_count == 1);
    REQUIRE(decoded_path == "loop.wav");
    REQUIRE(decode_count.load() == 1);

    SampleResourceHandle second;
    auto cached = service.load_async("loop.wav", second);
    REQUIRE_FALSE(cached.valid());
    REQUIRE(second.snapshot().ready());
    REQUIRE(decode_count.load() == 1);

    const auto stats = service.stats();
    REQUIRE(stats.submitted_prefetches == 1);
    REQUIRE(stats.submitted_loads == 1);
    REQUIRE(stats.cache_hits == 2);
}

TEST_CASE("SampleResourceService publishes missing diagnostics on decode failure",
          "[audio][sample-resource][service][missing][phase3]") {
    SampleResourceService service(
        {},
        [](const std::string&, pulp::runtime::BackgroundJobContext&) -> std::optional<AudioFileData> {
            return std::nullopt;
        });

    SampleResourceHandle handle;
    auto job = service.load_async("missing.wav", handle);
    REQUIRE(job.valid());
    job.wait();
    REQUIRE(service.drain_completions() == 1);

    const auto snap = handle.snapshot();
    REQUIRE(snap.status == SampleResourceStatus::Missing);
    REQUIRE_FALSE(snap.ready());

    const auto diag = handle.diagnostics();
    REQUIRE(diag.path == "missing.wav");
    REQUIRE(diag.reason == "decode failed");

    const auto stats = service.stats();
    REQUIRE(stats.decode_failures == 1);
    REQUIRE(stats.drained_completions == 1);
}

TEST_CASE("SampleResourceService cancellation avoids stale publication",
          "[audio][sample-resource][service][cancel][phase3]") {
    std::atomic<bool> started{false};
    SampleResourceService service(
        {},
        [&](const std::string&, pulp::runtime::BackgroundJobContext& context) -> std::optional<AudioFileData> {
            started.store(true);
            while (!context.is_cancelled()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return make_sample(1, 4);
        });

    SampleResourceHandle handle;
    auto job = service.load_async("cancelled.wav", handle);
    REQUIRE(job.valid());
    for (int i = 0; i < 200 && !started.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(started.load());

    job.cancel();
    job.wait();
    REQUIRE(service.drain_completions() == 1);
    REQUIRE_FALSE(handle.snapshot().ready());

    const auto stats = service.stats();
    REQUIRE(stats.cancelled == 1);
    REQUIRE(stats.decode_successes == 0);
}

TEST_CASE("SampleResourceService applies handle memory budgets after decode",
          "[audio][sample-resource][service][budget][phase3]") {
    SampleResourceService service(
        {},
        [](const std::string&, pulp::runtime::BackgroundJobContext&) -> std::optional<AudioFileData> {
            return make_sample(2, 16, 44100);
        });

    SampleResourceHandle handle;
    auto job = service.load_async(
        "oversized.wav",
        handle,
        {.memory_budget_bytes = sizeof(float)});
    REQUIRE(job.valid());
    job.wait();
    REQUIRE(service.drain_completions() == 1);

    const auto snap = handle.snapshot();
    REQUIRE(snap.status == SampleResourceStatus::Oversized);
    REQUIRE_FALSE(snap.ready());
    REQUIRE(snap.byte_size == 2 * 16 * sizeof(float));
    REQUIRE(snap.memory_budget_bytes == sizeof(float));
}
