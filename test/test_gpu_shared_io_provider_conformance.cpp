#include "harness/fake_shared_io_provider.hpp"
#include "harness/rt_allocation_probe.hpp"
#include "harness/shared_io_provider_conformance.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace conformance = pulp::test::shared_io_conformance;
using conformance::Fault;
using conformance::ProviderKind;
using conformance::ProviderUnderTest;
using conformance::Scenario;
using conformance::Verdict;
using pulp::test::FakeSharedIoProvider;
using Violation = FakeSharedIoProvider::Violation;

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

// The conformant fake behaves like the Dawn provider: work retires on
// service, lost work retires failed only at the drain boundary, drain()
// publishes everything still in flight before returning, and a clean drain
// may resume.
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

ProviderUnderTest fake_provider(Violation violation = Violation::None,
                                FakeSharedIoProvider::StorageKind storage =
                                    FakeSharedIoProvider::StorageKind::ImportedHostPointer,
                                std::string name = "fake") {
    ProviderUnderTest provider;
    provider.name = std::move(name);
    provider.storage_kind = storage;
    provider.completion_timeout = std::chrono::milliseconds(500);
    provider.quiet_window = std::chrono::milliseconds(5);
    provider.post_drain_window = std::chrono::milliseconds(10);
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
    provider.loss_record = [](const conformance::SharedIoArenaProvider& inner) {
        const auto& fake = static_cast<const FakeSharedIoProvider&>(inner);
        return conformance::LossRecord{fake.device_lost_callback_ns, fake.device_lost_reason};
    };
    return provider;
}

// A fake with a spontaneous completion source: it installs a notifier and,
// unless `fires` is false, wakes it when each terminal is published.
ProviderUnderTest notifying_fake_provider(bool supported, bool fires, bool expected) {
    auto provider = fake_provider();
    provider.name = "fake-notifying";
    provider.expects_completion_notifier = expected;
    provider.make = [supported, fires](Fault fault) {
        auto fake = std::make_unique<FakeSharedIoProvider>();
        configure(*fake, conformance::ProgramSpec{});
        fake->fault = fake_fault(fault);
        fake->notifier_supported = supported;
        fake->notify_on_terminal = fires;
        return conformance::CreateOutcome{std::move(fake), {}};
    };
    return provider;
}

// One non-conformant provider per contract rule, and the scenarios that must
// fail against it with the stated reason.
struct Mutant {
    Violation violation;
    const char* name;
    std::vector<std::pair<Scenario, const char*>> kills;
};

