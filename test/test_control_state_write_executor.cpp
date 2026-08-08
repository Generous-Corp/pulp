#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <pulp/inspect/control_state_write_executor.hpp>

#include <choc/text/choc_JSON.h>

#include <atomic>
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

} // namespace

TEST_CASE("canonical T1 gesture brackets apply and acknowledges the advanced generation",
          "[inspect][control][mutation][t1][main-thread]") {
    state::StateStore store;
    store.add_parameter({.id = 7, .name = "Mix", .range = {0.0f, 1.0f, 0.0f, 0.0f}});
    std::atomic<std::uint64_t> generation{4};
    int begins = 0;
    int ends = 0;
    store.set_gesture_callbacks(
        [&](state::ParamID id) {
            if (id == 7)
                ++begins;
        },
        [&](state::ParamID id) {
            if (id == 7)
                ++ends;
        });
    auto executor = make_control_state_write_executor([&](const ControlAdmissionPlan&) {
        return std::optional{ControlStateWriteTarget{
            .registration_id = ControlRegistrationId{"registration-1"},
            .host_tier = ControlHostTier::Standalone,
            .store = &store,
            .state_generation = generation.load(),
            .current_state_generation = [&] { return generation.load(); },
            .apply_if_state_generation =
                [&](std::uint64_t expected,
                    const std::function<void()>& mutation) -> std::optional<std::uint64_t> {
                if (generation.load() != expected)
                    return std::nullopt;
                mutation();
                generation = expected + 1;
                return expected + 1;
            },
        }};
    });

    const auto outcome = executor(plan(), request(), context());
    REQUIRE(outcome.terminal_state == ControlReceiptState::Completed);
    CHECK(store.get_normalized(7) == Catch::Approx(0.75f));
    CHECK(begins == 1);
    CHECK(ends == 1);
    CHECK(store.open_gesture_count() == 0);
    CHECK(generation == 5);
    const auto detail = choc::json::parse(outcome.result.detail_json);
    CHECK(detail["applied"].getWithDefault(false));
    CHECK(detail["receipt_id"].getString() == "receipt-1");
    CHECK(detail["state_generation"].getWithDefault<std::int64_t>(0) == 5);
}

TEST_CASE("T1 gesture shares UI ownership and rolls back a throwing begin callback",
          "[inspect][control][mutation][t1][gesture][race]") {
    state::StateStore store;
    store.add_parameter({.id = 7, .name = "Mix", .range = {0.0f, 1.0f, 0.0f, 0.0f}});
    std::atomic<std::uint64_t> generation{4};
    int begins = 0;
    int ends = 0;
    store.set_gesture_callbacks([&](state::ParamID) { ++begins; }, [&](state::ParamID) { ++ends; });
    store.begin_gesture(7);
    auto executor = make_control_state_write_executor([&](const ControlAdmissionPlan&) {
        return std::optional{ControlStateWriteTarget{
            .registration_id = ControlRegistrationId{"registration-1"},
            .host_tier = ControlHostTier::Standalone,
            .store = &store,
            .state_generation = generation.load(),
            .current_state_generation = [&] { return generation.load(); },
            .apply_if_state_generation =
                [&](std::uint64_t expected,
                    const std::function<void()>& mutation) -> std::optional<std::uint64_t> {
                if (generation.load() != expected)
                    return std::nullopt;
                mutation();
                generation = expected + 1;
                return expected + 1;
            },
        }};
    });
    CHECK(executor(plan(), request(), context()).terminal_state == ControlReceiptState::Completed);
    CHECK(store.open_gesture_count() == 1);
    CHECK(begins == 1);
    CHECK(ends == 0);
    store.end_gesture(7);
    CHECK(ends == 1);

    state::StateStore throwing;
    throwing.add_parameter({.id = 7, .name = "Mix", .range = {0.0f, 1.0f, 0.0f, 0.0f}});
    throwing.set_gesture_callbacks([](state::ParamID) { throw std::runtime_error("host"); },
                                   [](state::ParamID) {});
    generation = 4;
    auto throwing_executor = make_control_state_write_executor([&](const ControlAdmissionPlan&) {
        return std::optional{ControlStateWriteTarget{
            .registration_id = ControlRegistrationId{"registration-1"},
            .host_tier = ControlHostTier::Standalone,
            .store = &throwing,
            .state_generation = generation.load(),
            .current_state_generation = [&] { return generation.load(); },
            .apply_if_state_generation =
                [&](std::uint64_t expected,
                    const std::function<void()>& mutation) -> std::optional<std::uint64_t> {
                if (generation.load() != expected)
                    return std::nullopt;
                mutation();
                generation = expected + 1;
                return expected + 1;
            },
        }};
    });
    CHECK(throwing_executor(plan(), request(), context()).terminal_state ==
          ControlReceiptState::UnknownNeedsRefresh);
    CHECK(throwing.open_gesture_count() == 0);
}

