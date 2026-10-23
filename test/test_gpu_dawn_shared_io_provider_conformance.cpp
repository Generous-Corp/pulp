// Host-only instantiation of the provider conformance kit for the Dawn
// shared-I/O provider. Each scenario creates a fresh provider with the Dawn
// provider's own fault injection armed; the kit drives it through the
// production arena exactly as it drives the fake. An unavailable physical
// device is a Catch2 skip, never a pass.

#include "detail/dawn_shared_io_provider.hpp"
#include "harness/fake_shared_io_provider.hpp"
#include "harness/shared_io_provider_conformance.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>

#ifndef PULP_GPU_AUDIO_EXPECTED_DAWN_SHA
#define PULP_GPU_AUDIO_EXPECTED_DAWN_SHA ""
#endif

namespace {

namespace conformance = pulp::test::shared_io_conformance;
using conformance::Fault;
using conformance::ProviderKind;
using conformance::ProviderUnderTest;
using conformance::Verdict;
using pulp::gpu_audio::detail::DawnSharedIoProvider;

DawnSharedIoProvider::Fault dawn_fault(Fault fault) {
    switch (fault) {
    case Fault::None:
        return DawnSharedIoProvider::Fault::None;
    case Fault::ForceLossBeforeSubmit:
        return DawnSharedIoProvider::Fault::ForceLossBeforeSubmit;
    case Fault::ForceLossBetweenSubmitAndCompletionRegistration:
        return DawnSharedIoProvider::Fault::ForceLossBetweenSubmitAndCompletionRegistration;
    case Fault::ForceLossAfterCompletionRegistration:
        return DawnSharedIoProvider::Fault::ForceLossAfterCompletionRegistration;
    case Fault::DelayCompletion:
        return DawnSharedIoProvider::Fault::DelayCompletion;
    case Fault::RejectBeforeSubmit:
        return DawnSharedIoProvider::Fault::RejectBeforeSubmit;
    case Fault::HoldTerminalBusy:
        return DawnSharedIoProvider::Fault::HoldTerminalBusy;
    }
    return DawnSharedIoProvider::Fault::None;
}

ProviderUnderTest dawn_provider(DawnSharedIoProvider::StorageKind storage, std::string name) {
    ProviderUnderTest provider;
    provider.name = std::move(name);
    provider.storage_kind = storage;
    provider.make = [storage](Fault fault) {
        auto created = DawnSharedIoProvider::create(
            {.expected_dawn_revision = PULP_GPU_AUDIO_EXPECTED_DAWN_SHA,
             .fault = dawn_fault(fault),
             .fault_slot = 0,
             .storage_kind = storage});
        if (!created.provider)
            return conformance::CreateOutcome{nullptr, created.reason};
        return conformance::CreateOutcome{std::move(created.provider), {}};
    };
    provider.busy_retries = [](const conformance::SharedIoArenaProvider& inner) {
        return static_cast<const DawnSharedIoProvider&>(inner).stats().terminal_busy_retries;
    };
    provider.loss_record = [](const conformance::SharedIoArenaProvider& inner) {
        const auto stats = static_cast<const DawnSharedIoProvider&>(inner).stats();
        return conformance::LossRecord{stats.device_lost_callback_ns, stats.device_lost_reason};
    };
    return provider;
}

ProviderUnderTest fake_reference() {
    ProviderUnderTest provider;
    provider.name = "fake";
    provider.make = [](Fault) {
        const conformance::ProgramSpec spec;
        auto fake = std::make_unique<pulp::test::FakeSharedIoProvider>();
        fake->transform = pulp::test::FakeSharedIoProvider::Transform::Affine;
        fake->scale = spec.scale;
        fake->bias = spec.bias;
        fake->complete_on_poll = true;
        fake->drain_completes = true;
        fake->identity = pulp::test::FakeSharedIoProvider::authenticated_fake_identity();
        return conformance::CreateOutcome{std::move(fake), {}};
    };
    return provider;
}

} // namespace

TEST_CASE("Dawn shared IO provider passes the provider conformance kit",
          "[gpu_audio][shared_io][conformance][dawn]") {
    // A skipped run must leave no receipt, so the verify step skips rather
    // than re-reading a previous run's file.
    const char* receipt_path = std::getenv("PULP_PROVIDER_CONFORMANCE_RECEIPT");
    if (receipt_path && *receipt_path)
        std::remove(receipt_path);
    const auto imported =
        dawn_provider(DawnSharedIoProvider::StorageKind::ImportedHostPointer, "dawn");
    if (auto probe = imported.make(Fault::None); !probe.provider)
        SKIP("Dawn shared-I/O provider unavailable: " << probe.unavailable_reason);

    const conformance::ProgramSpec spec;
    auto receipt = conformance::run_suite(imported, spec);
    REQUIRE(receipt.rows.size() == conformance::kProviderScenarios.size());
    for (const auto& row : receipt.rows) {
        INFO(row.scenario << ": " << conformance::verdict_name(row.verdict) << " (" << row.reason
                          << ") submissions=" << row.submissions << " success="
                          << row.retired_success << " failure=" << row.retired_failure
                          << " rejected=" << row.rejected_callbacks
                          << " max_abs_error=" << row.max_abs_error);
        CHECK(row.verdict == Verdict::Passed);
    }

    // Two real providers: imported host pointers against staged copies. Each
    // nulls against the CPU reference, so staged Dawn is checked too.
    const auto staged = dawn_provider(DawnSharedIoProvider::StorageKind::Staged, "dawn-staged");
    const auto dawn_pair = conformance::run_differential_pair(imported, staged, spec);
    INFO(dawn_pair.reason << " max_abs_error=" << dawn_pair.max_abs_error);
    CHECK(dawn_pair.verdict == Verdict::Passed);
    receipt.capacity += 1;
    receipt.add(dawn_pair);

    // And the GPU against the fake that runs the same program on the CPU.
    const auto fake_pair = conformance::run_differential_pair(imported, fake_reference(), spec);
    INFO(fake_pair.reason << " max_abs_error=" << fake_pair.max_abs_error);
    CHECK(fake_pair.verdict == Verdict::Passed);

    INFO("identity authenticated=" << receipt.identity.authenticated
                                   << " revision=" << receipt.identity.provider_revision
                                   << " adapter=" << receipt.identity.adapter_name);
    CHECK(receipt.kind() == ProviderKind::Real);
    CHECK(receipt.identity.authenticated);
    CHECK(receipt.passed());
    if (receipt_path && *receipt_path)
        REQUIRE(receipt.write(receipt_path));
}
