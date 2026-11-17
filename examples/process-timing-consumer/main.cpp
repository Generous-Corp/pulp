// Installed-SDK consumer for neutral adapter timing facts.
//
// A host binding (or a Processor that owns an event timeline) reads only
// `ProcessContext::timing` and the block-relative MIDI offsets an adapter
// delivers, advances one
// `HostBlockTimeline`, and stages each event in a `HostEventIngress` at its
// epoch-local absolute sample. The contexts below are built with the same
// public `format::*_process_timing()` builders the format adapters use, so the
// consumer needs no plugin SDK.

#include <pulp/format/process_context.hpp>
#include <pulp/format/process_timing.hpp>
#include <pulp/host/graph_event_contract.hpp>
#include <pulp/host/host_event_timestamps.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <vector>

namespace {

using pulp::format::ProcessContext;
using pulp::host::HostBlockBoundary;
using pulp::host::HostBlockClock;
using pulp::host::HostBlockTimeline;
using pulp::host::HostBlockWindow;
using pulp::host::HostEventIngress;
using pulp::host::HostEventRecord;
using pulp::host::HostEventResult;

int checks = 0;

void check(bool condition, const char* rule) {
    ++checks;
    if (!condition)
        throw std::runtime_error(rule);
}

// The per-block step a timeline-owning binding runs: place the block, then
// stage the delivered events. Fixed-capacity and allocation-free in process.
class TimelineOwner {
  public:
    struct Result {
        HostEventResult advance = HostEventResult::Accepted;
        HostBlockBoundary boundary = HostBlockBoundary::First;
        HostBlockWindow window;
        std::array<HostEventResult, 8> events{};
        std::array<std::int64_t, 8> projected{};
        std::size_t projected_count = 0;
    };

    Result block(const ProcessContext& ctx, const std::vector<std::int32_t>& offsets) {
        Result result;
        result.advance = pulp::host::advance_from_context(timeline_, ctx, result.window);
        if (result.advance != HostEventResult::Accepted)
            return result;
        result.boundary = timeline_.last_boundary();
        if (ingress_.begin_block(result.window.epoch, result.window.sample_start,
                                 result.window.frames) != HostEventResult::Accepted)
            throw std::runtime_error("ingress refused the window");
        for (std::size_t i = 0; i < offsets.size() && i < result.events.size(); ++i) {
            HostEventRecord record;
            auto status = pulp::host::make_record_from_block_offset(
                timeline_, result.window, offsets[i], 1, ++sequence_,
                pulp::midi::MidiEvent::note_on(0, 60, 100), record);
            if (status == HostEventResult::Accepted)
                status = ingress_.publish(record);
            result.events[i] = status;
        }
        std::array<HostEventRecord, 8> out{};
        if (ingress_.project(out, result.projected_count) != HostEventResult::Accepted)
            throw std::runtime_error("projection refused");
        for (std::size_t i = 0; i < result.projected_count; ++i)
            result.projected[i] = out[i].absolute_sample;
        ingress_.end_block();
        return result;
    }

    const HostBlockTimeline& timeline() const { return timeline_; }
    HostEventIngress<16>& ingress() { return ingress_; }