TEST_CASE("host automation race and stale target fail before gesture side effects",
          "[inspect][control][mutation][t1][t2a][race]") {
    state::StateStore store;
    store.add_parameter({.id = 7, .name = "Mix", .range = {0.0f, 1.0f, 0.0f, 0.0f}});
    std::atomic<std::uint64_t> generation{5}; // automation advanced after admission
    int begins = 0;
    store.set_gesture_callbacks([&](state::ParamID) { ++begins; }, [](state::ParamID) {});
    auto executor = make_control_state_write_executor([&](const ControlAdmissionPlan&) {
        return std::optional{ControlStateWriteTarget{
            .registration_id = ControlRegistrationId{"registration-1"},
            .host_tier = ControlHostTier::SharedPluginHost,
            .store = &store,
            .state_generation = generation.load(),
            .current_state_generation = [&] { return generation.load(); },
            .apply_if_state_generation =
                [&](std::uint64_t expected,
                    const std::function<void()>& mutation) -> std::optional<std::uint64_t> {
                if (generation.load() != expected)
                    return std::nullopt;
                mutation();
                generation = expected + 1;
                return expected + 1;
            },
        }};
    });
    const auto outcome = executor(plan(), request(), context());
    CHECK(outcome.result.result_code == ControlResultCode::StateConflict);
    CHECK(begins == 0);
    CHECK(store.get_normalized(7) == Catch::Approx(0.0f));

    auto wrong_registration = make_control_state_write_executor([&](const ControlAdmissionPlan&) {
        return std::optional{ControlStateWriteTarget{
            .registration_id = ControlRegistrationId{"other-slot"},
            .host_tier = ControlHostTier::SharedPluginHost,
            .store = &store,
            .state_generation = generation.load(),
            .current_state_generation = [&] { return generation.load(); },
            .apply_if_state_generation =
                [&](std::uint64_t expected,
                    const std::function<void()>& mutation) -> std::optional<std::uint64_t> {
                if (generation.load() != expected)
                    return std::nullopt;
                mutation();
                generation = expected + 1;
                return expected + 1;
            },
        }};
    });
    CHECK(wrong_registration(plan(), request(), context()).result.result_code ==
          ControlResultCode::HostUnavailable);
}

TEST_CASE("generation reservation closes the race immediately before the gesture",
          "[inspect][control][mutation][t1][automation][race]") {
    state::StateStore store;
    store.add_parameter({.id = 7, .name = "Mix", .range = {0.0f, 1.0f, 0.0f, 0.0f}});
    int begins = 0;
    store.set_gesture_callbacks([&](state::ParamID) { ++begins; }, [](state::ParamID) {});
    auto executor = make_control_state_write_executor([&](const ControlAdmissionPlan&) {
        return std::optional{ControlStateWriteTarget{
            .registration_id = ControlRegistrationId{"registration-1"},
            .host_tier = ControlHostTier::Standalone,
            .store = &store,
            .state_generation = 4,
            .current_state_generation = [] { return std::uint64_t{4}; },
            .apply_if_state_generation = [](std::uint64_t, const std::function<void()>&)
                -> std::optional<std::uint64_t> { return std::nullopt; },
        }};
    });
    const auto outcome = executor(plan(), request(), context());
    CHECK(outcome.result.result_code == ControlResultCode::StateConflict);
    CHECK(begins == 0);
    CHECK(store.get_normalized(7) == Catch::Approx(0.0f));
}