const std::vector<Mutant>& mutants() {
    static const std::vector<Mutant> registry = {
        {Violation::DuplicateTerminal,
         "duplicate-terminal",
         {{Scenario::Nominal, "stale_or_duplicate_callback"}}},
        {Violation::ScaleError,
         "scale-error",
         {{Scenario::Nominal, "output_mismatch"},
          {Scenario::DifferentialCpuReference, "null_residual_exceeds_tolerance"}}},
        {Violation::EarlyFailedTerminal,
         "early-failed-terminal",
         {{Scenario::ForceLossBeforeSubmit, "terminal_before_drain"},
          {Scenario::ForceLossBetweenSubmitAndRegistration, "terminal_before_drain"},
          {Scenario::ForceLossAfterRegistration, "terminal_before_drain"},
          {Scenario::DrainAfterLoss, "terminal_before_drain"}}},
        {Violation::SuccessAfterLoss,
         "success-after-loss",
         {{Scenario::ForceLossBeforeSubmit, "success_after_loss"},
          {Scenario::ForceLossBetweenSubmitAndRegistration, "success_after_loss"},
          {Scenario::ForceLossAfterRegistration, "success_after_loss"},
          {Scenario::DrainAfterLoss, "success_after_loss"}}},
        {Violation::DuplicateLateTerminal,
         "duplicate-late-terminal",
         {{Scenario::LateCompletion, "stale_or_duplicate_callback"}}},
        {Violation::DrainSkipsWithheld,
         "drain-skips-withheld",
         {{Scenario::LateCompletion, "late_terminal_not_exactly_once"},
          {Scenario::MissingCompletion, "drain_did_not_terminate_missing_work"}}},
        {Violation::PushOnRefusal,
         "push-on-refusal",
         {{Scenario::RefusedSubmission, "terminal_after_refusal"}}},
        {Violation::DropBusyTerminal,
         "drop-busy-terminal",
         {{Scenario::TerminalBusyRetry, "busy_terminal_not_retried"}}},
        {Violation::DeferDrainTerminalToPoll,
         "terminal-after-drain-returns",
         {{Scenario::TeardownQuiescence, "terminal_after_drain_returned"},
          {Scenario::MissingCompletion, "drain_did_not_terminate_missing_work"}}},
        {Violation::CallbackAfterDrain,
         "callback-after-drain",
         {{Scenario::TeardownQuiescence, "callback_after_drain"}}},
        {Violation::PinLostWork,
         "pin-lost-work",
         {{Scenario::DrainAfterLoss, "lost_work_not_terminal_exactly_once"},
          {Scenario::ForceLossAfterRegistration, "lost_work_not_terminal_exactly_once"}}},
        {Violation::ResumeAfterLoss,
         "resume-after-loss",
         {{Scenario::DrainAfterLoss, "resume_claimed_after_loss"},
          {Scenario::ForceLossBeforeSubmit, "resume_claimed_after_loss"}}},
        {Violation::FalseResumeClaim,
         "false-resume-claim",
         {{Scenario::StaleGenerationReplay, "resume_claim_not_honoured"}}},
        {Violation::ReplayStaleOnResubmit,
         "replay-stale-on-resubmit",
         {{Scenario::StaleGenerationReplay, "provider_emitted_stale_callback"}}},
        {Violation::LossCallbackNotRecorded,
         "loss-callback-not-recorded",
         {{Scenario::ForceLossBeforeSubmit, "loss_callback_not_recorded"},
          {Scenario::ForceLossBetweenSubmitAndRegistration, "loss_callback_not_recorded"},
          {Scenario::ForceLossAfterRegistration, "loss_callback_not_recorded"},
          {Scenario::DrainAfterLoss, "loss_callback_not_recorded"}}},
        {Violation::LossCallbackStaleTime,
         "loss-callback-stale-time",
         {{Scenario::ForceLossBeforeSubmit, "loss_callback_time_out_of_range"},
          {Scenario::DrainAfterLoss, "loss_callback_time_out_of_range"}}},
    };
    return registry;
}

char cell(Verdict verdict) {
    return verdict == Verdict::Passed ? 'P' : verdict == Verdict::Failed ? 'F' : 'S';
}

void remove_requested_receipt() {
    if (const char* path = std::getenv("PULP_PROVIDER_CONFORMANCE_RECEIPT"); path && *path)
        std::remove(path);
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
    remove_requested_receipt();
    const conformance::ProgramSpec spec;
    auto receipt = conformance::run_suite(fake_provider(), spec);
    REQUIRE(receipt.rows.size() == conformance::kProviderScenarios.size());
    for (const auto& row : receipt.rows) {
        INFO(row.scenario << ": " << conformance::verdict_name(row.verdict) << " (" << row.reason
                          << ")");
        CHECK(row.verdict == Verdict::Passed);
    }

    // Two fake storage configurations are two providers; each nulls against
    // the CPU reference and against the other.
    const auto pair = conformance::run_differential_pair(
        fake_provider(),
        fake_provider({}, FakeSharedIoProvider::StorageKind::Staged, "fake-staged"), spec);
    INFO(pair.reason);
    CHECK(pair.verdict == Verdict::Passed);
    CHECK(pair.max_abs_error == 0.0);
    receipt.capacity += 1;
    receipt.add(pair);

    // Optional row: a provider with an event source proves the notifier fires.
    const auto notified = conformance::run_completion_notifier(
        notifying_fake_provider(true, true, true), spec);
    INFO(notified.reason);
    CHECK(notified.verdict == Verdict::Passed);
    receipt.capacity += 1;
    receipt.add(notified);

    CHECK(receipt.kind() == ProviderKind::Fake);
    CHECK(receipt.identity_admissible());
    CHECK_FALSE(receipt.truncated());
    CHECK(receipt.passed());
    write_receipt_if_requested(receipt);
}