  private:
    HostBlockTimeline timeline_;
    HostEventIngress<16> ingress_;
    std::uint64_t sequence_ = 0;
};

ProcessContext clap_block(std::int64_t steady, std::optional<std::int64_t> transport, bool playing,
                          int frames, bool discontinuity = false) {
    ProcessContext ctx;
    ctx.sample_rate = 48000.0;
    ctx.num_samples = frames;
    ctx.timing = pulp::format::clap_process_timing(steady, transport, playing, discontinuity);
    return ctx;
}

void activation_seek_gap_and_stop() {
    TimelineOwner owner;
    auto r = owner.block(clap_block(0, 1000, true, 64, true), {0, 63});
    check(r.boundary == HostBlockBoundary::First, "first block opens the timeline");
    check(r.projected_count == 2 && r.projected[0] == 0 && r.projected[1] == 63,
          "first-block events project at their offsets");
    r = owner.block(clap_block(64, 1064, true, 64), {5});
    check(r.boundary == HostBlockBoundary::Continuous, "contiguous block continues");
    check(r.projected_count == 1 && r.projected[0] == 69, "continuous event is absolute");
    r = owner.block(clap_block(128, 9000, true, 64), {5});
    check(r.boundary == HostBlockBoundary::NewEpoch, "playing seek opens a new epoch");
    check(r.window.sample_start == 0 && r.projected[0] == 5, "new epoch restarts at zero");
    r = owner.block(clap_block(192, 9064, false, 64), {});
    check(r.boundary == HostBlockBoundary::Continuous, "stopping is not a seek");
    r = owner.block(clap_block(256, 100, false, 64), {});
    check(r.boundary == HostBlockBoundary::Continuous, "relocating while stopped is not a seek");
    r = owner.block(clap_block(1000, 100, false, 64), {1});
    check(r.boundary == HostBlockBoundary::Gap, "skipped host blocks are a gap");
    check(r.window.sample_start == 1000 - 128, "a gap advances the window by the skip");
    r = owner.block(clap_block(-1, 100, false, 64), {});
    check(r.boundary == HostBlockBoundary::NewEpoch, "losing the counter is a clock change");
}

void invalid_offsets_and_stale_epochs() {
    TimelineOwner owner;
    auto r = owner.block(clap_block(0, 0, true, 32), {-1, 0, 31, 32});
    check(r.events[0] == HostEventResult::OutOfRange, "negative offset refused");
    check(r.events[1] == HostEventResult::Accepted, "first frame accepted");
    check(r.events[2] == HostEventResult::Accepted, "last frame accepted");
    check(r.events[3] == HostEventResult::OutOfRange, "offset at the block end refused");
    check(r.projected_count == 2, "refused offsets are never clamped into the block");
    const auto old_window = r.window;
    HostEventRecord stale;
    check(owner.timeline().make_record(old_window, 3, 1, 1000,
                                       pulp::midi::MidiEvent::note_on(0, 61, 1),
                                       stale) == HostEventResult::Accepted,
          "record on the current window");
    r = owner.block(clap_block(32, 50000, true, 32), {});
    check(r.boundary == HostBlockBoundary::NewEpoch, "seek before the stale publication");
    std::int64_t absolute = 0;
    check(owner.timeline().project(old_window, 3, absolute) == HostEventResult::StaleEpoch,
          "an old window is stale after a new epoch");
    check(owner.ingress().begin_block(r.window.epoch, r.window.sample_start + 32, 32) ==
              HostEventResult::Accepted,
          "next ingress window");
    check(owner.ingress().publish(stale) == HostEventResult::StaleEpoch,
          "the ingress refuses a retired-epoch record");
    owner.ingress().end_block();
}

void partitions_and_varispeed() {
    const std::array<std::int64_t, 5> events{0, 31, 32, 100, 191};
    const std::vector<std::vector<int>> partitions{{192}, {32, 32, 128}, {1, 63, 64, 64}};
    std::vector<std::int64_t> first;
    for (const auto& partition : partitions) {
        TimelineOwner owner;
        std::vector<std::int64_t> projected;
        std::int64_t start = 0;
        for (const int frames : partition) {
            std::vector<std::int32_t> offsets;
            for (const auto e : events)
                if (e >= start && e < start + frames)
                    offsets.push_back(static_cast<std::int32_t>(e - start));
            const auto r = owner.block(clap_block(500 + start, 500 + start, true, frames), offsets);
            projected.insert(projected.end(), r.projected.begin(),
                             r.projected.begin() + static_cast<std::ptrdiff_t>(r.projected_count));
            start += frames;
        }
        if (first.empty())
            first = projected;
        check(projected == first, "every partition projects the same absolute samples");
    }
    check(first == std::vector<std::int64_t>(events.begin(), events.end()),
          "partition projection equals the host samples");

    // LV2 at half speed: the transport moves half a block per block.
    TimelineOwner owner;
    std::int64_t frame = 0;
    bool continuous = true;
    for (int i = 0; i < 4; ++i) {
        ProcessContext ctx;
        ctx.num_samples = 64;
        ctx.timing = pulp::format::lv2_process_timing(true, frame, true, 0.5, i == 0);
        const auto r = owner.block(ctx, {});
        if (i > 0)
            continuous = continuous && r.boundary == HostBlockBoundary::Continuous;
        frame += 32;
    }
    check(continuous, "non-unit LV2 speed is not read as a seek");
}

void oversized_blocks_are_refused_or_chunked() {
    TimelineOwner owner;
    auto r = owner.block(clap_block(0, 0, true, 6000), {});
    check(r.advance == HostEventResult::OutOfRange, "a block above 4096 frames is refused");
    HostBlockClock whole;
    whole.steady_sample = 0;
    whole.frames = 6000;
    whole.has_transport_sample = true;
    whole.transport_playing = true;
    HostBlockTimeline timeline;
    std::size_t offset = 0;
    std::vector<HostBlockBoundary> boundaries;
    while (offset < whole.frames) {
        const auto frames = std::min<std::size_t>(pulp::host::kHostBlockMaxFrames,
                                                  whole.frames - offset);
        HostBlockClock chunk;
        check(pulp::host::host_block_chunk(whole, offset, frames, chunk) ==
                  HostEventResult::Accepted,
              "chunk accepted");
        HostBlockWindow window;
        check(timeline.advance(chunk, window) == HostEventResult::Accepted, "chunk placed");
        check(window.sample_start == offset, "chunks are contiguous");
        boundaries.push_back(timeline.last_boundary());
        offset += frames;
    }
    check(boundaries.size() == 2 && boundaries[1] == HostBlockBoundary::Continuous,
          "the second chunk continues the first");
}

} // namespace

int main() {
    try {
        activation_seek_gap_and_stop();
        invalid_offsets_and_stale_epochs();
        partitions_and_varispeed();
        oversized_blocks_are_refused_or_chunked();
    } catch (const std::exception& error) {
        std::cout << "process-timing-consumer FAIL " << error.what() << " after checks=" << checks
                  << "\n";
        return 1;
    }
    std::cout << "process-timing-consumer PASS checks=" << checks << "\n";
    return 0;
}
