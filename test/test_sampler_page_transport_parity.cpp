#include <catch2/catch_test_macros.hpp>

#include "support/sample_page_transport_parity.hpp"
#include "support/sampler_parity.hpp"

#include <array>
#include <bit>
#include <cstdint>

namespace page_parity = pulp::test::sample_page_transport_parity;
namespace parity = pulp::test::sampler_parity;

namespace {

void require_complete_page_render(const page_parity::PageRenderCapture& capture,
                                  std::size_t output_frames) {
    REQUIRE(capture.copied_taps == output_frames);
    REQUIRE(capture.missed_taps == 0);
    REQUIRE(capture.zero_filled_taps == 0);
    REQUIRE(capture.callback_allocations == 0);
}

}  // namespace

TEST_CASE("Prepared pages match resident integer taps across forward ratios and seeks",
          "[audio][sampler][stream-window][parity][forward]") {
    constexpr std::uint64_t page_frames = 257;
    constexpr std::uint64_t total_frames = page_frames * 4 + 73;
    constexpr std::uint64_t generation = 41;
    constexpr std::array segments = {
        page_parity::IntegerTapSegment{page_frames - 2, 1, 5},
        page_parity::IntegerTapSegment{page_frames * 2 - 3, 2, 5},
        page_parity::IntegerTapSegment{0, 3, 7},
        page_parity::IntegerTapSegment{page_frames * 4 + 5, 1, 13},
    };

    auto positions = page_parity::make_integer_tap_schedule(total_frames, segments);
    REQUIRE(positions.has_value());
    REQUIRE((*positions)[0] == page_frames - 2);
    REQUIRE((*positions)[2] == page_frames);
    REQUIRE((*positions)[5] == page_frames * 2 - 3);
    REQUIRE((*positions)[6] == page_frames * 2 - 1);
    REQUIRE((*positions)[10] == 0);

    auto source = parity::make_deterministic_source(2, total_frames);
    auto resident = page_parity::render_resident_integer_taps(source, *positions);
    page_parity::PreparedPageTransport prepared;
    REQUIRE(prepared.prepare(source, page_frames, generation));
    REQUIRE(prepared.page_count() == 5);

    const auto before_boundary = prepared.window().ready_page_for_frame(
        generation, page_frames - 1);
    const auto at_boundary = prepared.window().ready_page_for_frame(
        generation, page_frames);
    REQUIRE(before_boundary.valid);
    REQUIRE(at_boundary.valid);
    REQUIRE(before_boundary.page_index == 0);
    REQUIRE(at_boundary.page_index == 1);
    REQUIRE(before_boundary.local_offset == page_frames - 1);
    REQUIRE(at_boundary.local_offset == 0);

    auto paged = prepared.render_integer_taps(*positions, generation);
    require_complete_page_render(paged, positions->size());
    auto comparison = parity::compare_raw_float_bits(resident, paged.output);
    REQUIRE(comparison.equal_nonvacuous());
    REQUIRE(prepared.window().stats().ready_frames_read == positions->size());
}

TEST_CASE("Prepared pages match resident integer taps across reverse ratios and seeks",
          "[audio][sampler][stream-window][parity][reverse]") {
    constexpr std::uint64_t page_frames = 257;
    constexpr std::uint64_t total_frames = page_frames * 4 + 73;
    constexpr std::uint64_t generation = 42;
    constexpr std::array segments = {
        page_parity::IntegerTapSegment{page_frames + 2, -1, 5},
        page_parity::IntegerTapSegment{page_frames * 3 + 4, -2, 6},
        page_parity::IntegerTapSegment{page_frames * 4 + 20, -3, 7},
    };

    auto positions = page_parity::make_integer_tap_schedule(total_frames, segments);
    REQUIRE(positions.has_value());
    REQUIRE((*positions)[0] == page_frames + 2);
    REQUIRE((*positions)[2] == page_frames);
    REQUIRE((*positions)[4] == page_frames - 2);
    REQUIRE((*positions)[5] == page_frames * 3 + 4);
    REQUIRE((*positions)[11] == page_frames * 4 + 20);

    auto source = parity::make_deterministic_source(2, total_frames);
    auto resident = page_parity::render_resident_integer_taps(source, *positions);
    page_parity::PreparedPageTransport prepared;
    REQUIRE(prepared.prepare(source, page_frames, generation));

    auto paged = prepared.render_integer_taps(*positions, generation);
    require_complete_page_render(paged, positions->size());
    REQUIRE(parity::compare_raw_float_bits(resident, paged.output)
                .equal_nonvacuous());

    constexpr std::array underflow = {
        page_parity::IntegerTapSegment{1, -2, 2},
    };
    REQUIRE_FALSE(page_parity::make_integer_tap_schedule(
                      total_frames, underflow).has_value());
}