// The kit's mutation proof: every scenario fails against at least one
// non-conformant provider and passes against the conformant one. The full
// scenario x provider matrix is printed, and written to
// PULP_PROVIDER_CONFORMANCE_MATRIX when that is set.
TEST_CASE("every conformance scenario fails against its mutant provider",
          "[gpu_audio][shared_io][conformance][negative][mutants]") {
    const conformance::ProgramSpec spec;

    std::set<std::string> covered;
    for (const auto& mutant : mutants())
        for (const auto& [scenario, reason] : mutant.kills)
            covered.insert(std::string(conformance::scenario_name(scenario)));
    for (const auto scenario : conformance::kProviderScenarios) {
        INFO(conformance::scenario_name(scenario) << " has no mutant that must fail it");
        CHECK(covered.count(std::string(conformance::scenario_name(scenario))) == 1);
    }

    std::ostringstream matrix;
    matrix << "scenario\tconformant";
    for (const auto& mutant : mutants())
        matrix << '\t' << mutant.name;
    matrix << "\n";

    std::vector<std::vector<conformance::ScenarioResult>> results(mutants().size());
    std::vector<conformance::ScenarioResult> conformant;
    for (const auto scenario : conformance::kProviderScenarios) {
        conformant.push_back(conformance::run_scenario(scenario, fake_provider(), spec));
        for (std::size_t index = 0; index < mutants().size(); ++index)
            results[index].push_back(conformance::run_scenario(
                scenario, fake_provider(mutants()[index].violation), spec));
    }
    for (std::size_t row = 0; row < conformance::kProviderScenarios.size(); ++row) {
        const auto scenario = conformance::kProviderScenarios[row];
        matrix << conformance::scenario_name(scenario) << '\t' << cell(conformant[row].verdict);
        for (const auto& column : results)
            matrix << '\t' << cell(column[row].verdict);
        matrix << "\n";

        INFO(conformance::scenario_name(scenario) << " conformant: " << conformant[row].reason);
        CHECK(conformant[row].verdict == Verdict::Passed);
    }
    for (std::size_t index = 0; index < mutants().size(); ++index) {
        for (const auto& [scenario, reason] : mutants()[index].kills) {
            const auto row = static_cast<std::size_t>(
                std::find(conformance::kProviderScenarios.begin(),
                          conformance::kProviderScenarios.end(), scenario) -
                conformance::kProviderScenarios.begin());
            const auto& result = results[index][row];
            INFO(mutants()[index].name << " x " << conformance::scenario_name(scenario) << ": "
                                       << conformance::verdict_name(result.verdict) << " ("
                                       << result.reason << ")");
            CHECK(result.verdict == Verdict::Failed);
            CHECK(result.reason == reason);
        }
    }

    // The provider-pair differential against a scale mutant, on either side.
    const auto pair_clean =
        conformance::run_differential_pair(fake_provider(), fake_provider(), spec);
    const auto pair_second = conformance::run_differential_pair(
        fake_provider(), fake_provider(Violation::ScaleError, {}, "scaled"), spec);
    const auto pair_first = conformance::run_differential_pair(
        fake_provider(Violation::ScaleError, {}, "scaled"), fake_provider(), spec);
    CHECK(pair_clean.verdict == Verdict::Passed);
    CHECK(pair_second.verdict == Verdict::Failed);
    CHECK(pair_second.reason == "scaled:null_residual_exceeds_tolerance");
    CHECK(pair_first.verdict == Verdict::Failed);
    CHECK(pair_first.reason == "scaled:null_residual_exceeds_tolerance");
    matrix << conformance::kDifferentialPairScenario << '\t' << cell(pair_clean.verdict);
    for (const auto& mutant : mutants())
        matrix << '\t'
               << (mutant.violation == Violation::ScaleError ? cell(pair_second.verdict) : '-');
    matrix << "\n";

    std::cout << "provider conformance matrix (P pass, F fail, S skip):\n" << matrix.str();
    if (const char* path = std::getenv("PULP_PROVIDER_CONFORMANCE_MATRIX"); path && *path) {
        std::ofstream out(path, std::ios::trunc);
        out << matrix.str();
    }
}

