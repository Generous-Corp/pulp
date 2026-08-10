#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <pulp/inspect/control_state_write_executor.hpp>

#include <choc/text/choc_JSON.h>

#include <stdexcept>

using namespace pulp::inspect;
namespace state = pulp::state;

namespace {

ControlAdmissionPlan plan() {
    ControlAdmissionPlan value;
    value.registration_id = ControlRegistrationId{"registration-1"};
    value.receipt_id = ControlReceiptId{"receipt-1"};
    value.expected_state_generation = 4;
    return value;
}

ControlRequestEnvelope request() {
    return {.registration_id = "registration-1",
            .operation_id = "dev.pulp.state/parameter-gesture@1",
            .operation_version = 1,
            .expected_state_generation = 4,
            .params_json =
                R"({"idempotency_key":"once","normalized_value":0.75,"parameter_id":7})"};
}

ControlExecutionContext context() {
    return {.checkpoint = [] { return ControlExecutionCheckpoint::Continue; }};
}

void seed_generation(state::StateStore& store, std::uint64_t generation) {
    while (store.state_generation() < generation)
        store.set_value(7, store.get_value(7));
}

ControlStateWriteTarget target(state::StateStore& store,
                               ControlRegistrationId registration =
                                   ControlRegistrationId{"registration-1"}) {
    return {
        .registration_id = std::move(registration),
        .host_tier = ControlHostTier::Standalone,
        .store = &store,
        .state_generation = store.state_generation(),
    };
}

} // namespace

TEST_CASE("canonical T1 gesture uses StateStore generation and acknowledges the commit",
          "[inspect][control][mutation][t1][main-thread]") {
    state::StateStore store;
    store.add_parameter({.id = 7, .name = "Mix", .range = {0.0f, 1.0f, 0.0f, 0.0f}});
    seed_generation(store, 4);
    int begins = 0;
    int ends = 0;
    bool snapshot_current_during_begin = true;
    store.set_gesture_callbacks(
        [&](state::ParamID id) {
            begins += id == 7 ? 1 : 0;
            snapshot_current_during_begin = store.state_snapshot_is_current(5);
        },
        [&](state::ParamID id) { ends += id == 7 ? 1 : 0; });
    auto executor = make_control_state_write_executor(
        [&](const ControlAdmissionPlan&) { return std::optional{target(store)}; });

    const auto outcome = executor(plan(), request(), context());
    REQUIRE(outcome.terminal_state == ControlReceiptState::Completed);
    CHECK(store.get_normalized(7) == Catch::Approx(0.75f));
    CHECK(begins == 1);
    CHECK(ends == 1);
    CHECK_FALSE(snapshot_current_during_begin);
    CHECK(store.open_gesture_count() == 0);
    CHECK(store.state_generation() == 5);
    const auto detail = choc::json::parse(outcome.result.detail_json);
    CHECK(detail["applied"].getWithDefault(false));
    CHECK(detail["receipt_id"].getString() == "receipt-1");
    CHECK(detail["state_generation"].getWithDefault<std::int64_t>(0) == 5);
}

TEST_CASE("T1 gesture shares UI ownership and rolls back a throwing begin callback",
          "[inspect][control][mutation][t1][gesture][race]") {
    state::StateStore store;
    store.add_parameter({.id = 7, .name = "Mix", .range = {0.0f, 1.0f, 0.0f, 0.0f}});
    seed_generation(store, 4);
    int begins = 0;
    int ends = 0;
    store.set_gesture_callbacks([&](state::ParamID) { ++begins; },
                                [&](state::ParamID) { ++ends; });
    store.begin_gesture(7);
    auto executor = make_control_state_write_executor(
        [&](const ControlAdmissionPlan&) { return std::optional{target(store)}; });
    CHECK(executor(plan(), request(), context()).terminal_state ==
          ControlReceiptState::Completed);
    CHECK(store.open_gesture_count() == 1);
    CHECK(begins == 1);
    CHECK(ends == 0);
    store.end_gesture(7);
    CHECK(ends == 1);

    state::StateStore throwing;
    throwing.add_parameter({.id = 7, .name = "Mix", .range = {0.0f, 1.0f, 0.0f, 0.0f}});
    seed_generation(throwing, 4);
    throwing.set_gesture_callbacks([](state::ParamID) { throw std::runtime_error("host"); },
                                   [](state::ParamID) {});
    auto throwing_executor = make_control_state_write_executor(
        [&](const ControlAdmissionPlan&) { return std::optional{target(throwing)}; });
    CHECK(throwing_executor(plan(), request(), context()).terminal_state ==
          ControlReceiptState::UnknownNeedsRefresh);
    CHECK(throwing.get_normalized(7) == Catch::Approx(0.0f));
    CHECK(throwing.open_gesture_count() == 0);
}

