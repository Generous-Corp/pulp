#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/signal/units.hpp>

#include <array>

using Catch::Matchers::WithinAbs;

namespace {

struct DivisionParityRow {
    pulp::signal::units::Division signal;
    pulp::timebase::BeatDivision timebase;
};

constexpr std::array<DivisionParityRow, 21> kDivisionParityRows{{
    {pulp::signal::units::Division::whole, pulp::timebase::BeatDivision::Whole},
    {pulp::signal::units::Division::whole_dotted, pulp::timebase::BeatDivision::WholeDotted},
    {pulp::signal::units::Division::whole_triplet, pulp::timebase::BeatDivision::WholeTriplet},
    {pulp::signal::units::Division::half, pulp::timebase::BeatDivision::Half},
    {pulp::signal::units::Division::half_dotted, pulp::timebase::BeatDivision::HalfDotted},
    {pulp::signal::units::Division::half_triplet, pulp::timebase::BeatDivision::HalfTriplet},
    {pulp::signal::units::Division::quarter, pulp::timebase::BeatDivision::Quarter},
    {pulp::signal::units::Division::quarter_dotted, pulp::timebase::BeatDivision::QuarterDotted},
    {pulp::signal::units::Division::quarter_triplet, pulp::timebase::BeatDivision::QuarterTriplet},
    {pulp::signal::units::Division::eighth, pulp::timebase::BeatDivision::Eighth},
    {pulp::signal::units::Division::eighth_dotted, pulp::timebase::BeatDivision::EighthDotted},
    {pulp::signal::units::Division::eighth_triplet, pulp::timebase::BeatDivision::EighthTriplet},
    {pulp::signal::units::Division::sixteenth, pulp::timebase::BeatDivision::Sixteenth},
    {pulp::signal::units::Division::sixteenth_dotted, pulp::timebase::BeatDivision::SixteenthDotted},
    {pulp::signal::units::Division::sixteenth_triplet, pulp::timebase::BeatDivision::SixteenthTriplet},
    {pulp::signal::units::Division::thirty_second, pulp::timebase::BeatDivision::ThirtySecond},
    {pulp::signal::units::Division::thirty_second_dotted, pulp::timebase::BeatDivision::ThirtySecondDotted},
    {pulp::signal::units::Division::thirty_second_triplet, pulp::timebase::BeatDivision::ThirtySecondTriplet},
    {pulp::signal::units::Division::sixty_fourth, pulp::timebase::BeatDivision::SixtyFourth},
    {pulp::signal::units::Division::sixty_fourth_dotted, pulp::timebase::BeatDivision::SixtyFourthDotted},
    {pulp::signal::units::Division::sixty_fourth_triplet, pulp::timebase::BeatDivision::SixtyFourthTriplet},
}};

constexpr bool division_vocabulary_has_compile_time_parity() {
    if (kDivisionParityRows.size() !=
        static_cast<std::size_t>(pulp::signal::units::Division::count))
        return false;
    for (const auto& row : kDivisionParityRows) {
        if (pulp::signal::units::to_beat_division(row.signal) != row.timebase)
            return false;
        const auto fraction = pulp::timebase::beat_fraction_or(row.timebase, {0, 1});
        if (pulp::signal::units::division_to_beats(row.signal) !=
            static_cast<float>(fraction.numerator) / static_cast<float>(fraction.denominator))
            return false;
    }
    return true;
}

static_assert(division_vocabulary_has_compile_time_parity());

} // namespace

TEST_CASE("Musical division table is order-locked and correct", "[signal][units]") {
    using namespace pulp::signal::units;

    REQUIRE_THAT(division_to_beats(Division::whole), WithinAbs(4.0f, 1e-6f));
    REQUIRE_THAT(division_to_beats(Division::half), WithinAbs(2.0f, 1e-6f));
    REQUIRE_THAT(division_to_beats(Division::quarter), WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(division_to_beats(Division::eighth), WithinAbs(0.5f, 1e-6f));
    REQUIRE_THAT(division_to_beats(Division::sixteenth), WithinAbs(0.25f, 1e-6f));

    REQUIRE_THAT(division_to_beats(Division::quarter_dotted), WithinAbs(1.5f, 1e-6f));
    REQUIRE_THAT(division_to_beats(Division::eighth_triplet), WithinAbs(1.0f / 3.0f, 1e-6f));
    REQUIRE_THAT(3.0f * division_to_beats(Division::eighth_triplet), WithinAbs(1.0f, 1e-5f));

    for (const auto& row : kDivisionParityRows) {
        const auto fraction = pulp::timebase::beat_fraction(row.timebase);
        REQUIRE(fraction);
        REQUIRE(to_beat_division(row.signal) == row.timebase);
        REQUIRE(division_to_beats(row.signal) ==
                static_cast<float>(fraction->numerator) /
                    static_cast<float>(fraction->denominator));
    }

    const auto invalid = static_cast<Division>(static_cast<std::uint8_t>(Division::count));
    REQUIRE(to_beat_division(invalid) == pulp::timebase::BeatDivision::Count);
    REQUIRE(division_to_beats(invalid) == 1.0f);

    for (int i = 0; i < kDivisionCount; ++i)
        REQUIRE(division_to_beats(i) == division_to_beats(static_cast<Division>(i)));

    REQUIRE(division_to_beats(Division::sixty_fourth) < division_to_beats(Division::thirty_second));
    REQUIRE_THAT(division_to_samples(Division::quarter, 120.0f, 48000.0), WithinAbs(24000.0, 1e-3));
}
