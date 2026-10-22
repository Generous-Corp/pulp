#include "harness/fake_shared_io_provider.hpp"
#include "harness/rt_allocation_probe.hpp"
#include "harness/shared_io_provider_conformance.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <string>

namespace {

namespace conformance = pulp::test::shared_io_conformance;
using conformance::Fault;
using conformance::ProviderKind;
using conformance::ProviderUnderTest;
using conformance::Scenario;
using conformance::Verdict;
using pulp::test::FakeSharedIoProvider;

FakeSharedIoProvider::Fault fake_fault(Fault fault) {
    switch (fault) {
    case Fault::None:
        return FakeSharedIoProvider::Fault::None;
    case Fault::ForceLossBeforeSubmit:
        return FakeSharedIoProvider::Fault::ForceLossBeforeSubmit;
    case Fault::ForceLossBetweenSubmitAndCompletionRegistration:
        return FakeSharedIoProvider::Fault::ForceLossBetweenSubmitAndCompletionRegistration;
    case Fault::ForceLossAfterCompletionRegistration:
        return FakeSharedIoProvider::Fault::ForceLossAfterCompletionRegistration;
    case Fault::DelayCompletion:
        return FakeSharedIoProvider::Fault::DelayCompletion;
    case Fault::RejectBeforeSubmit:
        return FakeSharedIoProvider::Fault::RejectBeforeSubmit;
    case Fault::HoldTerminalBusy:
        return FakeSharedIoProvider::Fault::HoldTerminalBusy;
    }
    return FakeSharedIoProvider::Fault::None;
}

// The fake behaves like a GPU provider: work retires on service, a drain
// retires whatever is still in flight, and a clean drain may resume.
void configure(FakeSharedIoProvider& provider, const conformance::ProgramSpec& spec) {
    provider.transform = FakeSharedIoProvider::Transform::Affine;
    provider.scale = spec.scale;
    provider.bias = spec.bias;
    provider.complete_on_poll = true;
    provider.drain_completes = true;
    provider.resume_supported = true;
    provider.identity = FakeSharedIoProvider::authenticated_fake_identity();
    provider.capabilities.completion_service = true;
}

ProviderUnderTest
fake_provider(FakeSharedIoProvider::Violation violation = FakeSharedIoProvider::Violation::None,
              FakeSharedIoProvider::StorageKind storage =
                  FakeSharedIoProvider::StorageKind::ImportedHostPointer,
              std::string name = "fake") {
    ProviderUnderTest provider;
    provider.name = std::move(name);
    provider.kind = ProviderKind::Fake;
    provider.storage_kind = storage;
    provider.completion_timeout = std::chrono::milliseconds(500);
    provider.quiet_window = std::chrono::milliseconds(5);
    provider.make = [violation, storage](Fault fault) {
        auto fake = std::make_unique<FakeSharedIoProvider>();
        configure(*fake, conformance::ProgramSpec{});
        fake->storage_kind = storage;
        fake->fault = fake_fault(fault);
        fake->violation = violation;
        return conformance::CreateOutcome{std::move(fake), {}};
    };
    provider.busy_retries = [](const conformance::SharedIoArenaProvider& inner) {
        return static_cast<const FakeSharedIoProvider&>(inner).terminal_busy_retries;
    };
    return provider;
}

void write_receipt_if_requested(const conformance::Receipt& receipt) {
    const char* path = std::getenv("PULP_PROVIDER_CONFORMANCE_RECEIPT");
    if (path == nullptr || *path == '\0')
        return;
    REQUIRE(receipt.write(path));
}

} // namespace

TEST_CASE("fake shared IO provider passes the provider conformance kit",
          "[gpu_audio][shared_io][conformance][receipt]") {
    const conformance::ProgramSpec spec;
    auto receipt = conformance::run_suite(fake_provider(), spec);
    REQUIRE(receipt.rows.size() == conformance::kProviderScenarios.size());
    for (const auto& row : receipt.rows) {
        INFO(row.scenario << ": " << conformance::verdict_name(row.verdict) << " (" << row.reason
                          << ")");
        CHECK(row.verdict == Verdict::Passed);
    }

    // Two fake storage configurations are two providers: their outputs null.
    const auto pair = conformance::run_differential_pair(
        fake_provider(),
        fake_provider({}, FakeSharedIoProvider::StorageKind::Staged, "fake-staged"), spec);
    INFO(pair.reason);
    CHECK(pair.verdict == Verdict::Passed);
    CHECK(pair.max_abs_error == 0.0);
    receipt.capacity += 1;
    receipt.add(pair);

    CHECK(receipt.identity_admissible());
    CHECK_FALSE(receipt.truncated());
    CHECK(receipt.passed());
    write_receipt_if_requested(receipt);
}

