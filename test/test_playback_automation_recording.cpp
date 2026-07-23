#include "harness/scoped_rt_process_probe.hpp"

#include <pulp/playback/automation_recording.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace pulp;
using namespace pulp::playback;
using namespace pulp::timebase;
using namespace pulp::timeline;

TEST_CASE("touch automation captures the gesture and one return point") {
    AutomationRecorder recorder;
    REQUIRE(recorder.prepare(8));
    REQUIRE(recorder.begin(AutomationRecordMode::Touch));

    std::size_t allocations = 0;
    {
        test::ScopedRtProcessProbe probe;
        REQUIRE(recorder.record({0}, 0.1f, false) == AutomationRecordError::None);
        REQUIRE(recorder.record({1}, 0.5f, true) == AutomationRecordError::None);
        REQUIRE(recorder.record({2}, 0.6f, true) == AutomationRecordError::None);
        REQUIRE(recorder.record({3}, 0.2f, false) == AutomationRecordError::None);
        REQUIRE(recorder.record({4}, 0.3f, false) == AutomationRecordError::None);
        allocations = probe.allocation_count();
    }
    REQUIRE(allocations == 0);
    REQUIRE(recorder.end());
    REQUIRE(recorder.points().size() == 3);
    REQUIRE(recorder.points()[0] == RecordedAutomationPoint{{1}, 0.5f});
    REQUIRE(recorder.points()[1] == RecordedAutomationPoint{{2}, 0.6f});
    REQUIRE(recorder.points()[2] == RecordedAutomationPoint{{3}, 0.2f});

    auto curve = recorder.materialize_curve(100);
    REQUIRE(curve);
    REQUIRE(curve->curve.points().size() == 3);
    REQUIRE(curve->curve.points()[0].id == ItemId{100});
    REQUIRE(curve->curve.points()[2].value == 0.2f);
    REQUIRE(curve->next_item_id == 103);
}

TEST_CASE("latch automation holds the last touched value until recording stops") {
    AutomationRecorder recorder;
    REQUIRE(recorder.prepare(8));
    REQUIRE(recorder.begin(AutomationRecordMode::Latch));
    REQUIRE(recorder.record({0}, 0.1f, false) == AutomationRecordError::None);
    REQUIRE(recorder.record({1}, 0.7f, true) == AutomationRecordError::None);
    REQUIRE(recorder.record({2}, 0.2f, false) == AutomationRecordError::None);
    REQUIRE(recorder.record({3}, 0.3f, false) == AutomationRecordError::None);
    REQUIRE(recorder.end());

    REQUIRE(recorder.points().size() == 3);
    REQUIRE(recorder.points()[0].value == 0.7f);
    REQUIRE(recorder.points()[1].value == 0.7f);
    REQUIRE(recorder.points()[2].value == 0.7f);
}

TEST_CASE("write automation captures continuously and fails closed at capacity") {
    AutomationRecorder recorder;
    REQUIRE(recorder.prepare(2));
    REQUIRE(recorder.begin(AutomationRecordMode::Write));
    REQUIRE(recorder.record({0}, 0.1f, false) == AutomationRecordError::None);
    REQUIRE(recorder.record({0}, 0.2f, false) == AutomationRecordError::None);
    REQUIRE(recorder.record({1}, 0.3f, false) == AutomationRecordError::None);
    REQUIRE(recorder.record({2}, 0.4f, false) == AutomationRecordError::CapacityExceeded);
    REQUIRE(recorder.dropped_points() == 1);
    REQUIRE(recorder.points().size() == 2);
    REQUIRE(recorder.points()[0].value == 0.2f);
    REQUIRE(recorder.record({0}, 0.5f, false) == AutomationRecordError::NonMonotonicPosition);
}