TEST_CASE("host automation and stale exact targets fail before gesture side effects",
          "[inspect][control][mutation][t1][t2a][race]") {
    state::StateStore store;
    store.add_parameter({.id = 7, .name = "Mix", .range = {0.0f, 1.0f, 0.0f, 0.0f}});
    seed_generation(store, 4);
    int begins = 0;
    store.set_gesture_callbacks([&](state::ParamID) { ++begins; }, [](state::ParamID) {});

    const auto resolved = target(store);
    store.set_value_rt(7, 0.5f); // automation advances after exact resolution
    auto executor = make_control_state_write_executor(
        [&](const ControlAdmissionPlan&) { return std::optional{resolved}; });
    const auto outcome = executor(plan(), request(), context());
    CHECK(outcome.result.result_code == ControlResultCode::StateConflict);
    CHECK(begins == 0);
    CHECK(store.get_normalized(7) == Catch::Approx(0.5f));

    auto wrong_registration = make_control_state_write_executor([&](const ControlAdmissionPlan&) {
        return std::optional{target(store, ControlRegistrationId{"other-slot"})};
    });
    CHECK(wrong_registration(plan(), request(), context()).result.result_code ==
          ControlResultCode::HostUnavailable);

    auto wrong_request = request();
    wrong_request.registration_id = "other-slot";
    CHECK(executor(plan(), wrong_request, context()).result.result_code ==
          ControlResultCode::InvalidRequest);

    wrong_request = request();
    wrong_request.expected_state_generation = 3;
    CHECK(executor(plan(), wrong_request, context()).result.result_code ==
          ControlResultCode::InvalidRequest);
}

TEST_CASE("writer arriving inside the claimed gesture is preserved and requires refresh",
          "[inspect][control][mutation][t1][automation][race][rollback]") {
    state::StateStore store;
    store.add_parameter({.id = 7, .name = "Mix", .range = {0.0f, 1.0f, 0.0f, 0.0f}});
    seed_generation(store, 4);
    int begins = 0;
    int ends = 0;
    store.set_gesture_callbacks(
        [&](state::ParamID) {
            ++begins;
            store.set_value_rt(7, 0.5f); // deterministic automation race after reservation
        },
        [&](state::ParamID) { ++ends; });
    auto executor = make_control_state_write_executor(
        [&](const ControlAdmissionPlan&) { return std::optional{target(store)}; });

    const auto outcome = executor(plan(), request(), context());
    CHECK(outcome.terminal_state == ControlReceiptState::UnknownNeedsRefresh);
    CHECK(store.get_normalized(7) == Catch::Approx(0.5f));
    CHECK(store.state_generation() == 6);
    CHECK(begins == 1);
    CHECK(ends == 1);
    CHECK(store.open_gesture_count() == 0);

    SECTION("an identical newer value is not mistaken for the control write") {
        state::StateStore identical;
        identical.add_parameter(
            {.id = 7, .name = "Mix", .range = {0.0f, 1.0f, 0.0f, 0.0f}});
        seed_generation(identical, 4);
        identical.set_gesture_callbacks(
            [](state::ParamID) {},
            [&](state::ParamID) { identical.set_value_rt(7, 0.75f); });
        auto identical_executor = make_control_state_write_executor(
            [&](const ControlAdmissionPlan&) { return std::optional{target(identical)}; });

        const auto identical_outcome = identical_executor(plan(), request(), context());
        CHECK(identical_outcome.terminal_state ==
              ControlReceiptState::UnknownNeedsRefresh);
        CHECK(identical.get_normalized(7) == Catch::Approx(0.75f));
        CHECK(identical.state_generation() == 6);
    }
}