TEST_CASE("provider conformance kit fails each planted provider violation",
          "[gpu_audio][shared_io][conformance][negative]") {
    using V = FakeSharedIoProvider::Violation;
    struct Planted {
        V violation;
        Scenario scenario;
        const char* reason;
    };
    const Planted planted[] = {
        {V::CallbackAfterDrain, Scenario::TeardownQuiescence, "callback_after_drain"},
        {V::SuccessAfterLoss, Scenario::ForceLossBeforeSubmit, "success_after_loss"},
        {V::SuccessAfterLoss, Scenario::ForceLossBetweenSubmitAndRegistration,
         "success_after_loss"},
        {V::SuccessAfterLoss, Scenario::ForceLossAfterRegistration, "success_after_loss"},
        {V::ResumeAfterLoss, Scenario::DrainAfterLoss, "resume_claimed_after_loss"},
        {V::PushOnRefusal, Scenario::RefusedSubmission, "terminal_after_refusal"},
        {V::DropBusyTerminal, Scenario::TerminalBusyRetry, "busy_terminal_not_retried"},
        {V::ReplayStaleOnResubmit, Scenario::StaleGenerationReplay,
         "provider_emitted_stale_callback"},
        {V::WrongOutput, Scenario::DifferentialCpuReference, "null_residual_exceeds_tolerance"},
        {V::IgnoreDelay, Scenario::MissingCompletion, "delay_fault_not_honoured"},
        {V::IgnoreDelay, Scenario::LateCompletion, "delay_fault_not_honoured"},
    };
    const conformance::ProgramSpec spec;
    for (const auto& entry : planted) {
        const auto name = conformance::scenario_name(entry.scenario);
        // Positive control: the same scenario passes without the violation.
        const auto clean = conformance::run_scenario(entry.scenario, fake_provider(), spec);
        INFO(name << " clean: " << clean.reason);
        CHECK(clean.verdict == Verdict::Passed);

        const auto bad =
            conformance::run_scenario(entry.scenario, fake_provider(entry.violation), spec);
        INFO(name << " planted: " << bad.reason);
        CHECK(bad.verdict == Verdict::Failed);
        CHECK(bad.reason == entry.reason);
    }
}

TEST_CASE("provider pair differential fails a provider with the wrong program",
          "[gpu_audio][shared_io][conformance][negative]") {
    const conformance::ProgramSpec spec;
    const auto clean = conformance::run_differential_pair(fake_provider(), fake_provider(), spec);
    CHECK(clean.verdict == Verdict::Passed);

    const auto bad = conformance::run_differential_pair(
        fake_provider(), fake_provider(FakeSharedIoProvider::Violation::WrongOutput), spec);
    CHECK(bad.verdict == Verdict::Failed);
    CHECK(bad.reason == "provider_pair_null_exceeds_tolerance");
    CHECK(bad.max_abs_error > 0.99);
}

TEST_CASE("provider conformance receipt rejects an unauthenticated real provider",
          "[gpu_audio][shared_io][conformance][receipt][negative]") {
    const conformance::ProgramSpec spec;
    auto as_real = fake_provider();
    as_real.name = "unauthenticated-real";
    as_real.kind = ProviderKind::Real;
    auto make = as_real.make;
    as_real.make = [make](Fault fault) {
        auto created = make(fault);
        static_cast<FakeSharedIoProvider&>(*created.provider).identity = {};
        return created;
    };
    const auto rejected = conformance::run_suite(as_real, spec);
    CHECK_FALSE(rejected.identity.authenticated);
    CHECK_FALSE(rejected.identity_admissible());
    CHECK_FALSE(rejected.passed());
    CHECK(rejected.to_json().find("\"verdict\":\"failed\"") != std::string::npos);

    // Control: the same provider with its authenticated identity is admitted.
    auto authenticated = fake_provider();
    authenticated.kind = ProviderKind::Real;
    const auto admitted = conformance::run_suite(authenticated, spec);
    CHECK(admitted.identity.authenticated);
    CHECK(admitted.identity_admissible());
    CHECK(admitted.passed());
}