TEST_CASE("provider conformance kit derives an unauthenticated real provider as inadmissible",
          "[gpu_audio][shared_io][conformance][receipt][negative]") {
    const conformance::ProgramSpec spec;
    auto with_identity = [](bool authenticated) {
        auto provider = fake_provider();
        auto make = provider.make;
        provider.make = [make, authenticated](Fault fault) {
            auto created = make(fault);
            auto& fake = static_cast<FakeSharedIoProvider&>(*created.provider);
            fake.identity.adapter_backend = "metal";
            fake.identity.authenticated = authenticated;
            fake.identity.native_runtime_authenticated = false;
            return created;
        };
        return provider;
    };
    const auto rejected = conformance::run_suite(with_identity(false), spec);
    CHECK(rejected.kind() == ProviderKind::Real);
    CHECK_FALSE(rejected.identity_admissible());
    CHECK_FALSE(rejected.passed());
    CHECK(rejected.to_json().find("\"kind\":\"real\"") != std::string::npos);
    CHECK(rejected.to_json().find("\"verdict\":\"failed\"") != std::string::npos);

    // Control: the same provider, authenticated, is admitted as real.
    const auto admitted = conformance::run_suite(with_identity(true), spec);
    CHECK(admitted.kind() == ProviderKind::Real);
    CHECK(admitted.identity_admissible());
    CHECK(admitted.passed());

    // A provider that names no backend is unidentified, whatever it claims.
    auto anonymous = fake_provider();
    auto make = anonymous.make;
    anonymous.make = [make](Fault fault) {
        auto created = make(fault);
        static_cast<FakeSharedIoProvider&>(*created.provider).identity = {};
        return created;
    };
    const auto unidentified = conformance::run_suite(anonymous, spec);
    CHECK(unidentified.kind() == ProviderKind::Unidentified);
    CHECK_FALSE(unidentified.passed());
}

TEST_CASE("a real provider must expose its device-lost callback record",
          "[gpu_audio][shared_io][conformance][negative]") {
    const conformance::ProgramSpec spec;
    auto real_without_hook = fake_provider();
    real_without_hook.loss_record = nullptr;
    auto make = real_without_hook.make;
    real_without_hook.make = [make](Fault fault) {
        auto created = make(fault);
        static_cast<FakeSharedIoProvider&>(*created.provider).identity.adapter_backend = "metal";
        return created;
    };
    for (const auto scenario : {Scenario::ForceLossBeforeSubmit, Scenario::DrainAfterLoss}) {
        const auto row = conformance::run_scenario(scenario, real_without_hook, spec);
        INFO(conformance::scenario_name(scenario) << ": " << row.reason);
        CHECK(row.verdict == Verdict::Failed);
        CHECK(row.reason == "loss_callback_unobservable");
    }
    // Controls: the same real provider with the hook passes, and a fake may
    // omit the hook.
    auto real_with_hook = real_without_hook;
    real_with_hook.loss_record = fake_provider().loss_record;
    CHECK(conformance::run_scenario(Scenario::DrainAfterLoss, real_with_hook, spec).verdict ==
          Verdict::Passed);
    auto fake_without_hook = fake_provider();
    fake_without_hook.loss_record = nullptr;
    CHECK(conformance::run_scenario(Scenario::DrainAfterLoss, fake_without_hook, spec).verdict ==
          Verdict::Passed);
}

