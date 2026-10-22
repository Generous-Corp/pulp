#pragma once

// Provider conformance kit for SharedIoArenaProvider.
//
// Every provider is driven through the production SharedIoArena, exactly as
// the realtime dispatcher drives it, and judged only by what the provider
// itself controls: submit() results, the terminals it publishes and when,
// delivered output, device-loss and resume facts, and inbox callbacks after
// its drain boundary. Checks whose outcome is fixed by the arena, ledger or
// inbox alone are not scenario evidence and are not counted. A provider
// supplies a factory that builds a fresh instance with one kit-level fault
// armed; the kit owns the program spec and its CPU reference.
//
// Each scenario returns a ScenarioResult instead of asserting, so a mutant
// provider can prove the scenario turns red, and every run can be bound into
// a `pulp.gpu-audio.provider-conformance.v1` receipt.

#include "detail/shared_io_arena.hpp"
#include "detail/shared_io_provider_identity.hpp"

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace pulp::test::shared_io_conformance {

using pulp::gpu_audio::detail::SharedIoArena;
using pulp::gpu_audio::detail::SharedIoArenaProvider;
using pulp::gpu_audio::detail::SharedIoProviderCapabilities;
using pulp::gpu_audio::detail::SharedIoProviderIdentity;
using pulp::gpu_audio::detail::SharedIoTerminalInbox;
using pulp::gpu_audio::detail::SharedIoTerminalStatus;
using SlotToken = SharedIoArenaProvider::SlotToken;

inline constexpr std::string_view kReceiptSchema = "pulp.gpu-audio.provider-conformance.v1";
inline constexpr std::uint32_t kKitVersion = 1;

// Kit-level faults. A factory maps each to its provider's own injection, or
// reports it unsupported, which the kit records as a typed skip.
enum class Fault : std::uint8_t {
    None,
    ForceLossBeforeSubmit,
    ForceLossBetweenSubmitAndCompletionRegistration,
    ForceLossAfterCompletionRegistration,
    // Withhold completion of slot 0 from service until drain().
    DelayCompletion,
    // Refuse slot 0 before any backend submission.
    RejectBeforeSubmit,
    // First terminal publication finds its inbox cell busy.
    HoldTerminalBusy,
};

// Derived from the provider's own identity, never declared by the caller:
// a "fake" adapter backend is a fake, any other named backend is real, and
// a provider that names no backend is unidentified and never admissible.
enum class ProviderKind : std::uint8_t { Fake, Real, Unidentified };

inline constexpr std::string_view kFakeBackend = "fake";

inline ProviderKind kind_of(const SharedIoProviderIdentity& identity) noexcept {
    if (identity.adapter_backend == kFakeBackend)
        return ProviderKind::Fake;
    if (!identity.adapter_backend.empty())
        return ProviderKind::Real;
    return ProviderKind::Unidentified;
}

inline std::string_view kind_name(ProviderKind kind) noexcept {
    switch (kind) {
    case ProviderKind::Fake:
        return "fake";
    case ProviderKind::Real:
        return "real";
    case ProviderKind::Unidentified:
        return "unidentified";
    }
    return "unidentified";
}

// The program every provider runs through its ordinary submit(): an affine
// map over float samples. The Dawn P1 provider program is 1.75x + 0.25. The
// verifier pins these values; a receipt for another program is rejected.
struct ProgramSpec {
    std::string name = "affine-1.75x+0.25";
    float scale = 1.75f;
    float bias = 0.25f;
    std::uint32_t frames = 64;
    std::uint32_t slots = 2;
    std::uint32_t blocks = 32;
    double tolerance = 1.0e-5;

    float reference(float x) const noexcept {
        return x * scale + bias;
    }
    std::size_t bytes() const noexcept {
        return static_cast<std::size_t>(frames) * sizeof(float);
    }
};

// What a provider recorded inside its device-lost callback: the steady-clock
// time of the callback and the backend's numeric loss reason. Zero means no
// loss callback has run.
struct LossRecord {
    std::uint64_t callback_ns = 0;
    std::uint32_t reason = 0;
};

inline std::uint64_t steady_now_ns() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

struct CreateOutcome {
    std::unique_ptr<SharedIoArenaProvider> provider;
    // Non-empty when the provider or the requested fault is unavailable.
    std::string unavailable_reason;
};

struct ProviderUnderTest {
    std::string name;
    std::function<CreateOutcome(Fault)> make;
    // The backing the arena requests; it must match what the provider creates.
    SharedIoArenaProvider::StorageKind storage_kind =
        SharedIoArenaProvider::StorageKind::ImportedHostPointer;
    // Optional: busy terminal publications retained and retried so far. Without
    // it the kit cannot prove the busy path ran and skips that scenario.
    std::function<std::uint64_t(const SharedIoArenaProvider&)> busy_retries;
    // Reads the provider's device-lost callback record. Required for a real
    // provider (its identity derives kind real): without it every loss
    // scenario fails. A fake may omit it.
    std::function<LossRecord(const SharedIoArenaProvider&)> loss_record;
    // The provider has a spontaneous completion source, so the completion
    // notifier scenario requires it to install and fire.
    bool expects_completion_notifier = false;
    std::chrono::milliseconds completion_timeout{15000};
    // How long withheld or lost work is serviced before the drain boundary.
    std::chrono::milliseconds quiet_window{30};
    // How long inboxes are watched for callbacks after the final drain.
    std::chrono::milliseconds post_drain_window{50};
};

enum class Verdict : std::uint8_t { Passed, Failed, Skipped };

inline std::string_view verdict_name(Verdict verdict) noexcept {
    switch (verdict) {
    case Verdict::Passed:
        return "passed";
    case Verdict::Failed:
        return "failed";
    case Verdict::Skipped:
        return "skipped";
    }
    return "failed";
}

enum class Scenario : std::uint8_t {
    Nominal,
    ForceLossBeforeSubmit,
    ForceLossBetweenSubmitAndRegistration,
    ForceLossAfterRegistration,
    LateCompletion,
    MissingCompletion,
    RefusedSubmission,
    TerminalBusyRetry,
    TeardownQuiescence,
    DrainAfterLoss,
    StaleGenerationReplay,
    DifferentialCpuReference,
    // Optional, not in kProviderScenarios: run through run_completion_notifier().
    CompletionNotifier,
};

inline constexpr std::array<Scenario, 12> kProviderScenarios = {
    Scenario::Nominal,
    Scenario::ForceLossBeforeSubmit,
    Scenario::ForceLossBetweenSubmitAndRegistration,
    Scenario::ForceLossAfterRegistration,
    Scenario::LateCompletion,
    Scenario::MissingCompletion,
    Scenario::RefusedSubmission,
    Scenario::TerminalBusyRetry,
    Scenario::TeardownQuiescence,
    Scenario::DrainAfterLoss,
    Scenario::StaleGenerationReplay,
    Scenario::DifferentialCpuReference,
};

inline constexpr std::string_view kDifferentialPairScenario = "differential_provider_pair";

inline std::string_view scenario_name(Scenario scenario) noexcept {
    switch (scenario) {
    case Scenario::Nominal:
        return "nominal";
    case Scenario::ForceLossBeforeSubmit:
        return "force_loss_before_submit";
    case Scenario::ForceLossBetweenSubmitAndRegistration:
        return "force_loss_between_submit_and_registration";
    case Scenario::ForceLossAfterRegistration:
        return "force_loss_after_registration";
    case Scenario::LateCompletion:
        return "late_completion";
    case Scenario::MissingCompletion:
        return "missing_completion";
    case Scenario::RefusedSubmission:
        return "refused_submission";
    case Scenario::TerminalBusyRetry:
        return "terminal_busy_retry";
    case Scenario::TeardownQuiescence:
        return "teardown_quiescence";
    case Scenario::DrainAfterLoss:
        return "drain_after_loss";
    case Scenario::StaleGenerationReplay:
        return "stale_generation_replay";
    case Scenario::DifferentialCpuReference:
        return "differential_cpu_reference";
    case Scenario::CompletionNotifier:
        return "completion_notifier";
    }
    return "unknown";
}

struct ScenarioResult {
    std::string scenario;
    Verdict verdict = Verdict::Failed;
    std::string reason;
    std::uint64_t submissions = 0;
    std::uint64_t retired_success = 0;
    std::uint64_t retired_failure = 0;
    std::uint64_t rejected_callbacks = 0;
    // Only differential scenarios measure a residual; negative means unmeasured.
    double max_abs_error = -1.0;

    bool passed() const noexcept {
        return verdict == Verdict::Passed;
    }
};

// ---------------------------------------------------------------------------
// Observation plumbing

// Forwards every call to the provider under test and records each submission's
// token and inbox, so the kit can replay stale terminals and watch an inbox
// after the provider's drain boundary. While `suppress_service` is set, poll()
// and service_until() are not forwarded: the kit then sees only what drain()
// itself published before returning.
class RecordingProvider final : public SharedIoArenaProvider {
  public:
    explicit RecordingProvider(SharedIoArenaProvider& inner) : inner_(inner) {
        submissions_.reserve(256);
    }

    struct Submission {
        SlotToken token;
        std::shared_ptr<SharedIoTerminalInbox> inbox;
        bool accepted = false;
    };

    bool create_slot(std::uint32_t slot, std::size_t input_bytes, std::size_t output_bytes,
                     SlotResources& resources) noexcept override {
        return inner_.create_slot(slot, input_bytes, output_bytes, resources);
    }
    void retire_slot(SlotResources& resources) noexcept override {
        inner_.retire_slot(resources);
    }
    void destroy_slot(SlotResources& resources) noexcept override {
        inner_.destroy_slot(resources);
    }
    bool acquire_slot_buffers(const SlotResources& resources,
                              SlotBufferHandle& handle) const noexcept override {
        return inner_.acquire_slot_buffers(resources, handle);
    }
    bool validate_slot_buffers(const SlotBufferHandle& handle) const noexcept override {
        return inner_.validate_slot_buffers(handle);
    }
    bool submit(const SlotResources& resources, SlotToken token,
                std::shared_ptr<SharedIoTerminalInbox> inbox) noexcept override {
        auto retained = inbox;
        const bool accepted = inner_.submit(resources, token, std::move(inbox));
        if (submissions_.size() < submissions_.capacity())
            submissions_.push_back({token, std::move(retained), accepted});
        return accepted;
    }
    void poll() noexcept override {
        if (!suppress_service)
            inner_.poll();
    }
    void service_until(std::uint64_t deadline_ns) noexcept override {
        if (!suppress_service)
            inner_.service_until(deadline_ns);
    }
    std::uint64_t service_wait_ns() const noexcept override {
        return inner_.service_wait_ns();
    }
    bool device_lost() const noexcept override {
        return inner_.device_lost();
    }
    bool can_resume_after_drain() const noexcept override {
        return inner_.can_resume_after_drain();
    }
    bool set_completion_notifier(
        pulp::gpu_audio::detail::SharedIoCompletionNotifier notifier) noexcept override {
        return inner_.set_completion_notifier(notifier);
    }
    SharedIoProviderIdentity provider_identity() const noexcept override {
        return inner_.provider_identity();
    }
    SharedIoProviderCapabilities provider_capabilities() const noexcept override {
        return inner_.provider_capabilities();
    }
    bool gpu_elapsed_ns(SlotToken token, std::uint64_t& elapsed) const noexcept override {
        return inner_.gpu_elapsed_ns(token, elapsed);
    }
    bool drain() noexcept override {
        return inner_.drain();
    }

    const std::vector<Submission>& submissions() const noexcept {
        return submissions_;
    }

    bool suppress_service = false;

  private:
    SharedIoArenaProvider& inner_;
    std::vector<Submission> submissions_;
};

struct TerminalLog {
    struct Entry {
        SlotToken token;
        SharedIoTerminalStatus status = SharedIoTerminalStatus::RetiredFailed;
    };
    std::array<Entry, 256> entries{};
    std::size_t count = 0;
    std::size_t overflow = 0;

    SharedIoArena::CompletionObserver observer() noexcept {
        return {this,
                [](void* context, const SlotToken& token, SharedIoTerminalStatus status) noexcept {
                    auto* log = static_cast<TerminalLog*>(context);
                    if (log->count < log->entries.size())
                        log->entries[log->count++] = {token, status};
                    else
                        ++log->overflow;
                }};
    }
    std::size_t count_for(const SlotToken& token) const noexcept {
        return static_cast<std::size_t>(
            std::count_if(entries.begin(), entries.begin() + static_cast<std::ptrdiff_t>(count),
                          [&](const Entry& entry) { return entry.token == token; }));
    }
    std::optional<SharedIoTerminalStatus> status_for(const SlotToken& token) const noexcept {
        for (std::size_t index = 0; index < count; ++index) {
            if (entries[index].token == token)
                return entries[index].status;
        }
        return std::nullopt;
    }
    std::uint64_t successes() const noexcept {
        return static_cast<std::uint64_t>(
            std::count_if(entries.begin(), entries.begin() + static_cast<std::ptrdiff_t>(count),
                          [](const Entry& entry) {
                              return entry.status == SharedIoTerminalStatus::RetiredSuccess;
                          }));
    }
    std::uint64_t failures() const noexcept {
        return static_cast<std::uint64_t>(count) - successes();
    }
};

// Deterministic, non-periodic block content: a tone plus a ramp, so a slot or
// sequence mix-up cannot null by accident.
inline float stimulus(std::uint32_t block, std::uint32_t frame, std::uint32_t frames) noexcept {
    const auto n = static_cast<double>(block) * frames + frame;
    const double tone = 0.5 * std::sin(2.0 * 3.14159265358979323846 * 440.0 * n / 48000.0);
    const double ramp = static_cast<double>((frame + block * 7u) % 97u) * 0.125 - 3.0;
    return static_cast<float>(tone + ramp);
}

inline void write_block(std::span<std::byte> bytes, std::uint32_t block,
                        std::uint32_t frames) noexcept {
    const auto values = std::min<std::size_t>(frames, bytes.size() / sizeof(float));
    for (std::size_t frame = 0; frame < values; ++frame) {
        const float sample = stimulus(block, static_cast<std::uint32_t>(frame), frames);
        std::memcpy(bytes.data() + frame * sizeof(float), &sample, sizeof(float));
    }
}

inline double sample_error(float actual, std::uint32_t block, std::uint32_t frame,
                           const ProgramSpec& spec) noexcept {
    if (!std::isfinite(actual))
        return INFINITY;
    const float expected = spec.reference(stimulus(block, frame, spec.frames));
    return std::abs(static_cast<double>(actual) - expected);
}

inline double block_error(std::span<const std::byte> bytes, std::uint32_t block,
                          const ProgramSpec& spec) noexcept {
    if (bytes.size() < spec.bytes())
        return INFINITY;
    double worst = 0.0;
    for (std::uint32_t frame = 0; frame < spec.frames; ++frame) {
        float actual = 0.0f;
        std::memcpy(&actual, bytes.data() + frame * sizeof(float), sizeof(float));
        worst = std::max(worst, sample_error(actual, block, frame, spec));
    }
    return worst;
}

// ---------------------------------------------------------------------------
// Scenario machinery

namespace detail {

using Clock = std::chrono::steady_clock;

struct Run {
    const ProviderUnderTest& provider;
    const ProgramSpec& spec;
    ScenarioResult result;
    std::unique_ptr<SharedIoArenaProvider> owned;
    std::unique_ptr<RecordingProvider> recorder;
    SharedIoArena arena;
    TerminalLog terminals;

    Run(const ProviderUnderTest& under_test, const ProgramSpec& program, Scenario scenario)
        : provider(under_test), spec(program) {
        result.scenario = std::string(scenario_name(scenario));
    }

    ~Run() {
        // A failed scenario may leave work pinned; a bounded drain keeps the
        // provider destructor from observing live slots where it can.
        if (arena.prepared())
            (void)arena.release();
    }

    bool create(Fault fault) {
        auto created = provider.make(fault);
        if (!created.provider) {
            result.verdict = Verdict::Skipped;
            result.reason = "provider_unavailable:" + (created.unavailable_reason.empty()
                                                           ? std::string("unspecified")
                                                           : created.unavailable_reason);
            return false;
        }
        owned = std::move(created.provider);
        recorder = std::make_unique<RecordingProvider>(*owned);
        return true;
    }

    bool prepare(std::uint32_t slots) {
        if (!arena.prepare(*recorder, {.slots = slots,
                                       .input_bytes_per_slot = spec.bytes(),
                                       .output_bytes_per_slot = spec.bytes(),
                                       .storage_kind = provider.storage_kind}))
            return fail("prepare_failed");
        return true;
    }

    bool fail(std::string reason) {
        if (result.verdict != Verdict::Failed || result.reason.empty())
            result.reason = std::move(reason);
        result.verdict = Verdict::Failed;
        return false;
    }

    ScenarioResult failed(std::string reason) {
        finish_counts();
        fail(std::move(reason));
        return result;
    }

    ScenarioResult skipped(std::string reason) {
        finish_counts();
        result.verdict = Verdict::Skipped;
        result.reason = std::move(reason);
        return result;
    }

    void pass(std::string reason) {
        result.verdict = Verdict::Passed;
        result.reason = std::move(reason);
    }

    void service_once() {
        (void)arena.drain_completions(terminals.observer());
    }

    // Grants, writes and submits one block. `accepted` reports submit().
    std::optional<SlotToken> submit_block(std::uint64_t sequence, std::uint32_t block,
                                          bool& accepted) {
        auto lease = arena.grant_write(sequence);
        if (!lease)
            return std::nullopt;
        write_block(lease->bytes, block, spec.frames);
        if (!arena.publish_written({lease->token}))
            return std::nullopt;
        accepted = arena.submit(lease->token);
        if (accepted)
            ++result.submissions;
        return lease->token;
    }

    // Services until the output for `token` can be claimed, then measures it
    // against the CPU reference and releases it.
    std::optional<double> await_output(const SlotToken& token, std::uint32_t block) {
        const auto deadline = Clock::now() + provider.completion_timeout;
        for (;;) {
            service_once();
            auto output = arena.acquire_output(token.preparation_epoch, token.stream_sequence);
            if (output) {
                const double error = block_error(output->bytes, block, spec);
                const auto claimed = output->token;
                output.reset();
                if (!arena.release_output({claimed}))
                    return std::nullopt;
                return error;
            }
            if (terminals.status_for(token) == SharedIoTerminalStatus::RetiredFailed)
                return std::nullopt;
            if (Clock::now() >= deadline)
                return std::nullopt;
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }

    // Services for the quiet window; returns the terminals seen for `token`.
    std::size_t observe_quiet(const SlotToken& token) {
        const auto deadline = Clock::now() + provider.quiet_window;
        while (Clock::now() < deadline) {
            service_once();
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
        return terminals.count_for(token);
    }

    // The dispatcher's retirement step: stop admission and cross the
    // provider's drain boundary. Service is suppressed while consuming, so
    // the log holds exactly what drain() published before it returned.
    bool retire_and_drain() {
        arena.begin_retirement();
        if (!recorder->drain())
            return fail("drain_failed");
        recorder->suppress_service = true;
        service_once();
        recorder->suppress_service = false;
        return true;
    }

    // Teardown: release succeeds, then no callback reaches any inbox the
    // provider was handed, through the release's own drains, a watch window
    // of further service, and one more drain barrier.
    bool release_and_verify_quiescence() {
        std::vector<std::shared_ptr<SharedIoTerminalInbox>> inboxes;
        for (const auto& submission : recorder->submissions()) {
            if (submission.inbox &&
                std::find(inboxes.begin(), inboxes.end(), submission.inbox) == inboxes.end())
                inboxes.push_back(submission.inbox);
        }
        std::vector<std::uint64_t> baseline;
        for (const auto& inbox : inboxes)
            baseline.push_back(inbox->rejected_callbacks());
        result.rejected_callbacks = arena.rejected_terminal_callbacks();
        if (!arena.release())
            return fail("release_failed");
        const auto deadline = Clock::now() + provider.post_drain_window;
        while (Clock::now() < deadline) {
            recorder->poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (!recorder->drain())
            return fail("repeated_drain_failed");
        recorder->poll();
        for (std::size_t index = 0; index < inboxes.size(); ++index) {
            if (inboxes[index]->rejected_callbacks() != baseline[index] ||
                !inboxes[index]->quiescent())
                return fail("callback_after_drain");
        }
        return true;
    }

    // A loss scenario's provider must have recorded its device-lost callback,
    // timestamped between the faulted submission and now.
    bool verify_loss_record(std::uint64_t submitted_ns) {
        if (!provider.loss_record) {
            if (kind_of(owned->provider_identity()) == ProviderKind::Real)
                return fail("loss_callback_unobservable");
            return true;
        }
        const auto record = provider.loss_record(*owned);
        if (record.callback_ns == 0 || record.reason == 0)
            return fail("loss_callback_not_recorded");
        if (record.callback_ns < submitted_ns || record.callback_ns > steady_now_ns())
            return fail("loss_callback_time_out_of_range");
        return true;
    }

    void finish_counts() {
        result.retired_success = terminals.successes();
        result.retired_failure = terminals.failures();
        if (arena.prepared())
            result.rejected_callbacks = arena.rejected_terminal_callbacks();
    }
};

inline Fault loss_fault(Scenario scenario) noexcept {
    switch (scenario) {
    case Scenario::ForceLossBeforeSubmit:
        return Fault::ForceLossBeforeSubmit;
    case Scenario::ForceLossBetweenSubmitAndRegistration:
        return Fault::ForceLossBetweenSubmitAndCompletionRegistration;
    default:
        return Fault::ForceLossAfterCompletionRegistration;
    }
}

inline ScenarioResult nominal(const ProviderUnderTest& provider, const ProgramSpec& spec) {
    Run run(provider, spec, Scenario::Nominal);
    if (!run.create(Fault::None) || !run.prepare(spec.slots))
        return run.result;
    double worst = 0.0;
    for (std::uint32_t block = 0; block < std::max<std::uint32_t>(spec.slots * 2, 4); ++block) {
        bool accepted = false;
        const auto token = run.submit_block(block + 1, block, accepted);
        if (!token || !accepted)
            return run.failed("submit_failed");
        const auto error = run.await_output(*token, block);
        if (!error)
            return run.failed("completion_missing");
        worst = std::max(worst, *error);
        if (run.terminals.count_for(*token) != 1)
            return run.failed("terminal_not_exactly_once");
    }
    run.result.max_abs_error = worst;
    if (worst > spec.tolerance)
        return run.failed("output_mismatch");
    if (run.terminals.failures() != 0)
        return run.failed("unexpected_failed_terminal");
    if (run.arena.rejected_terminal_callbacks() != 0)
        return run.failed("stale_or_duplicate_callback");
    if (run.recorder->device_lost())
        return run.failed("device_lost_without_fault");
    run.finish_counts();
    if (!run.release_and_verify_quiescence())
        return run.result;
    run.pass("exact_once_terminals_and_reference_output");
    return run.result;
}

// Lost work retires failed exactly once, and only at the drain boundary; the
// provider then reports loss and refuses to resume.
inline ScenarioResult force_loss(const ProviderUnderTest& provider, const ProgramSpec& spec,
                                 Scenario scenario) {
    Run run(provider, spec, scenario);
    if (!run.create(loss_fault(scenario)) || !run.prepare(spec.slots))
        return run.result;
    const auto submitted_ns = steady_now_ns();
    bool accepted = false;
    const auto token = run.submit_block(1, 0, accepted);
    if (!token)
        return run.failed("grant_failed");
    (void)run.observe_quiet(*token);
    if (!accepted) {
        // A proven pre-submit refusal is legal; it must not publish a terminal.
        if (run.terminals.count != 0 || run.arena.rejected_terminal_callbacks() != 0)
            return run.failed("terminal_after_refusal");
    } else {
        if (run.terminals.successes() != 0)
            return run.failed("success_after_loss");
        if (run.terminals.count_for(*token) != 0)
            return run.failed("terminal_before_drain");
        if (!run.retire_and_drain())
            return run.failed("drain_failed_after_loss");
        if (run.terminals.count_for(*token) != 1)
            return run.failed("lost_work_not_terminal_exactly_once");
        if (run.terminals.status_for(*token) != SharedIoTerminalStatus::RetiredFailed)
            return run.failed("success_after_loss");
        (void)run.arena.discard(*token);
    }
    if (!run.recorder->device_lost())
        return run.failed("device_loss_not_reported");
    if (!run.verify_loss_record(submitted_ns))
        return run.failed(run.result.reason);
    if (run.recorder->can_resume_after_drain())
        return run.failed("resume_claimed_after_loss");
    run.finish_counts();
    if (!run.release_and_verify_quiescence())
        return run.result;

    // The same provider may refuse a new preparation outright. If it admits
    // one, nothing it accepts may retire successfully.
    SharedIoArena retry;
    TerminalLog retry_terminals;
    if (retry.prepare(*run.recorder, {.slots = 1,
                                      .input_bytes_per_slot = spec.bytes(),
                                      .output_bytes_per_slot = spec.bytes(),
                                      .storage_kind = provider.storage_kind})) {
        auto lease = retry.grant_write(1);
        if (lease) {
            write_block(lease->bytes, 0, spec.frames);
            (void)retry.publish_written({lease->token});
            if (retry.submit(lease->token)) {
                const auto deadline = Clock::now() + provider.quiet_window;
                while (Clock::now() < deadline) {
                    (void)retry.drain_completions(retry_terminals.observer());
                    std::this_thread::sleep_for(std::chrono::microseconds(500));
                }
            }
        }
        retry.begin_retirement();
        (void)run.recorder->drain();
        (void)retry.drain_completions(retry_terminals.observer());
        if (!retry.release())
            return run.failed("post_loss_release_failed");
        if (retry_terminals.successes() != 0)
            return run.failed("success_after_loss_on_new_preparation");
    }
    run.pass(accepted ? "lost_work_failed_at_drain_and_not_resumable"
                      : "refused_before_submit_and_not_resumable");
    return run.result;
}

// Work withheld past its delivery deadline retires exactly once at drain and
// is never published twice.
inline ScenarioResult late_completion(const ProviderUnderTest& provider, const ProgramSpec& spec) {
    Run run(provider, spec, Scenario::LateCompletion);
    if (!run.create(Fault::DelayCompletion) || !run.prepare(spec.slots))
        return run.result;
    bool accepted = false;
    const auto token = run.submit_block(1, 0, accepted);
    if (!token || !accepted)
        return run.failed("submit_failed");
    if (token->slot != 0)
        return run.failed("fault_slot_not_targeted");
    if (!run.arena.expire_delivery(*token))
        return run.failed("expiry_rejected");
    if (run.observe_quiet(*token) != 0)
        return run.failed("delay_fault_not_honoured");
    if (!run.retire_and_drain())
        return run.result;
    if (run.terminals.count_for(*token) != 1)
        return run.failed("late_terminal_not_exactly_once");
    if (run.arena.rejected_terminal_callbacks() != 0)
        return run.failed("stale_or_duplicate_callback");
    (void)run.arena.discard(*token);
    run.finish_counts();
    if (!run.release_and_verify_quiescence())
        return run.result;
    run.pass("late_terminal_retired_once_at_drain");
    return run.result;
}

// Work that never completes on service is forced terminal by drain().
inline ScenarioResult missing_completion(const ProviderUnderTest& provider,
                                         const ProgramSpec& spec) {
    Run run(provider, spec, Scenario::MissingCompletion);
    if (!run.create(Fault::DelayCompletion) || !run.prepare(1))
        return run.result;
    bool accepted = false;
    const auto token = run.submit_block(1, 0, accepted);
    if (!token || !accepted)
        return run.failed("submit_failed");
    if (run.observe_quiet(*token) != 0)
        return run.failed("delay_fault_not_honoured");
    if (!run.arena.expire_delivery(*token))
        return run.failed("expiry_rejected");
    if (!run.retire_and_drain())
        return run.result;
    if (run.terminals.count_for(*token) != 1)
        return run.failed("drain_did_not_terminate_missing_work");
    (void)run.arena.discard(*token);
    run.finish_counts();
    if (!run.release_and_verify_quiescence())
        return run.result;
    run.pass("drain_forced_the_missing_terminal");
    return run.result;
}

inline ScenarioResult refused_submission(const ProviderUnderTest& provider,
                                         const ProgramSpec& spec) {
    Run run(provider, spec, Scenario::RefusedSubmission);
    if (!run.create(Fault::RejectBeforeSubmit) || !run.prepare(spec.slots))
        return run.result;
    bool accepted = true;
    const auto token = run.submit_block(1, 0, accepted);
    if (!token)
        return run.failed("grant_failed");
    if (token->slot != 0)
        return run.failed("fault_slot_not_targeted");
    if (accepted)
        return run.failed("refusal_fault_not_honoured");
    (void)run.observe_quiet(*token);
    if (run.terminals.count != 0 || run.arena.rejected_terminal_callbacks() != 0)
        return run.failed("terminal_after_refusal");
    // A provider that published into the reserved cell leaves the arena's
    // rollback pending and the slot pinned.
    if (run.arena.available_slots() != spec.slots)
        return run.failed("refused_slot_not_returned");
    run.finish_counts();
    if (!run.release_and_verify_quiescence())
        return run.result;
    run.pass("refusal_published_nothing");
    return run.result;
}

inline ScenarioResult terminal_busy_retry(const ProviderUnderTest& provider,
                                          const ProgramSpec& spec) {
    Run run(provider, spec, Scenario::TerminalBusyRetry);
    if (!provider.busy_retries) {
        run.result.verdict = Verdict::Skipped;
        run.result.reason = "busy_retry_unobservable";
        return run.result;
    }
    if (!run.create(Fault::HoldTerminalBusy) || !run.prepare(1))
        return run.result;
    bool accepted = false;
    const auto token = run.submit_block(1, 0, accepted);
    if (!token || !accepted)
        return run.failed("submit_failed");
    const auto error = run.await_output(*token, 0);
    if (!error)
        return run.failed("busy_terminal_not_retried");
    run.result.max_abs_error = *error;
    if (*error > spec.tolerance)
        return run.failed("output_mismatch");
    if (provider.busy_retries(*run.owned) == 0)
        return run.failed("busy_path_not_exercised");
    if (run.terminals.count_for(*token) != 1)
        return run.failed("terminal_not_exactly_once");
    run.finish_counts();
    if (!run.release_and_verify_quiescence())
        return run.result;
    run.pass("busy_terminal_retained_and_retried_once");
    return run.result;
}

// drain() publishes every in-flight terminal before it returns, and no
// callback follows it.
inline ScenarioResult teardown_quiescence(const ProviderUnderTest& provider,
                                          const ProgramSpec& spec) {
    Run run(provider, spec, Scenario::TeardownQuiescence);
    if (!run.create(Fault::None) || !run.prepare(spec.slots))
        return run.result;
    std::vector<SlotToken> inflight;
    for (std::uint32_t block = 0; block < spec.slots; ++block) {
        bool accepted = false;
        const auto token = run.submit_block(block + 1, block, accepted);
        if (!token || !accepted)
            return run.failed("submit_failed");
        inflight.push_back(*token);
    }
    // Tear down without servicing first.
    if (!run.retire_and_drain())
        return run.result;
    for (const auto& token : inflight) {
        if (run.terminals.count_for(token) != 1)
            return run.failed("terminal_after_drain_returned");
    }
    run.finish_counts();
    if (!run.release_and_verify_quiescence())
        return run.result;
    run.pass("drain_published_every_terminal_before_returning");
    return run.result;
}

inline ScenarioResult drain_after_loss(const ProviderUnderTest& provider, const ProgramSpec& spec) {
    Run run(provider, spec, Scenario::DrainAfterLoss);
    if (!run.create(Fault::ForceLossAfterCompletionRegistration) || !run.prepare(spec.slots))
        return run.result;
    const auto submitted_ns = steady_now_ns();
    std::vector<SlotToken> inflight;
    for (std::uint32_t block = 0; block < spec.slots; ++block) {
        bool accepted = false;
        const auto token = run.submit_block(block + 1, block, accepted);
        if (!token)
            return run.failed("grant_failed");
        if (accepted)
            inflight.push_back(*token);
    }
    if (inflight.empty())
        return run.failed("no_work_accepted_before_loss");
    for (const auto& token : inflight)
        (void)run.observe_quiet(token);
    if (run.terminals.count != 0)
        return run.failed("terminal_before_drain");
    if (!run.retire_and_drain())
        return run.failed("drain_failed_after_loss");
    for (const auto& token : inflight) {
        if (run.terminals.count_for(token) != 1)
            return run.failed("lost_work_not_terminal_exactly_once");
        if (run.terminals.status_for(token) != SharedIoTerminalStatus::RetiredFailed)
            return run.failed("success_after_loss");
        (void)run.arena.discard(token);
    }
    if (!run.recorder->device_lost())
        return run.failed("device_loss_not_reported");
    if (!run.verify_loss_record(submitted_ns))
        return run.failed(run.result.reason);
    if (run.recorder->can_resume_after_drain())
        return run.failed("resume_claimed_after_loss");
    run.finish_counts();
    if (!run.release_and_verify_quiescence())
        return run.result;
    if (run.recorder->can_resume_after_drain())
        return run.failed("resume_claimed_after_loss");
    run.pass("drained_lost_work_failed_and_not_resumable");
    return run.result;
}

// After a clean drain, a provider that claims it can resume must accept the
// next epoch and must never re-publish a previous epoch's terminal.
inline ScenarioResult stale_generation_replay(const ProviderUnderTest& provider,
                                              const ProgramSpec& spec) {
    Run run(provider, spec, Scenario::StaleGenerationReplay);
    if (!run.create(Fault::None) || !run.prepare(1))
        return run.result;
    bool accepted = false;
    const auto old = run.submit_block(1, 0, accepted);
    if (!old || !accepted)
        return run.failed("submit_failed");
    if (!run.await_output(*old, 0))
        return run.failed("completion_missing");
    if (run.recorder->submissions().empty() || !run.recorder->submissions().back().inbox)
        return run.failed("submission_not_recorded");
    const auto stale = run.recorder->submissions().back();

    // Re-prime the same arena after the drain boundary: the inbox persists,
    // the epoch advances, and only an affirmative resume proof reopens it.
    if (!run.retire_and_drain())
        return run.result;
    if (!run.recorder->can_resume_after_drain())
        return run.skipped("provider_cannot_resume_after_drain");
    if (!run.arena.reset_when_quiescent())
        return run.failed("reprime_failed");

    const auto current = run.submit_block(1, 1, accepted);
    if (!current)
        return run.failed("grant_after_reprime_failed");
    if (!accepted)
        return run.failed("resume_claim_not_honoured");
    const auto before = run.arena.rejected_terminal_callbacks();
    if (stale.inbox->push(stale.token, SharedIoTerminalStatus::RetiredSuccess) !=
        SharedIoTerminalInbox::PushResult::Rejected)
        return run.failed("stale_terminal_retired");
    const auto error = run.await_output(*current, 1);
    if (!error)
        return run.failed("current_completion_missing");
    run.result.max_abs_error = *error;
    if (*error > spec.tolerance)
        return run.failed("output_mismatch");
    if (run.terminals.count_for(stale.token) != 1 || run.terminals.count_for(*current) != 1)
        return run.failed("terminal_not_exactly_once");
    // Exactly the kit's one deliberate replay; the provider emitted none.
    if (run.arena.rejected_terminal_callbacks() != before + 1 || before != 0)
        return run.failed("provider_emitted_stale_callback");
    run.finish_counts();
    if (!run.release_and_verify_quiescence())
        return run.result;
    run.pass("resumed_epoch_without_stale_terminals");
    return run.result;
}

inline ScenarioResult differential_cpu(const ProviderUnderTest& provider, const ProgramSpec& spec) {
    Run run(provider, spec, Scenario::DifferentialCpuReference);
    if (!run.create(Fault::None) || !run.prepare(spec.slots))
        return run.result;
    double worst = 0.0;
    std::uint32_t next_block = 0;
    std::vector<std::pair<SlotToken, std::uint32_t>> inflight;
    // Keep every slot busy so the null also covers overlapping retirement.
    while (next_block < spec.blocks || !inflight.empty()) {
        while (next_block < spec.blocks && inflight.size() < spec.slots) {
            bool accepted = false;
            const auto token = run.submit_block(next_block + 1, next_block, accepted);
            if (!token || !accepted)
                return run.failed("submit_failed");
            inflight.emplace_back(*token, next_block++);
        }
        const auto [token, block] = inflight.front();
        inflight.erase(inflight.begin());
        const auto error = run.await_output(token, block);
        if (!error)
            return run.failed("completion_missing");
        worst = std::max(worst, *error);
    }
    run.result.max_abs_error = worst;
    if (worst > spec.tolerance)
        return run.failed("null_residual_exceeds_tolerance");
    run.finish_counts();
    if (!run.release_and_verify_quiescence())
        return run.result;
    run.pass("provider_nulls_against_cpu_reference");
    return run.result;
}

} // namespace detail

// ---------------------------------------------------------------------------
// Receipt

struct Receipt {
    std::string provider_name;
    SharedIoProviderIdentity identity;
    SharedIoProviderCapabilities capabilities;
    ProgramSpec program;
    std::vector<ScenarioResult> rows;
    std::size_t capacity = 32;
    std::size_t dropped_rows = 0;

    ProviderKind kind() const noexcept {
        return kind_of(identity);
    }
    // Rows beyond capacity are counted, never silently lost; a truncated
    // receipt is inadmissible.
    void add(ScenarioResult row) {
        if (rows.size() >= capacity) {
            ++dropped_rows;
            return;
        }
        rows.push_back(std::move(row));
    }
    bool truncated() const noexcept {
        return dropped_rows != 0;
    }
    bool identity_admissible() const noexcept {
        return kind() == ProviderKind::Fake ||
               (kind() == ProviderKind::Real && identity.authenticated);
    }
    bool passed() const noexcept {
        if (truncated() || !identity_admissible() || rows.empty())
            return false;
        return std::none_of(rows.begin(), rows.end(),
                            [](const auto& row) { return row.verdict == Verdict::Failed; });
    }

    std::string to_json() const;
    bool write(const std::string& path) const {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        if (!stream)
            return false;
        stream << to_json();
        return static_cast<bool>(stream);
    }
};

namespace detail {

inline std::string json_string(std::string_view value) {
    std::string out = "\"";
    for (const char ch : value) {
        switch (ch) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                char escaped[8];
                std::snprintf(escaped, sizeof(escaped), "\\u%04x",
                              static_cast<unsigned>(static_cast<unsigned char>(ch)));
                out += escaped;
            } else {
                out += ch;
            }
        }
    }
    out += '"';
    return out;
}

inline const char* json_bool(bool value) noexcept {
    return value ? "true" : "false";
}

inline std::string json_number(double value) {
    std::ostringstream stream;
    stream.precision(17);
    stream << value;
    return stream.str();
}

} // namespace detail

inline std::string Receipt::to_json() const {
    using detail::json_bool;
    using detail::json_number;
    using detail::json_string;
    std::ostringstream out;
    out << "{\"schema\":" << json_string(kReceiptSchema) << ",\"kit_version\":" << kKitVersion
        << ",\"provider\":{\"name\":" << json_string(provider_name)
        << ",\"kind\":" << json_string(kind_name(kind()))
        << ",\"identity\":{\"authenticated\":" << json_bool(identity.authenticated)
        << ",\"provider_revision\":" << json_string(identity.provider_revision)
        << ",\"adapter_name\":" << json_string(identity.adapter_name)
        << ",\"adapter_backend\":" << json_string(identity.adapter_backend)
        << ",\"adapter_vendor_id\":" << identity.adapter_vendor_id
        << ",\"adapter_device_id\":" << identity.adapter_device_id
        << ",\"native_runtime_authenticated\":" << json_bool(identity.native_runtime_authenticated)
        << ",\"native_runtime_name\":" << json_string(identity.native_runtime_name)
        << ",\"native_runtime_backend\":" << json_string(identity.native_runtime_backend)
        << "},\"capabilities\":{\"imported_host_pointer\":"
        << json_bool(capabilities.imported_host_pointer)
        << ",\"ordered_causal_state\":" << json_bool(capabilities.ordered_causal_state)
        << ",\"completion_service\":" << json_bool(capabilities.completion_service)
        << ",\"device_loss_recovery\":" << json_bool(capabilities.device_loss_recovery)
        << ",\"gpu_timestamps\":" << json_bool(capabilities.gpu_timestamps) << "}}"
        << ",\"program\":{\"name\":" << json_string(program.name)
        << ",\"scale\":" << json_number(program.scale) << ",\"bias\":" << json_number(program.bias)
        << ",\"frames\":" << program.frames << ",\"slots\":" << program.slots
        << ",\"blocks\":" << program.blocks << ",\"tolerance\":" << json_number(program.tolerance)
        << "},\"required_scenarios\":[";
    for (std::size_t index = 0; index < kProviderScenarios.size(); ++index)
        out << (index ? "," : "") << json_string(scenario_name(kProviderScenarios[index]));
    out << "],\"scenarios\":[";
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const auto& row = rows[index];
        out << (index ? "," : "") << "{\"name\":" << json_string(row.scenario)
            << ",\"verdict\":" << json_string(verdict_name(row.verdict))
            << ",\"reason\":" << json_string(row.reason) << ",\"submissions\":" << row.submissions
            << ",\"retired_success\":" << row.retired_success
            << ",\"retired_failure\":" << row.retired_failure
            << ",\"rejected_callbacks\":" << row.rejected_callbacks << ",\"max_abs_error\":";
        if (row.max_abs_error < 0.0 || !std::isfinite(row.max_abs_error))
            out << "null";
        else
            out << json_number(row.max_abs_error);
        out << "}";
    }
    out << "],\"row_count\":" << rows.size() << ",\"capacity\":" << capacity
        << ",\"truncated\":" << json_bool(truncated()) << ",\"dropped_rows\":" << dropped_rows
        << ",\"verdict\":" << json_string(passed() ? "passed" : "failed") << "}\n";
    return out.str();
}

namespace detail {

// A provider that installs a completion notifier wakes it for every accepted
// submission, by the time that submission's output is serviceable, and never
// after the notifier is cleared. A provider without a spontaneous source
// refuses the install, which conforms unless the provider claims one.
inline ScenarioResult completion_notifier(const ProviderUnderTest& provider,
                                          const ProgramSpec& spec) {
    Run run(provider, spec, Scenario::CompletionNotifier);
    if (!run.create(Fault::None) || !run.prepare(spec.slots))
        return run.result;
    struct Counter {
        std::atomic<std::uint64_t> wakes{0};
    } counter;
    const pulp::gpu_audio::detail::SharedIoCompletionNotifier notifier{
        [](void* context) noexcept {
            static_cast<Counter*>(context)->wakes.fetch_add(1, std::memory_order_relaxed);
        },
        &counter};
    if (!run.arena.set_completion_notifier(notifier)) {
        if (provider.expects_completion_notifier)
            return run.failed("notifier_not_installed");
        run.finish_counts();
        if (!run.release_and_verify_quiescence())
            return run.result;
        run.pass("no_spontaneous_completion_source");
        return run.result;
    }
    const auto blocks = std::max<std::uint32_t>(spec.slots * 2, 4);
    for (std::uint32_t block = 0; block < blocks; ++block) {
        bool accepted = false;
        const auto token = run.submit_block(block + 1, block, accepted);
        if (!token || !accepted)
            return run.failed("submit_failed");
        if (!run.await_output(*token, block))
            return run.failed("completion_missing");
        // The wake may trail the inbox by the backend's callback hop; bound it.
        const auto deadline = Clock::now() + provider.completion_timeout;
        while (counter.wakes.load(std::memory_order_relaxed) < block + 1 &&
               Clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        if (counter.wakes.load(std::memory_order_relaxed) < block + 1)
            return run.failed("notify_missing_for_submission");
    }
    if (!run.arena.set_completion_notifier({}))
        return run.failed("notifier_clear_refused");
    const auto wakes_at_clear = counter.wakes.load(std::memory_order_relaxed);
    bool accepted = false;
    const auto token = run.submit_block(blocks + 1, blocks, accepted);
    if (!token || !accepted)
        return run.failed("submit_failed_after_clear");
    if (!run.await_output(*token, blocks))
        return run.failed("completion_missing_after_clear");
    (void)run.observe_quiet(*token);
    if (counter.wakes.load(std::memory_order_relaxed) != wakes_at_clear)
        return run.failed("notify_after_clear");
    if (run.terminals.failures() != 0)
        return run.failed("unexpected_failed_terminal");
    run.finish_counts();
    if (!run.release_and_verify_quiescence())
        return run.result;
    run.pass("notifier_fires_per_submission_and_never_after_clear");
    return run.result;
}

} // namespace detail

// ---------------------------------------------------------------------------
// Entry points

inline ScenarioResult run_scenario(Scenario scenario, const ProviderUnderTest& provider,
                                   const ProgramSpec& spec) {
    switch (scenario) {
    case Scenario::Nominal:
        return detail::nominal(provider, spec);
    case Scenario::ForceLossBeforeSubmit:
    case Scenario::ForceLossBetweenSubmitAndRegistration:
    case Scenario::ForceLossAfterRegistration:
        return detail::force_loss(provider, spec, scenario);
    case Scenario::LateCompletion:
        return detail::late_completion(provider, spec);
    case Scenario::MissingCompletion:
        return detail::missing_completion(provider, spec);
    case Scenario::RefusedSubmission:
        return detail::refused_submission(provider, spec);
    case Scenario::TerminalBusyRetry:
        return detail::terminal_busy_retry(provider, spec);
    case Scenario::TeardownQuiescence:
        return detail::teardown_quiescence(provider, spec);
    case Scenario::DrainAfterLoss:
        return detail::drain_after_loss(provider, spec);
    case Scenario::StaleGenerationReplay:
        return detail::stale_generation_replay(provider, spec);
    case Scenario::DifferentialCpuReference:
        return detail::differential_cpu(provider, spec);
    case Scenario::CompletionNotifier:
        return detail::completion_notifier(provider, spec);
    }
    return {};
}

// Runs the same blocks through two providers. Each must null against the CPU
// reference, and the two must null against each other.
inline ScenarioResult run_differential_pair(const ProviderUnderTest& first,
                                            const ProviderUnderTest& second,
                                            const ProgramSpec& spec) {
    ScenarioResult result;
    result.scenario = std::string(kDifferentialPairScenario);
    bool skipped = false;
    auto collect = [&](const ProviderUnderTest& provider, std::vector<float>& samples,
                       std::string& reason) {
        detail::Run run(provider, spec, Scenario::DifferentialCpuReference);
        if (!run.create(Fault::None)) {
            skipped = true;
            reason = run.result.reason;
            return false;
        }
        if (!run.prepare(1)) {
            reason = run.result.reason;
            return false;
        }
        samples.reserve(static_cast<std::size_t>(spec.blocks) * spec.frames);
        for (std::uint32_t block = 0; block < spec.blocks; ++block) {
            bool accepted = false;
            const auto token = run.submit_block(block + 1, block, accepted);
            if (!token || !accepted) {
                reason = "submit_failed";
                return false;
            }
            const auto deadline = detail::Clock::now() + provider.completion_timeout;
            std::optional<SharedIoArena::OutputLease> output;
            while (!output && detail::Clock::now() < deadline) {
                run.service_once();
                output = run.arena.acquire_output(token->preparation_epoch, token->stream_sequence);
                if (!output)
                    std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
            if (!output) {
                reason = "completion_missing";
                return false;
            }
            for (std::uint32_t frame = 0; frame < spec.frames; ++frame) {
                float sample = 0.0f;
                std::memcpy(&sample, output->bytes.data() + frame * sizeof(float), sizeof(float));
                samples.push_back(sample);
            }
            const auto claimed = output->token;
            output.reset();
            if (!run.arena.release_output({claimed})) {
                reason = "output_release_failed";
                return false;
            }
            result.submissions += 1;
            result.retired_success += 1;
        }
        if (!run.arena.release()) {
            reason = "release_failed";
            return false;
        }
        return true;
    };
    auto cpu_error = [&](const std::vector<float>& samples) {
        double worst = 0.0;
        for (std::size_t index = 0; index < samples.size(); ++index) {
            const auto block = static_cast<std::uint32_t>(index / spec.frames);
            const auto frame = static_cast<std::uint32_t>(index % spec.frames);
            worst = std::max(worst, sample_error(samples[index], block, frame, spec));
        }
        return worst;
    };
    std::vector<float> a;
    std::vector<float> b;
    std::string reason;
    for (const auto* provider : {&first, &second}) {
        auto& samples = provider == &first ? a : b;
        if (!collect(*provider, samples, reason)) {
            result.verdict = skipped ? Verdict::Skipped : Verdict::Failed;
            result.reason = provider->name + ":" + reason;
            return result;
        }
    }
    const double first_cpu = cpu_error(a);
    const double second_cpu = cpu_error(b);
    double pair = a.size() == b.size() ? 0.0 : INFINITY;
    for (std::size_t index = 0; index < a.size() && index < b.size(); ++index) {
        if (!std::isfinite(a[index]) || !std::isfinite(b[index])) {
            pair = INFINITY;
            break;
        }
        pair = std::max(pair, std::abs(static_cast<double>(a[index]) - b[index]));
    }
    const double worst = std::max({first_cpu, second_cpu, pair});
    result.max_abs_error = std::isfinite(worst) ? worst : -1.0;
    result.verdict = Verdict::Failed;
    if (!(first_cpu <= spec.tolerance))
        result.reason = first.name + ":null_residual_exceeds_tolerance";
    else if (!(second_cpu <= spec.tolerance))
        result.reason = second.name + ":null_residual_exceeds_tolerance";
    else if (!(pair <= spec.tolerance))
        result.reason = "provider_pair_null_exceeds_tolerance";
    else {
        result.verdict = Verdict::Passed;
        result.reason = first.name + "_and_" + second.name + "_null_against_cpu_and_each_other";
    }
    return result;
}

// The optional completion-notifier scenario, for providers that may have a
// spontaneous completion source. Add its row to a receipt explicitly.
inline ScenarioResult run_completion_notifier(const ProviderUnderTest& provider,
                                              const ProgramSpec& spec) {
    return detail::completion_notifier(provider, spec);
}

// Runs every provider scenario and binds the results, with the provider's own
// identity, into a receipt. Identity is read from a fresh, unfaulted instance.
inline Receipt run_suite(const ProviderUnderTest& provider, const ProgramSpec& spec) {
    Receipt receipt{.provider_name = provider.name, .program = spec};
    if (auto created = provider.make(Fault::None); created.provider) {
        receipt.identity = created.provider->provider_identity();
        receipt.capabilities = created.provider->provider_capabilities();
    }
    for (const auto scenario : kProviderScenarios)
        receipt.add(run_scenario(scenario, provider, spec));
    return receipt;
}

} // namespace pulp::test::shared_io_conformance