TEST_CASE("provider conformance kit records unsupported faults as typed skips",
          "[gpu_audio][shared_io][conformance]") {
    auto partial = fake_provider();
    auto make = partial.make;
    partial.make = [make](Fault fault) {
        if (fault == Fault::HoldTerminalBusy)
            return conformance::CreateOutcome{nullptr, "fault_unsupported"};
        return make(fault);
    };
    const conformance::ProgramSpec spec;
    const auto row = conformance::run_scenario(Scenario::TerminalBusyRetry, partial, spec);
    CHECK(row.verdict == Verdict::Skipped);
    CHECK(row.reason == "provider_unavailable:fault_unsupported");

    partial.busy_retries = nullptr;
    const auto unobservable = conformance::run_scenario(Scenario::TerminalBusyRetry, partial, spec);
    CHECK(unobservable.verdict == Verdict::Skipped);
    CHECK(unobservable.reason == "busy_retry_unobservable");
}

TEST_CASE("provider conformance receipt flags truncation and carries every row",
          "[gpu_audio][shared_io][conformance][receipt]") {
    const conformance::ProgramSpec spec;
    const auto row = conformance::run_scenario(Scenario::ReceiptTruncation, fake_provider(), spec);
    CHECK(row.verdict == Verdict::Passed);

    auto receipt = conformance::run_suite(fake_provider(), spec);
    const auto json = receipt.to_json();
    CHECK(json.find("\"schema\":\"pulp.gpu-audio.provider-conformance.v1\"") != std::string::npos);
    CHECK(json.find("\"truncated\":false") != std::string::npos);
    CHECK(json.find("\"row_count\":13") != std::string::npos);
    for (const auto scenario : conformance::kProviderScenarios) {
        const auto quoted =
            "\"name\":\"" + std::string(conformance::scenario_name(scenario)) + "\"";
        INFO(quoted);
        CHECK(json.find(quoted) != std::string::npos);
    }

    receipt.capacity = receipt.rows.size();
    receipt.add(receipt.rows.front());
    CHECK(receipt.truncated());
    CHECK(receipt.dropped_rows == 1);
    CHECK_FALSE(receipt.passed());
    CHECK(receipt.to_json().find("\"truncated\":true") != std::string::npos);
}

TEST_CASE("conformance fake provider prepared path allocates nothing",
          "[gpu_audio][shared_io][conformance][allocation]") {
    const conformance::ProgramSpec spec;
    FakeSharedIoProvider provider;
    configure(provider, spec);
    conformance::RecordingProvider recorder(provider);
    conformance::SharedIoArena arena;
    REQUIRE(arena.prepare(
        recorder,
        {.slots = 2, .input_bytes_per_slot = spec.bytes(), .output_bytes_per_slot = spec.bytes()}));
    conformance::TerminalLog terminals;

    bool ok = true;
    double worst = 0.0;
    std::size_t allocations = 1;
    {
        pulp::test::RtAllocationProbe probe;
        for (std::uint32_t block = 0; block < 8 && ok; ++block) {
            const auto lease = arena.grant_write(block + 1);
            ok = lease.has_value();
            if (!ok)
                break;
            conformance::write_block(lease->bytes, block, spec.frames);
            ok = arena.publish_written({lease->token}) && arena.submit(lease->token);
            (void)arena.drain_completions(terminals.observer());
            const auto output =
                arena.acquire_output(lease->token.preparation_epoch, lease->token.stream_sequence);
            ok = ok && output.has_value();
            if (output) {
                worst = std::max(worst, conformance::block_error(output->bytes, block, spec));
                ok = arena.release_output({output->token}) && ok;
            }
        }
        allocations = probe.allocation_count();
    }
    CHECK(ok);
    CHECK(worst == 0.0);
    CHECK(terminals.count == 8);
    CHECK(allocations == 0);
    REQUIRE(arena.release());
}