TEST_CASE("provider conformance kit records unsupported and unprovable paths as typed skips",
          "[gpu_audio][shared_io][conformance]") {
    const conformance::ProgramSpec spec;
    auto partial = fake_provider();
    auto make = partial.make;
    partial.make = [make](Fault fault) {
        if (fault == Fault::HoldTerminalBusy)
            return conformance::CreateOutcome{nullptr, "fault_unsupported"};
        return make(fault);
    };
    const auto row = conformance::run_scenario(Scenario::TerminalBusyRetry, partial, spec);
    CHECK(row.verdict == Verdict::Skipped);
    CHECK(row.reason == "provider_unavailable:fault_unsupported");

    partial.busy_retries = nullptr;
    const auto unobservable = conformance::run_scenario(Scenario::TerminalBusyRetry, partial, spec);
    CHECK(unobservable.verdict == Verdict::Skipped);
    CHECK(unobservable.reason == "busy_retry_unobservable");

    // A provider that cannot resume after a clean drain is not re-primed.
    auto non_resumable = fake_provider();
    auto resumable_make = non_resumable.make;
    non_resumable.make = [resumable_make](Fault fault) {
        auto created = resumable_make(fault);
        static_cast<FakeSharedIoProvider&>(*created.provider).resume_supported = false;
        return created;
    };
    const auto stale =
        conformance::run_scenario(Scenario::StaleGenerationReplay, non_resumable, spec);
    CHECK(stale.verdict == Verdict::Skipped);
    CHECK(stale.reason == "provider_cannot_resume_after_drain");
}

TEST_CASE("provider conformance receipt flags truncation and carries every row",
          "[gpu_audio][shared_io][conformance][receipt]") {
    const conformance::ProgramSpec spec;
    auto receipt = conformance::run_suite(fake_provider(), spec);
    const auto json = receipt.to_json();
    CHECK(json.find("\"schema\":\"pulp.gpu-audio.provider-conformance.v1\"") != std::string::npos);
    CHECK(json.find("\"kind\":\"fake\"") != std::string::npos);
    CHECK(json.find("\"truncated\":false") != std::string::npos);
    CHECK(json.find("\"row_count\":12") != std::string::npos);
    for (const auto scenario : conformance::kProviderScenarios) {
        const auto quoted =
            "\"name\":\"" + std::string(conformance::scenario_name(scenario)) + "\"";
        INFO(quoted);
        CHECK(json.find(quoted) != std::string::npos);
    }
    CHECK(receipt.passed());

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

TEST_CASE("completion notifier scenario proves a provider's wakes",
          "[gpu_audio][shared_io][conformance][wake]") {
    const conformance::ProgramSpec spec;
    // Positive control: installed, fires per submission, silent after clear.
    auto result =
        conformance::run_completion_notifier(notifying_fake_provider(true, true, true), spec);
    INFO(result.reason);
    CHECK(result.verdict == Verdict::Passed);
    CHECK(result.reason == "notifier_fires_per_submission_and_never_after_clear");

    // A provider without a source conforms by refusing, unless it claims one.
    result = conformance::run_completion_notifier(fake_provider(), spec);
    CHECK(result.verdict == Verdict::Passed);
    CHECK(result.reason == "no_spontaneous_completion_source");
    result =
        conformance::run_completion_notifier(notifying_fake_provider(false, true, true), spec);
    CHECK(result.verdict == Verdict::Failed);
    CHECK(result.reason == "notifier_not_installed");

    // A provider that installs the notifier but never fires it fails.
    result =
        conformance::run_completion_notifier(notifying_fake_provider(true, false, true), spec);
    CHECK(result.verdict == Verdict::Failed);
    CHECK(result.reason == "notify_missing_for_submission");
}