TEST_CASE("Prepared page transport exposes stale and dropped-page misses before recovery",
          "[audio][sampler][stream-window][parity][recovery]") {
    constexpr std::uint64_t page_frames = 257;
    constexpr std::uint64_t total_frames = page_frames * 4 + 73;
    constexpr std::uint64_t generation = 43;
    auto source = parity::make_deterministic_source(2, total_frames);

    SECTION("stale stream generation cannot read current pages") {
        constexpr std::array segments = {
            page_parity::IntegerTapSegment{page_frames - 1, 1, 3},
            page_parity::IntegerTapSegment{page_frames * 3 + 2, -1, 3},
        };
        auto positions = page_parity::make_integer_tap_schedule(total_frames, segments);
        REQUIRE(positions.has_value());
        auto resident = page_parity::render_resident_integer_taps(source, *positions);

        page_parity::PreparedPageTransport prepared;
        REQUIRE(prepared.prepare(source, page_frames, generation));
        auto stale = prepared.render_integer_taps(*positions, generation + 1);
        REQUIRE(stale.copied_taps == 0);
        REQUIRE(stale.missed_taps == positions->size());
        REQUIRE(stale.zero_filled_taps == positions->size());
        REQUIRE(stale.callback_allocations == 0);
        auto stale_comparison = parity::compare_raw_float_bits(resident, stale.output);
        REQUIRE_FALSE(stale_comparison.equal());
        REQUIRE(stale_comparison.nonzero_expected);
        REQUIRE(stale_comparison.mismatch_count > 0);

        auto current = prepared.render_integer_taps(*positions, generation);
        require_complete_page_render(current, positions->size());
        REQUIRE(parity::compare_raw_float_bits(resident, current.output)
                    .equal_nonvacuous());
    }

    SECTION("publishing an omitted page recovers exact storage-coordinate taps") {
        constexpr std::uint32_t omitted_page = 2;
        constexpr std::array segments = {
            page_parity::IntegerTapSegment{page_frames * omitted_page - 2, 1, 7},
        };
        auto positions = page_parity::make_integer_tap_schedule(total_frames, segments);
        REQUIRE(positions.has_value());
        auto resident = page_parity::render_resident_integer_taps(source, *positions);

        page_parity::PreparedPageTransport prepared;
        REQUIRE(prepared.prepare(source, page_frames, generation, omitted_page));
        auto faulted = prepared.render_integer_taps(*positions, generation);
        REQUIRE(faulted.copied_taps == 2);
        REQUIRE(faulted.missed_taps == 5);
        REQUIRE(faulted.zero_filled_taps == 5);
        REQUIRE(faulted.callback_allocations == 0);
        auto fault_comparison = parity::compare_raw_float_bits(resident,
                                                               faulted.output);
        REQUIRE_FALSE(fault_comparison.equal());
        REQUIRE(fault_comparison.nonzero_expected);
        REQUIRE(fault_comparison.first_mismatch.has_value());
        REQUIRE(fault_comparison.first_mismatch->frame == 2);
        REQUIRE(fault_comparison.first_mismatch->actual_bits == 0u);

        REQUIRE(prepared.publish_page(omitted_page));
        auto recovered = prepared.render_integer_taps(*positions, generation);
        require_complete_page_render(recovered, positions->size());
        REQUIRE(parity::compare_raw_float_bits(resident, recovered.output)
                    .equal_nonvacuous());
    }
}
