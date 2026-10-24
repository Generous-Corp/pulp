// Consumer-contract tests for the private render-loss cursor. Every input
// here is a Pulp-side fixture: no Vellum producer exists, so these tests prove
// Pulp's refusal logic only and say nothing about whether a real render loss
// is produced or reaches GPU audio. Producer halves are reported as
// SKIPPED-VELLUM-GATED with the reason.

#include "detail/render_loss_consumer.hpp"
#include "harness/rt_allocation_probe.hpp"

#include <catch2/catch_test_macros.hpp>
#include <choc/text/choc_JSON.h>

#include <cstdint>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

using namespace pulp::gpu_audio::detail;

namespace {

constexpr auto kU64Max = std::numeric_limits<std::uint64_t>::max();
constexpr const char* kFakeProvider = "fixture.fake-backend";
constexpr const char* kFakeProviderVersion = "fixture.fake-backend.v1";
constexpr const char* kOwnerA = "fixture.render-owner-A";
constexpr const char* kOwnerB = "fixture.render-owner-B";
constexpr const char* kManifestSchema = "fixture.resource-manifest.v1";

RenderOwnerIdentity identity(std::string_view provider, std::string_view version,
                             std::string_view owner) {
    return {RenderLossText::from(provider), RenderLossText::from(version),
            RenderLossText::from(owner)};
}

RenderOwnerIdentity owner_a() {
    return identity(kFakeProvider, kFakeProviderVersion, kOwnerA);
}
RenderOwnerIdentity owner_b() {
    return identity(kFakeProvider, kFakeProviderVersion, kOwnerB);
}

// Deterministic fixture fingerprint standing in for the producer's digests.
// It is not SHA-256 and is never cited as an execution digest.
RenderLossDigest fingerprint(const std::string& text) {
    RenderLossDigest out{};
    for (std::size_t lane = 0; lane < 4; ++lane) {
        std::uint64_t hash = 1469598103934665603ULL ^ (lane * 0x9e3779b97f4a7c15ULL);
        for (const unsigned char c : text) {
            hash ^= c;
            hash *= 1099511628211ULL;
        }
        for (std::size_t byte = 0; byte < 8; ++byte)
            out[lane * 8 + byte] = static_cast<std::uint8_t>(hash >> (byte * 8));
    }
    return out;
}

RenderResourceManifest manifest(std::string_view label = "M",
                                std::string_view schema = kManifestSchema) {
    return {RenderLossText::from(schema),
            fingerprint(std::string("manifest:") + std::string(label))};
}

std::string canonical(const RenderOwnerLossEvent& e) {
    std::ostringstream out;
    out << e.contract_version << '|' << e.identity.provider_id.view() << '|'
        << e.identity.provider_version.view() << '|' << e.identity.owner_id.view() << '|'
        << e.owner_generation << '|' << e.surface_epoch << '|' << e.event_id.view() << '|'
        << int(e.kind) << '|' << e.event_seq << '|' << int(e.reason) << '|' << int(e.disposition)
        << '|' << int(e.quiescence) << '|' << e.callbacks_suppressed << '|' << int(e.replay) << '|'
        << e.next_owner_generation << '|' << e.next_surface_epoch << '|'
        << e.resource_manifest.schema_version.view() << '|' << e.has_terminal << '|'
        << int(e.terminal.kind) << '|' << e.terminal.quiescent << '|'
        << e.terminal.callbacks_suppressed << '|' << int(e.terminal.replay);
    for (const auto b : e.resource_manifest.digest_sha256)
        out << int(b) << ',';
    return out.str();
}

void seal(RenderOwnerLossEvent& e) {
    e.payload_digest_sha256 = fingerprint(canonical(e));
}

RenderOwnerLossEvent loss(std::uint64_t seq, std::uint64_t gen, std::uint64_t ep,
                          std::string_view event_id = "fixture.event.E1") {
    RenderOwnerLossEvent e;
    e.identity = owner_a();
    e.event_seq = seq;
    e.owner_generation = gen;
    e.surface_epoch = ep;
    e.event_id = RenderLossText::from(event_id);
    e.kind = RenderOwnerLossEventKind::Loss;
    e.reason = RenderOwnerLossReason::DeviceLost;
    e.quiescence = RenderOwnerQuiescence::Draining;
    e.resource_manifest = manifest();
    seal(e);
    return e;
}

RenderOwnerLossEvent replay_success(std::uint64_t seq, std::uint64_t gen, std::uint64_t ep,
                                    std::uint64_t next_gen, std::uint64_t next_ep,
                                    std::string_view event_id = "fixture.event.E1") {
    auto e = loss(seq, gen, ep, event_id);
    e.kind = RenderOwnerLossEventKind::ReplayResult;
    e.replay = RenderOwnerReplay::Succeeded;
    e.next_owner_generation = next_gen;
    e.next_surface_epoch = next_ep;
    seal(e);
    return e;
}

void make_terminal(RenderOwnerLossEvent& e, RenderOwnerTerminalKind kind) {
    e.disposition = RenderOwnerLossDisposition::Terminal;
    e.quiescence = RenderOwnerQuiescence::Quiescent;
    e.callbacks_suppressed = true;
    e.has_terminal = true;
    e.terminal = {kind, true, true, e.replay};
    if (kind == RenderOwnerTerminalKind::OwnerTeardown)
        e.reason = RenderOwnerLossReason::OwnerTeardown;
    seal(e);
}

RenderLossConsumer& installed(RenderLossConsumer& consumer) {
    REQUIRE(consumer.install(owner_a()).accepted());
    return consumer;
}

// ---------------------------------------------------------------------------
// Trace replay shared by the v1 adapter and the package vectors.

std::string read_fixture(const std::string& relative) {
    std::ifstream in(std::string(PULP_REPO_ROOT) + "/" + relative, std::ios::binary);
    REQUIRE(in.good());
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

std::string_view outcome_name(RenderLossOutcome outcome) {
    switch (outcome) {
    case RenderLossOutcome::Accepted:
        return "accepted";
    case RenderLossOutcome::IgnoredDuplicate:
        return "ignored_duplicate";
    case RenderLossOutcome::IgnoredStale:
        return "ignored_stale";
    case RenderLossOutcome::Rejected:
        return "rejected";
    case RenderLossOutcome::RejectedFailClosed:
        return "rejected_fail_closed";
    }
    return "invalid";
}

std::string_view admission_name(RenderLossAdmission admission) {
    switch (admission) {
    case RenderLossAdmission::Open:
        return "open";
    case RenderLossAdmission::Fenced:
        return "fenced";
    case RenderLossAdmission::Closed:
        return "closed";
    }
    return "invalid";
}

std::string_view closure_name(RenderLossCursorClosure closure) {
    switch (closure) {
    case RenderLossCursorClosure::Open:
        return "open";
    case RenderLossCursorClosure::RetiredByConsumer:
        return "retired_by_consumer";
    case RenderLossCursorClosure::Terminal:
        return "terminal";
    case RenderLossCursorClosure::ResyncRequired:
        return "resync_required";
    }
    return "invalid";
}

std::string_view fallback_name(pulp::gpu_audio::detail::SharedIoFallbackReason reason) {
    using R = pulp::gpu_audio::detail::SharedIoFallbackReason;
    if (reason == R::None)
        return "none";
    if (reason == R::DeviceLost)
        return "device_lost";
    if (reason == R::Teardown)
        return "teardown";
    return "other";
}

RenderOwnerQuiescence quiescence_from(const std::string& text) {
    if (text == "active")
        return RenderOwnerQuiescence::Active;
    if (text == "draining")
        return RenderOwnerQuiescence::Draining;
    if (text == "quiescent")
        return RenderOwnerQuiescence::Quiescent;
    FAIL("unknown quiescence state in fixture: " << text);
    return RenderOwnerQuiescence::Active;
}

RenderOwnerReplay replay_from(const std::string& text) {
    if (text == "not_attempted")
        return RenderOwnerReplay::NotAttempted;
    if (text == "succeeded")
        return RenderOwnerReplay::Succeeded;
    if (text == "failed")
        return RenderOwnerReplay::Failed;
    FAIL("unknown replay value in fixture: " << text);
    return RenderOwnerReplay::NotAttempted;
}

RenderOwnerTerminalKind terminal_kind_from(const std::string& text) {
    if (text == "device_lost")
        return RenderOwnerTerminalKind::DeviceLost;
    if (text == "replay_failed")
        return RenderOwnerTerminalKind::ReplayFailed;
    if (text == "provider_refused")
        return RenderOwnerTerminalKind::ProviderRefused;
    if (text == "owner_teardown")
        return RenderOwnerTerminalKind::OwnerTeardown;
    FAIL("unknown terminal kind in fixture: " << text);
    return RenderOwnerTerminalKind::DeviceLost;
}

RenderOwnerLossReason reason_from(const std::string& text) {
    if (text == "device_lost")
        return RenderOwnerLossReason::DeviceLost;
    if (text == "driver_reset")
        return RenderOwnerLossReason::DriverReset;
    if (text == "out_of_memory")
        return RenderOwnerLossReason::OutOfMemory;
    if (text == "provider_refused")
        return RenderOwnerLossReason::ProviderRefused;
    if (text == "owner_teardown")
        return RenderOwnerLossReason::OwnerTeardown;
    if (text == "unknown")
        return RenderOwnerLossReason::Unknown;
    FAIL("unknown reason in fixture: " << text);
    return RenderOwnerLossReason::Unknown;
}

std::uint64_t u64_from(const choc::value::ValueView& v) {
    if (v.isString()) {
        REQUIRE(v.toString() == "u64max");
        return kU64Max;
    }
    REQUIRE(v.isInt());
    const auto n = v.get<std::int64_t>();
    REQUIRE(n >= 0);
    return static_cast<std::uint64_t>(n);
}

std::string str_or(const choc::value::ValueView& object, const char* key, std::string fallback) {
    if (object.hasObjectMember(key) && object[key].isString())
        return object[key].toString();
    return fallback;
}

struct CursorView {
    RenderOwnerIdentity identity;
    std::uint64_t seq = 0;
    std::uint64_t gen = 0;
    std::uint64_t ep = 0;
    RenderLossCursorClosure closure = RenderLossCursorClosure::Open;
    RenderLossAdmission admission = RenderLossAdmission::Closed;

    static CursorView of(const RenderLossConsumer& c) {
        return {c.identity(),           c.last_event_seq(), c.last_owner_generation(),
                c.last_surface_epoch(), c.closure(),        c.admission()};
    }
    bool operator==(const CursorView&) const = default;
};

// ---------------------------------------------------------------------------
// Package vectors (Pulp-side transcription).

RenderOwnerIdentity package_identity(const choc::value::ValueView& op) {
    const auto provider = str_or(op, "provider", "fake");
    const auto owner = str_or(op, "owner", "A");
    std::string provider_id = kFakeProvider;
    std::string provider_version = kFakeProviderVersion;
    if (provider == "other") {
        provider_id = "fixture.other-provider";
        provider_version = "fixture.other-provider.v1";
    }
    return identity(provider_id, provider_version, owner == "B" ? kOwnerB : kOwnerA);
}

RenderOwnerLossEvent package_event(const choc::value::ValueView& op, bool replay_result) {
    RenderOwnerLossEvent e;
    if (op.hasObjectMember("contract_version"))
        e.contract_version = static_cast<std::uint32_t>(u64_from(op["contract_version"]));
    e.identity = package_identity(op);
    e.event_seq = u64_from(op["seq"]);
    e.owner_generation = u64_from(op["gen"]);
    e.surface_epoch = u64_from(op["ep"]);
    e.event_id = RenderLossText::from("fixture.event." + op["event"].toString());
    e.kind =
        replay_result ? RenderOwnerLossEventKind::ReplayResult : RenderOwnerLossEventKind::Loss;
    e.reason = reason_from(str_or(op, "reason", "device_lost"));
    e.disposition = str_or(op, "disposition", "recoverable") == "terminal"
                        ? RenderOwnerLossDisposition::Terminal
                        : RenderOwnerLossDisposition::Recoverable;
    e.quiescence = quiescence_from(str_or(op, "quiescence", "draining"));
    e.callbacks_suppressed = e.quiescence == RenderOwnerQuiescence::Quiescent;
    e.replay = replay_from(str_or(op, "replay", "not_attempted"));
    if (op.hasObjectMember("next_gen"))
        e.next_owner_generation = u64_from(op["next_gen"]);
    if (op.hasObjectMember("next_ep"))
        e.next_surface_epoch = u64_from(op["next_ep"]);
    e.resource_manifest =
        manifest(str_or(op, "manifest", "M"), str_or(op, "manifest_schema", kManifestSchema));
    if (op.hasObjectMember("terminal")) {
        const auto t = op["terminal"];
        e.has_terminal = true;
        e.terminal.kind = terminal_kind_from(str_or(t, "kind", "device_lost"));
        e.terminal.quiescent = e.quiescence == RenderOwnerQuiescence::Quiescent;
        e.terminal.callbacks_suppressed = e.callbacks_suppressed;
        e.terminal.replay = replay_from(str_or(t, "replay", str_or(op, "replay", "not_attempted")));
    }
    seal(e);
    return e;
}

void check_expect(const choc::value::ValueView& expect, const RenderLossVerdict& verdict,
                  const RenderLossConsumer& consumer) {
    const auto status = consumer.status();
    for (std::uint32_t i = 0; i < expect.size(); ++i) {
        const auto member = expect.getObjectMemberAt(i);
        const std::string key = member.name;
        INFO("expect." << key);
        if (key == "outcome") {
            CHECK(outcome_name(verdict.outcome) == member.value.toString());
        } else if (key == "refusal") {
            CHECK(to_string(verdict.refusal) == member.value.toString());
        } else if (key == "admission") {
            CHECK(admission_name(consumer.admission()) == member.value.toString());
        } else if (key == "replay_pending") {
            CHECK(status.replay_pending == member.value.getBool());
        } else if (key == "lost_generation_quiescent") {
            CHECK(status.lost_generation_quiescent == member.value.getBool());
        } else {
            FAIL("unhandled package expectation key: " << key);
        }
    }
}

void check_final(const choc::value::ValueView& final_state, const RenderLossConsumer& consumer) {
    const auto status = consumer.status();
    for (std::uint32_t i = 0; i < final_state.size(); ++i) {
        const auto member = final_state.getObjectMemberAt(i);
        const std::string key = member.name;
        INFO("final." << key);
        if (key == "owner") {
            CHECK(consumer.identity().owner_id.view() ==
                  (member.value.toString() == "B" ? kOwnerB : kOwnerA));
        } else if (key == "seq") {
            CHECK(consumer.last_event_seq() == u64_from(member.value));
        } else if (key == "gen") {
            CHECK(consumer.last_owner_generation() == u64_from(member.value));
        } else if (key == "ep") {
            CHECK(consumer.last_surface_epoch() == u64_from(member.value));
        } else if (key == "closure") {
            CHECK(closure_name(consumer.closure()) == member.value.toString());
        } else if (key == "admission") {
            CHECK(admission_name(consumer.admission()) == member.value.toString());
        } else if (key == "fence_transitions") {
            CHECK(status.fence_transitions == u64_from(member.value));
        } else if (key == "reopen_transitions") {
            CHECK(status.reopen_transitions == u64_from(member.value));
        } else if (key == "fallback_reason") {
            CHECK(fallback_name(status.loss_fallback_reason) == member.value.toString());
        } else {
            FAIL("unhandled package final key: " << key);
        }
    }
}

void replay_package_vector(const choc::value::ValueView& vector) {
    RenderLossConsumer consumer;
    installed(consumer);
    const auto trace = vector["trace"];
    for (std::uint32_t i = 0; i < trace.size(); ++i) {
        const auto op = trace[i];
        const auto kind = op["op"].toString();
        INFO("op " << i + 1 << ": " << kind);
        RenderLossVerdict verdict;
        if (kind == "loss" || kind == "replay_result") {
            verdict = consumer.apply(package_event(op, kind == "replay_result"));
        } else if (kind == "completion") {
            verdict = consumer.classify_completion(package_identity(op), u64_from(op["gen"]));
        } else if (kind == "handoff") {
            RenderOwnerHandoff transaction;
            transaction.from = op["from"].toString() == "B" ? owner_b() : owner_a();
            transaction.to = op["to"].toString() == "B" ? owner_b() : owner_a();
            verdict = consumer.handoff(transaction);
        } else if (kind == "install") {
            verdict = consumer.install(package_identity(op));
        } else {
            FAIL("unknown package op: " << kind);
        }
        check_expect(op["expect"], verdict, consumer);
    }
    check_final(vector["final"], consumer);
}

// ---------------------------------------------------------------------------
// v1 vectors. The v1 rows predate the package, so the fake producer below
// translates them under the package's corrections to v1:
// - render_generation is owner_generation. A succeeded replay_result row
//   carries the post-replay generation and epoch; section 3 moves those to
//   next_owner_generation / next_surface_epoch and binds the row's own
//   generation and epoch to the loss it answers.
// - reason is absent in v1. It is derived from the terminal kind
//   (owner_teardown, provider_refused) and is device_lost otherwise; the v1
//   value software_provider_refused is outside the closed enum and maps to
//   provider_refused.
// - Event ids and digests are filled with deterministic fixture values. The
//   transcription names fixture digests by label (D1, M1, ...), which the
//   test turns into fingerprints. A replay_result reuses its loss's id.
// - A replay_result row without quiescence inherits the producer's state:
//   the last published quiescence, or quiescent after reach_quiescent.
// - rejected_event counts the consumer-visible steps of the trace
//   (publish_loss_event, publish_replay_result, owner_handoff, teardown_owner,
//   invoke_completion_callback); producer-internal steps are not counted.

struct V1Run {
    RenderLossConsumer consumer;
    RenderOwnerQuiescence producer_quiescence = RenderOwnerQuiescence::Active;
    std::uint64_t loss_generation = 0;
    std::uint64_t loss_epoch = 0;
    std::string loss_event_id;
    int step = 0;
    std::optional<int> first_rejected_step;
    std::string first_rejected_reason;
    std::optional<CursorView> cursor_before_rejection;
    std::optional<CursorView> cursor_after_rejection;
    std::vector<RenderLossVerdict> publish_verdicts;
    std::vector<RenderLossVerdict> handoff_verdicts;
    std::vector<RenderLossVerdict> completion_verdicts;
    std::vector<RenderLossVerdict> retire_verdicts;
    std::vector<RenderLossVerdict> registered_completion_verdicts;
    std::optional<RenderOwnerReplay> accepted_replay;
    std::vector<std::string> producer_trace;

    void note(const RenderLossVerdict& verdict, const CursorView& before) {
        const bool refused = verdict.outcome == RenderLossOutcome::Rejected ||
                             verdict.outcome == RenderLossOutcome::RejectedFailClosed ||
                             verdict.outcome == RenderLossOutcome::IgnoredStale;
        if (refused && !first_rejected_step) {
            first_rejected_step = step;
            first_rejected_reason = std::string(to_string(verdict.refusal));
            cursor_before_rejection = before;
            cursor_after_rejection = CursorView::of(consumer);
        }
    }
};

RenderOwnerIdentity v1_identity(const choc::value::ValueView& row,
                                const choc::value::ValueView& fixture) {
    return identity(str_or(row, "provider_id", fixture["provider_id"].toString()),
                    str_or(row, "provider_version", fixture["provider_version"].toString()),
                    str_or(row, "owner_id", fixture["owner_id"].toString()));
}

RenderOwnerLossEvent v1_event(V1Run& run, const choc::value::ValueView& row,
                              const choc::value::ValueView& fixture) {
    RenderOwnerLossEvent e;
    e.contract_version = static_cast<std::uint32_t>(u64_from(row["contract_version"]));
    e.identity = v1_identity(row, fixture);
    e.event_seq = u64_from(row["event_seq"]);
    const bool replay_result = row["event_kind"].toString() == "replay_result";
    e.kind =
        replay_result ? RenderOwnerLossEventKind::ReplayResult : RenderOwnerLossEventKind::Loss;
    e.disposition = str_or(row, "disposition", "recoverable") == "terminal"
                        ? RenderOwnerLossDisposition::Terminal
                        : RenderOwnerLossDisposition::Recoverable;
    e.replay = replay_from(row["replay"].toString());
    const auto row_generation = u64_from(row["render_generation"]);
    const auto row_epoch = u64_from(row["surface_epoch"]);

    if (row.hasObjectMember("quiescence")) {
        e.quiescence = quiescence_from(row["quiescence"].toString());
        if (e.quiescence > run.producer_quiescence)
            run.producer_quiescence = e.quiescence;
    } else {
        e.quiescence = run.producer_quiescence;
    }
    e.callbacks_suppressed = e.quiescence == RenderOwnerQuiescence::Quiescent;

    if (replay_result) {
        e.event_id = RenderLossText::from(run.loss_event_id);
        e.owner_generation = run.loss_generation;
        e.surface_epoch = run.loss_epoch;
        if (e.replay == RenderOwnerReplay::Succeeded) {
            e.next_owner_generation = row_generation;
            e.next_surface_epoch = row_epoch;
        } else {
            e.owner_generation = row_generation;
            e.surface_epoch = row_epoch;
        }
    } else {
        e.owner_generation = row_generation;
        e.surface_epoch = row_epoch;
        run.loss_event_id = "v1." + std::string(e.identity.owner_id.view()) + ".loss." +
                            std::to_string(e.event_seq);
        e.event_id = RenderLossText::from(run.loss_event_id);
        run.loss_generation = row_generation;
        run.loss_epoch = row_epoch;
    }

    if (row.hasObjectMember("terminal")) {
        const auto t = row["terminal"];
        e.has_terminal = true;
        e.terminal.kind = terminal_kind_from(str_or(t, "kind", "device_lost"));
        e.terminal.quiescent = t["quiescent"].getBool();
        e.terminal.callbacks_suppressed = t["callbacks_suppressed"].getBool();
        e.terminal.replay = replay_from(str_or(t, "replay", row["replay"].toString()));
        e.callbacks_suppressed = e.terminal.callbacks_suppressed;
    }
    e.reason = RenderOwnerLossReason::DeviceLost;
    if (e.has_terminal && e.terminal.kind == RenderOwnerTerminalKind::OwnerTeardown)
        e.reason = RenderOwnerLossReason::OwnerTeardown;
    if (e.has_terminal && e.terminal.kind == RenderOwnerTerminalKind::ProviderRefused)
        e.reason = RenderOwnerLossReason::ProviderRefused;
    if (row.hasObjectMember("reason")) {
        const auto text = row["reason"].toString();
        e.reason = text == "software_provider_refused" ? RenderOwnerLossReason::ProviderRefused
                                                       : reason_from(text);
    }

    e.resource_manifest.schema_version =
        RenderLossText::from(fixture["resource_manifest_schema_version"].toString());
    e.resource_manifest.digest_sha256 = fingerprint("manifest:" + str_or(row, "manifest", "v1-M"));
    if (row.hasObjectMember("payload_digest"))
        e.payload_digest_sha256 = fingerprint("payload:" + row["payload_digest"].toString());
    else
        seal(e);
    return e;
}

// OPEN CONTRACT QUESTION for Vellum. The package says the software provider in
// v1:positive.terminal.software-provider-refused is installed by H9, but the
// vector keeps owner_id fixture.render-owner-A, while section 3 and H11 say an
// owner_id is never reused once its cursor closes. The consumer follows
// section 3/H11 and refuses that handoff; the vector itself is not edited to
// pick a side. Its consumer half runs with the software provider installed
// directly, and the case is listed in the SKIPPED-VELLUM-GATED report.
constexpr const char* kSoftwareProviderCase = "positive.terminal.software-provider-refused";

RenderOwnerIdentity software_provider_a() {
    return identity("fixture.software-provider", "fixture.software-provider.v1", kOwnerA);
}

void run_v1_trace(V1Run& run, const choc::value::ValueView& c,
                  const choc::value::ValueView& fixture) {
    const auto trace = c["input_trace"];
    for (std::uint32_t i = 0; i < trace.size(); ++i) {
        const auto row = trace[i];
        const auto op = row["op"].toString();
        INFO("trace step " << i + 1 << ": " << op);
        const auto before = CursorView::of(run.consumer);
        if (op == "publish_loss_event" || op == "publish_replay_result") {
            ++run.step;
            const auto verdict = run.consumer.apply(v1_event(run, row, fixture));
            run.publish_verdicts.push_back(verdict);
            if (verdict.accepted() && op == "publish_replay_result")
                run.accepted_replay = replay_from(row["replay"].toString());
            run.note(verdict, before);
        } else if (op == "owner_handoff") {
            ++run.step;
            RenderOwnerHandoff transaction;
            transaction.contract_version =
                static_cast<std::uint32_t>(u64_from(row["contract_version"]));
            transaction.from =
                identity(row["from_provider_id"].toString(),
                         row["from_provider_version"].toString(), row["from_owner_id"].toString());
            transaction.to =
                identity(row["to_provider_id"].toString(), row["to_provider_version"].toString(),
                         row["to_owner_id"].toString());
            const auto verdict = run.consumer.handoff(transaction);
            run.handoff_verdicts.push_back(verdict);
            run.note(verdict, before);
        } else if (op == "teardown_owner") {
            ++run.step;
            run.producer_trace.push_back(op);
        } else if (op == "invoke_completion_callback" || op == "retire_completion") {
            if (op == "invoke_completion_callback")
                ++run.step;
            const auto generation = row.hasObjectMember("generation") ? u64_from(row["generation"])
                                                                      : run.loss_generation;
            const auto verdict =
                run.consumer.classify_completion(run.consumer.identity(), generation);
            (op == "invoke_completion_callback" ? run.completion_verdicts : run.retire_verdicts)
                .push_back(verdict);
            if (op == "invoke_completion_callback")
                run.note(verdict, before);
        } else if (op == "register_completion_callback") {
            // Only a registration after the fence asks the consumer anything.
            if (row.hasObjectMember("expected")) {
                run.registered_completion_verdicts.push_back(
                    run.consumer.classify_completion(run.consumer.identity(), run.loss_generation));
            }
            run.producer_trace.push_back(op);
        } else if (op == "fence_gpu_admission") {
            CHECK(run.consumer.admission() != RenderLossAdmission::Open);
            CHECK(run.consumer.admission_ticket() == 0);
        } else if (op == "resume_gpu_admission") {
            CHECK(run.consumer.admission() == RenderLossAdmission::Open);
            CHECK(run.consumer.admission_ticket_current(run.consumer.admission_ticket()));
        } else if (op == "close_cursor") {
            run.consumer.close();
        } else if (op == "reach_quiescent") {
            run.producer_quiescence = RenderOwnerQuiescence::Quiescent;
            run.producer_trace.push_back(op);
        } else if (op == "inject_device_loss" || op == "submit_frame" ||
                   op == "replay_from_immutable_manifest" || op == "mutate_resource_manifest" ||
                   op == "suppress_callbacks") {
            run.producer_trace.push_back(op);
        } else {
            FAIL("unknown v1 op: " << op);
        }
    }
}

// Expectation keys that only a Vellum producer can satisfy.
const std::map<std::string, std::string>& v1_vellum_gated_keys() {
    static const std::map<std::string, std::string> keys = {
        {"callback_ordering",
         "loss < replay < resume, and cb1 never invoked with a gen-7 tag after the fence, is an "
         "ordering property of the owner's own trace."},
        {"audio_progress",
         "Monotonic audio progress through a render loss needs a real producer and an audio run "
         "(package C6/C7)."},
    };
    return keys;
}

void check_v1_expected(V1Run& run, const choc::value::ValueView& c) {
    const auto id = c["id"].toString();
    const auto expected = c["expected"];
    const auto& consumer = run.consumer;
    for (std::uint32_t i = 0; i < expected.size(); ++i) {
        const auto member = expected.getObjectMemberAt(i);
        const std::string key = member.name;
        const auto value = member.value;
        INFO(id << " expected." << key);
        if (v1_vellum_gated_keys().count(key) != 0) {
            // Reported by the SKIPPED-VELLUM-GATED case.
            continue;
        } else if (key == "cursor") {
            if (value.hasObjectMember("provider_id"))
                CHECK(consumer.identity().provider_id.view() == value["provider_id"].toString());
            if (value.hasObjectMember("owner_id"))
                CHECK(consumer.identity().owner_id.view() == value["owner_id"].toString());
            CHECK(consumer.last_event_seq() == u64_from(value["last_event_seq"]));
            CHECK((consumer.closure() != RenderLossCursorClosure::Open) ==
                  value["closed"].getBool());
        } else if (key == "disposition") {
            const auto text = value.toString();
            if (text == "rejected") {
                CHECK(run.first_rejected_step);
            } else if (text == "terminal") {
                CHECK(consumer.closure() == RenderLossCursorClosure::Terminal);
            } else {
                REQUIRE(text == "recoverable");
                CHECK(consumer.closure() == RenderLossCursorClosure::Open);
            }
        } else if (key == "replay") {
            const auto replay = replay_from(value.toString());
            if (replay == RenderOwnerReplay::NotAttempted)
                CHECK_FALSE(run.accepted_replay);
            else
                CHECK(run.accepted_replay == replay);
        } else if (key == "admission") {
            const auto text = value.toString();
            if (text == "reopened_only_after_replay") {
                CHECK(consumer.admission() == RenderLossAdmission::Open);
                CHECK(consumer.status().fence_transitions == 1);
                CHECK(consumer.status().reopen_transitions == 1);
            } else if (text == "permanently_closed") {
                CHECK(consumer.admission() == RenderLossAdmission::Closed);
                CHECK(consumer.closure() == RenderLossCursorClosure::Terminal);
            } else if (text == "remains_fenced" || text == "fenced_on_new_owner_loss") {
                CHECK(consumer.admission() == RenderLossAdmission::Fenced);
            } else {
                FAIL("unknown v1 admission expectation: " << text);
            }
        } else if (key == "callbacks_suppressed") {
            // Consumer half: once the terminal event is applied, no completion
            // from the owner can retire audio work.
            REQUIRE(value.getBool());
            const auto verdict = run.consumer.classify_completion(consumer.identity(),
                                                                  consumer.last_owner_generation());
            CHECK(verdict.refusal == RenderLossRefusal::CallbackAfterTeardown);
        } else if (key == "stale_completion_retired") {
            REQUIRE_FALSE(value.getBool());
            REQUIRE(run.registered_completion_verdicts.size() == 1);
            CHECK_FALSE(run.registered_completion_verdicts.front().accepted());
        } else if (key == "first_event") {
            REQUIRE(value.toString() == "accepted");
            REQUIRE_FALSE(run.publish_verdicts.empty());
            CHECK(run.publish_verdicts.front().accepted());
        } else if (key == "duplicate_event") {
            REQUIRE(value.toString() == "ignored_idempotently");
            REQUIRE(run.publish_verdicts.size() == 2);
            CHECK(run.publish_verdicts[1].outcome == RenderLossOutcome::IgnoredDuplicate);
            CHECK(consumer.status().fence_transitions == 1);
        } else if (key == "handoff") {
            REQUIRE(value.toString() == "accepted_atomically");
            REQUIRE(run.handoff_verdicts.size() == 1);
            CHECK(run.handoff_verdicts.front().accepted());
        } else if (key == "new_owner_event" || key == "old_owner_event") {
            REQUIRE(run.publish_verdicts.size() == 2);
            const auto& verdict = run.publish_verdicts[key == "new_owner_event" ? 0 : 1];
            CHECK(verdict.accepted() == (value.toString() == "accepted"));
            if (key == "old_owner_event")
                CHECK(verdict.refusal == RenderLossRefusal::OwnerReuseWithoutExplicitHandoff);
        } else if (key == "rejected_event") {
            REQUIRE(run.first_rejected_step);
            CHECK(*run.first_rejected_step == static_cast<int>(u64_from(value)));
        } else if (key == "reason") {
            CHECK(run.first_rejected_reason == value.toString());
        } else if (key == "cursor_unchanged") {
            REQUIRE(value.getBool());
            REQUIRE(run.cursor_before_rejection);
            CHECK(*run.cursor_before_rejection == *run.cursor_after_rejection);
        } else if (key == "cursor_closed" || key == "cursor_closed_or_resync_required") {
            REQUIRE(value.getBool());
            CHECK(consumer.closure() != RenderLossCursorClosure::Open);
            CHECK(consumer.admission() == RenderLossAdmission::Closed);
        } else if (key == "callback_invoked") {
            REQUIRE_FALSE(value.getBool());
            REQUIRE_FALSE(run.completion_verdicts.empty());
            for (const auto& verdict : run.completion_verdicts)
                CHECK_FALSE(verdict.accepted());
        } else if (key == "retirement_accepted") {
            REQUIRE_FALSE(value.getBool());
            REQUIRE_FALSE(run.retire_verdicts.empty());
            for (const auto& verdict : run.retire_verdicts)
                CHECK_FALSE(verdict.accepted());
        } else if (key == "replay_accepted") {
            REQUIRE_FALSE(value.getBool());
            CHECK_FALSE(run.accepted_replay);
        } else {
            FAIL("unhandled v1 expectation key: " << key);
        }
    }
}

// Package section 5.1 adds generation and epoch to the cursors of the
// recoverable positives; v1 rows have none.
void check_package_cursor_for_v1(const V1Run& run, const std::string& id) {
    if (id.rfind("positive.recoverable.replay-success.", 0) == 0) {
        CHECK(run.consumer.last_owner_generation() == 8);
        CHECK(run.consumer.last_surface_epoch() == 4);
    }
}

template <class T>
concept HasReset = requires(T& t) { t.reset(); };
template <class T>
concept HasClear = requires(T& t) { t.clear(); };

} // namespace

// ---------------------------------------------------------------------------

TEST_CASE("render-loss consumer offers no implicit reset path", "[gpu_audio][render_loss]") {
    STATIC_REQUIRE_FALSE(std::is_copy_assignable_v<RenderLossConsumer>);
    STATIC_REQUIRE_FALSE(std::is_move_assignable_v<RenderLossConsumer>);
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<RenderLossConsumer>);
    STATIC_REQUIRE_FALSE(HasReset<RenderLossConsumer>);
    STATIC_REQUIRE_FALSE(HasClear<RenderLossConsumer>);

    RenderLossConsumer consumer;
    installed(consumer);
    REQUIRE(consumer.apply(loss(1, 7, 3)).accepted());
    // H10: a second install (process or surface re-creation calling it again)
    // never resets the cursor.
    const auto again = consumer.install(owner_a());
    CHECK(again.refusal == RenderLossRefusal::ImplicitResetForbidden);
    const auto other = consumer.install(owner_b());
    CHECK(other.refusal == RenderLossRefusal::ImplicitResetForbidden);
    CHECK(consumer.last_event_seq() == 1);
    CHECK(consumer.identity() == owner_a());
    CHECK(consumer.admission() == RenderLossAdmission::Fenced);
}

TEST_CASE("H1 contiguous events advance the whole cursor and fence then reopen",
          "[gpu_audio][render_loss]") {
    RenderLossConsumer consumer;
    installed(consumer);
    const auto open_ticket = consumer.admission_ticket();
    REQUIRE(consumer.admission_ticket_current(open_ticket));

    REQUIRE(consumer.apply(loss(1, 7, 3)).accepted());
    CHECK(consumer.admission() == RenderLossAdmission::Fenced);
    CHECK(consumer.admission_ticket() == 0);
    CHECK_FALSE(consumer.admission_ticket_current(open_ticket));
    CHECK(consumer.status().replay_pending);
    CHECK(consumer.status().loss_fallback_reason == SharedIoFallbackReason::DeviceLost);

    REQUIRE(consumer.apply(replay_success(2, 7, 3, 8, 4)).accepted());
    CHECK(consumer.admission() == RenderLossAdmission::Open);
    const auto reopened = consumer.admission_ticket();
    CHECK(reopened != 0);
    CHECK(reopened != open_ticket);
    // A ticket captured before the loss never becomes current again.
    CHECK_FALSE(consumer.admission_ticket_current(open_ticket));
    CHECK(consumer.last_event_seq() == 2);
    CHECK(consumer.last_owner_generation() == 8);
    CHECK(consumer.last_surface_epoch() == 4);
}

TEST_CASE("H2 exact duplicate is a no-op and H6 conflicting duplicate fails closed",
          "[gpu_audio][render_loss]") {
    SECTION("positive control: identical retransmit") {
        RenderLossConsumer consumer;
        installed(consumer);
        const auto event = loss(1, 7, 3);
        REQUIRE(consumer.apply(event).accepted());
        const auto before = CursorView::of(consumer);
        const auto verdict = consumer.apply(event);
        CHECK(verdict.outcome == RenderLossOutcome::IgnoredDuplicate);
        CHECK(CursorView::of(consumer) == before);
        CHECK(consumer.status().fence_transitions == 1);
        CHECK(consumer.status().duplicates_ignored == 1);
    }
    SECTION("same digest, different payload is a conflict") {
        RenderLossConsumer consumer;
        installed(consumer);
        const auto event = loss(1, 7, 3);
        REQUIRE(consumer.apply(event).accepted());
        auto forged = event;
        forged.surface_epoch = 99; // digest deliberately left as the original's
        const auto verdict = consumer.apply(forged);
        CHECK(verdict.outcome == RenderLossOutcome::RejectedFailClosed);
        CHECK(verdict.refusal == RenderLossRefusal::ConflictingPayloadForSequence);
        CHECK(consumer.closure() == RenderLossCursorClosure::ResyncRequired);
        CHECK(consumer.admission() == RenderLossAdmission::Closed);
    }
    SECTION("same payload, different digest is a conflict") {
        RenderLossConsumer consumer;
        installed(consumer);
        const auto event = loss(1, 7, 3);
        REQUIRE(consumer.apply(event).accepted());
        auto forged = event;
        forged.payload_digest_sha256[0] ^= 0x01;
        CHECK(consumer.apply(forged).refusal == RenderLossRefusal::ConflictingPayloadForSequence);
    }
}

TEST_CASE("H3 stale sequence is ignored and H5 gap fails closed", "[gpu_audio][render_loss]") {
    RenderLossConsumer consumer;
    installed(consumer);
    REQUIRE(consumer.apply(loss(1, 7, 3)).accepted());
    REQUIRE(consumer.apply(replay_success(2, 7, 3, 8, 4)).accepted());

    const auto before = CursorView::of(consumer);
    const auto stale = consumer.apply(loss(1, 7, 3, "fixture.event.old"));
    CHECK(stale.outcome == RenderLossOutcome::IgnoredStale);
    CHECK(stale.refusal == RenderLossRefusal::StaleSequence);
    CHECK(CursorView::of(consumer) == before);

    // Positive control: the contiguous next event is accepted.
    RenderLossConsumer control;
    installed(control);
    REQUIRE(control.apply(loss(1, 7, 3)).accepted());
    REQUIRE(control.apply(replay_success(2, 7, 3, 8, 4)).accepted());
    CHECK(control.apply(loss(3, 8, 4, "fixture.event.E2")).accepted());

    const auto gap = consumer.apply(loss(4, 8, 4, "fixture.event.E2"));
    CHECK(gap.outcome == RenderLossOutcome::RejectedFailClosed);
    CHECK(gap.refusal == RenderLossRefusal::SequenceGap);
    CHECK(consumer.last_event_seq() == 2);
    CHECK(consumer.admission() == RenderLossAdmission::Closed);
    // Neither the gapped event nor its successor is ever applied.
    const auto after_gap = consumer.apply(loss(3, 8, 4, "fixture.event.E2"));
    CHECK(after_gap.refusal == RenderLossRefusal::CursorResyncRequired);
    CHECK(consumer.last_event_seq() == 2);
}

TEST_CASE("H4 stale generation and stale surface epoch keep the cursor and the fence",
          "[gpu_audio][render_loss]") {
    RenderLossConsumer consumer;
    installed(consumer);
    REQUIRE(consumer.apply(loss(1, 7, 3)).accepted());
    REQUIRE(consumer.apply(replay_success(2, 7, 3, 8, 4)).accepted());
    REQUIRE(consumer.apply(loss(3, 8, 4, "fixture.event.E2")).accepted());
    const auto before = CursorView::of(consumer);

    CHECK(consumer.apply(loss(4, 7, 4, "fixture.event.E3")).refusal ==
          RenderLossRefusal::StaleGeneration);
    CHECK(consumer.apply(loss(4, 8, 3, "fixture.event.E3")).refusal ==
          RenderLossRefusal::StaleSurfaceEpoch);
    // Epochs never reset, even across a generation change.
    CHECK(consumer.apply(loss(4, 9, 2, "fixture.event.E3")).refusal ==
          RenderLossRefusal::StaleSurfaceEpoch);
    CHECK(CursorView::of(consumer) == before);
    CHECK(consumer.admission() == RenderLossAdmission::Fenced);

    // Positive control: equal generation with equal or higher epoch passes H4
    // (P-11) and is then judged by the replay rules.
    RenderLossConsumer control;
    installed(control);
    REQUIRE(control.apply(loss(1, 7, 3)).accepted());
    REQUIRE(control.apply(replay_success(2, 7, 3, 8, 4)).accepted());
    CHECK(control.apply(loss(3, 8, 4, "fixture.event.E2")).accepted());
}

TEST_CASE("H7 identity mismatch is rejected without touching the cursor",
          "[gpu_audio][render_loss]") {
    RenderLossConsumer consumer;
    installed(consumer);
    auto foreign = loss(1, 7, 3);
    foreign.identity = identity("fixture.other-provider", kFakeProviderVersion, kOwnerA);
    seal(foreign);
    CHECK(consumer.apply(foreign).refusal == RenderLossRefusal::UnexpectedProviderIdentity);
    foreign.identity = identity(kFakeProvider, "fixture.fake-backend.v2", kOwnerA);
    seal(foreign);
    CHECK(consumer.apply(foreign).refusal == RenderLossRefusal::UnexpectedProviderIdentity);
    foreign.identity = owner_b();
    seal(foreign);
    CHECK(consumer.apply(foreign).refusal == RenderLossRefusal::UnexpectedOwnerIdentity);
    CHECK(consumer.last_event_seq() == 0);
    CHECK(consumer.admission() == RenderLossAdmission::Open);

    // An identity longer than the fixed capacity can never alias another.
    const auto long_owner = std::string(RenderLossText::kCapacity + 1, 'a');
    CHECK_FALSE(RenderLossText::from(long_owner).valid());
    CHECK_FALSE(RenderLossText::from(long_owner) ==
                RenderLossText::from(long_owner.substr(0, RenderLossText::kCapacity)));

    // Positive control.
    CHECK(consumer.apply(loss(1, 7, 3)).accepted());
}

TEST_CASE("H8 closed cursors refuse events; H9 is the only way to a new owner; H11 owner reuse",
          "[gpu_audio][render_loss]") {
    RenderLossConsumer consumer;
    installed(consumer);
    REQUIRE(consumer.apply(loss(1, 7, 3)).accepted());

    // Only a replay success for the pending loss lifts its fence; a handoff
    // cannot be used to skip it.
    const RenderOwnerHandoff early{kRenderOwnerLossContractVersion, owner_a(), owner_b()};
    CHECK(consumer.handoff(early).refusal == RenderLossRefusal::HandoffDuringPendingReplay);
    CHECK(consumer.identity() == owner_a());
    CHECK(consumer.admission() == RenderLossAdmission::Fenced);
    CHECK(consumer.status().replay_pending);
    REQUIRE(consumer.apply(replay_success(2, 7, 3, 8, 4)).accepted());

    // H11 keys on owner_id: the same owner under another provider version is
    // still the same owner.
    const RenderOwnerHandoff same_owner{
        kRenderOwnerLossContractVersion, owner_a(),
        identity(kFakeProvider, "fixture.fake-backend.v2", kOwnerA)};
    CHECK(consumer.handoff(same_owner).refusal == RenderLossRefusal::HandoffToRetiredOwner);

    RenderOwnerHandoff wrong_from{kRenderOwnerLossContractVersion, owner_b(), owner_b()};
    CHECK(consumer.handoff(wrong_from).refusal == RenderLossRefusal::HandoffFromMismatch);
    RenderOwnerHandoff self{kRenderOwnerLossContractVersion, owner_a(), owner_a()};
    CHECK(consumer.handoff(self).refusal == RenderLossRefusal::HandoffToRetiredOwner);
    RenderOwnerHandoff future{2, owner_a(), owner_b()};
    CHECK(consumer.handoff(future).refusal == RenderLossRefusal::UnknownContractVersion);
    CHECK(consumer.identity() == owner_a());
    CHECK(consumer.last_event_seq() == 2);

    const auto stale_ticket = consumer.admission_ticket();
    RenderOwnerHandoff a_to_b{kRenderOwnerLossContractVersion, owner_a(), owner_b()};
    REQUIRE(consumer.handoff(a_to_b).accepted());
    CHECK(consumer.identity() == owner_b());
    CHECK(consumer.last_event_seq() == 0);
    CHECK(consumer.last_owner_generation() == 0);
    CHECK(consumer.closure() == RenderLossCursorClosure::Open);
    CHECK(consumer.admission() == RenderLossAdmission::Open);
    CHECK(consumer.admission_ticket() != stale_ticket);

    auto from_a = loss(3, 8, 4, "fixture.event.E2");
    CHECK(consumer.apply(from_a).refusal == RenderLossRefusal::OwnerReuseWithoutExplicitHandoff);
    from_a.identity = identity("fixture.other-provider", "fixture.other-provider.v1", kOwnerA);
    seal(from_a);
    CHECK(consumer.apply(from_a).refusal == RenderLossRefusal::OwnerReuseWithoutExplicitHandoff);
    CHECK(consumer.classify_completion(owner_a(), 7).refusal ==
          RenderLossRefusal::CompletionFromRetiredOwner);
    RenderOwnerHandoff back_to_a{kRenderOwnerLossContractVersion, owner_b(), owner_a()};
    CHECK(consumer.handoff(back_to_a).refusal == RenderLossRefusal::HandoffToRetiredOwner);

    auto from_b = loss(1, 2, 1, "fixture.event.B1");
    from_b.identity = owner_b();
    seal(from_b);
    CHECK(consumer.apply(from_b).accepted());

    SECTION("closed by the consumer") {
        consumer.close();
        auto next = loss(2, 2, 1, "fixture.event.B2");
        next.identity = owner_b();
        seal(next);
        CHECK(consumer.apply(next).refusal == RenderLossRefusal::OwnerReuseWithoutExplicitHandoff);
    }
    SECTION("a terminal while a loss is open fails closed (R-08)") {
        auto terminal = loss(2, 2, 1, "fixture.event.B2");
        terminal.identity = owner_b();
        make_terminal(terminal, RenderOwnerTerminalKind::ProviderRefused);
        auto accepted = consumer.apply(terminal);
        // A terminal while the B1 loss is open is an interleaved loss (R-08).
        CHECK(accepted.refusal == RenderLossRefusal::InterleavedLossDuringReplay);
        auto next = loss(3, 2, 1, "fixture.event.B3");
        next.identity = owner_b();
        seal(next);
        CHECK(consumer.apply(next).refusal == RenderLossRefusal::CursorResyncRequired);
        // A closed cursor has no live replay, so resync goes through H9.
        CHECK_FALSE(consumer.status().replay_pending);
        const auto owner_c =
            identity(kFakeProvider, kFakeProviderVersion, "fixture.render-owner-C");
        CHECK(consumer.handoff({kRenderOwnerLossContractVersion, owner_b(), owner_c}).accepted());
    }
}

TEST_CASE("a handoff after close() opens the new owner and refuses the old replay",
          "[gpu_audio][render_loss]") {
    RenderLossConsumer consumer;
    installed(consumer);
    REQUIRE(consumer.apply(loss(1, 7, 3)).accepted());
    REQUIRE(consumer.status().replay_pending);
    // Positive control for the guard: while A's loss is live, H9 is refused.
    REQUIRE(consumer.handoff({kRenderOwnerLossContractVersion, owner_a(), owner_b()}).refusal ==
            RenderLossRefusal::HandoffDuringPendingReplay);

    consumer.close();
    CHECK(consumer.admission() == RenderLossAdmission::Closed);
    CHECK_FALSE(consumer.status().replay_pending);
    REQUIRE(consumer.handoff({kRenderOwnerLossContractVersion, owner_a(), owner_b()}).accepted());

    // The new owner on its fresh device has no loss to replay.
    CHECK(consumer.identity() == owner_b());
    CHECK(consumer.admission() == RenderLossAdmission::Open);
    const auto ticket = consumer.admission_ticket();
    CHECK(consumer.admission_ticket_current(ticket));

    // A's replay result for the abandoned loss arrives late.
    const auto late = consumer.apply(replay_success(2, 7, 3, 8, 4));
    CHECK(late.outcome == RenderLossOutcome::Rejected);
    CHECK(late.refusal == RenderLossRefusal::OwnerReuseWithoutExplicitHandoff);
    CHECK(consumer.classify_completion(owner_a(), 8).refusal ==
          RenderLossRefusal::CompletionFromRetiredOwner);
    CHECK(consumer.admission() == RenderLossAdmission::Open);
    CHECK(consumer.admission_ticket_current(ticket));
    CHECK(consumer.last_event_seq() == 0);
}

TEST_CASE("resolved event ids are never reused and fail closed before H4",
          "[gpu_audio][render_loss]") {
    SECTION("R-03: a second replay result at the lost generation") {
        RenderLossConsumer consumer;
        installed(consumer);
        REQUIRE(consumer.apply(loss(1, 7, 3)).accepted());
        REQUIRE(consumer.apply(replay_success(2, 7, 3, 8, 4)).accepted());
        const auto verdict = consumer.apply(replay_success(3, 7, 3, 8, 4));
        CHECK(verdict.outcome == RenderLossOutcome::RejectedFailClosed);
        CHECK(verdict.refusal == RenderLossRefusal::DuplicateReplayResult);
        CHECK(consumer.admission() == RenderLossAdmission::Closed);
    }
    SECTION("a loss reusing an older resolved id") {
        RenderLossConsumer consumer;
        installed(consumer);
        REQUIRE(consumer.apply(loss(1, 7, 3, "fixture.event.E1")).accepted());
        REQUIRE(consumer.apply(replay_success(2, 7, 3, 8, 4, "fixture.event.E1")).accepted());
        REQUIRE(consumer.apply(loss(3, 8, 4, "fixture.event.E2")).accepted());
        REQUIRE(consumer.apply(replay_success(4, 8, 4, 9, 5, "fixture.event.E2")).accepted());
        // Positive control: a fresh id at the same position is accepted.
        RenderLossConsumer control;
        installed(control);
        REQUIRE(control.apply(loss(1, 7, 3, "fixture.event.E1")).accepted());
        REQUIRE(control.apply(replay_success(2, 7, 3, 8, 4, "fixture.event.E1")).accepted());
        REQUIRE(control.apply(loss(3, 8, 4, "fixture.event.E2")).accepted());
        REQUIRE(control.apply(replay_success(4, 8, 4, 9, 5, "fixture.event.E2")).accepted());
        CHECK(control.apply(loss(5, 9, 5, "fixture.event.E3")).accepted());

        const auto verdict = consumer.apply(loss(5, 9, 5, "fixture.event.E1"));
        CHECK(verdict.outcome == RenderLossOutcome::RejectedFailClosed);
        CHECK(verdict.refusal == RenderLossRefusal::DuplicateLossEventId);
    }
    SECTION("resolved ids outlive a handoff") {
        RenderLossConsumer consumer;
        installed(consumer);
        auto terminal = loss(1, 7, 3, "fixture.event.E1");
        make_terminal(terminal, RenderOwnerTerminalKind::ProviderRefused);
        REQUIRE(consumer.apply(terminal).accepted());
        REQUIRE(
            consumer.handoff({kRenderOwnerLossContractVersion, owner_a(), owner_b()}).accepted());
        auto reused = loss(1, 2, 1, "fixture.event.E1");
        reused.identity = owner_b();
        seal(reused);
        CHECK(consumer.apply(reused).refusal == RenderLossRefusal::DuplicateLossEventId);
    }
    SECTION("the memory is the documented ring of recent resolved ids") {
        // Resolves `fills` further losses after "first", then reuses "first".
        auto reuse_after = [](std::size_t fills) {
            RenderLossConsumer consumer;
            installed(consumer);
            std::uint64_t seq = 1;
            std::uint64_t gen = 1;
            auto resolve = [&](const std::string& id) {
                REQUIRE(consumer.apply(loss(seq, gen, gen, id)).accepted());
                REQUIRE(consumer.apply(replay_success(seq + 1, gen, gen, gen + 1, gen + 1, id))
                            .accepted());
                seq += 2;
                ++gen;
            };
            resolve("fixture.event.first");
            for (std::size_t i = 0; i < fills; ++i)
                resolve("fixture.event.fill-" + std::to_string(i));
            return consumer.apply(loss(seq, gen, gen, "fixture.event.first"));
        };
        constexpr auto capacity = RenderLossConsumer::kResolvedEventIdCapacity;
        CHECK(reuse_after(capacity - 1).refusal == RenderLossRefusal::DuplicateLossEventId);
        // Beyond the ring an id is forgotten; this is the documented bound.
        CHECK(reuse_after(capacity).accepted());
    }
}

TEST_CASE("retired-owner memory is bounded and refuses rather than forgetting",
          "[gpu_audio][render_loss]") {
    RenderLossConsumer consumer;
    installed(consumer);
    auto current = owner_a();
    for (std::size_t i = 0; i < RenderLossConsumer::kRetiredOwnerCapacity; ++i) {
        const auto next =
            identity(kFakeProvider, kFakeProviderVersion, "owner-" + std::to_string(i));
        REQUIRE(consumer.handoff({kRenderOwnerLossContractVersion, current, next}).accepted());
        current = next;
    }
    const auto overflow = identity(kFakeProvider, kFakeProviderVersion, "owner-overflow");
    CHECK(consumer.handoff({kRenderOwnerLossContractVersion, current, overflow}).refusal ==
          RenderLossRefusal::HandoffCapacityExhausted);
    CHECK(consumer.identity() == current);
    // The first retired owner is still refused precisely.
    CHECK(consumer.apply(loss(1, 7, 3)).refusal ==
          RenderLossRefusal::OwnerReuseWithoutExplicitHandoff);
}

TEST_CASE("H12 completions from a lost or unannounced generation never retire",
          "[gpu_audio][render_loss]") {
    RenderLossConsumer consumer;
    installed(consumer);
    // No generation floor exists before the owner's first event, so nothing
    // is classified as retirable; generation 0 is never valid.
    CHECK(consumer.classify_completion(owner_a(), 7).refusal ==
          RenderLossRefusal::CompletionBeforeFirstEvent);
    CHECK(consumer.classify_completion(owner_a(), 0).refusal ==
          RenderLossRefusal::CompletionWithoutGeneration);

    REQUIRE(consumer.apply(loss(1, 7, 3)).accepted());
    CHECK(consumer.classify_completion(owner_a(), 7).refusal ==
          RenderLossRefusal::CompletionWhileFenced);
    REQUIRE(consumer.apply(replay_success(2, 7, 3, 8, 4)).accepted());
    CHECK(consumer.classify_completion(owner_a(), 7).refusal ==
          RenderLossRefusal::CompletionFromLostGeneration);
    CHECK(consumer.classify_completion(owner_a(), 9).refusal ==
          RenderLossRefusal::CompletionFromUnknownGeneration);
    CHECK(consumer.classify_completion(owner_a(), 0).refusal ==
          RenderLossRefusal::CompletionWithoutGeneration);
    // Positive control: the generation the replay announced retires.
    CHECK(consumer.classify_completion(owner_a(), 8).accepted());
    CHECK(consumer.status().completions_suppressed == 6);
}

TEST_CASE("event shape refusals leave the cursor unchanged", "[gpu_audio][render_loss]") {
    auto expect_refusal = [](RenderOwnerLossEvent event, RenderLossRefusal refusal) {
        RenderLossConsumer consumer;
        installed(consumer);
        const auto before = CursorView::of(consumer);
        const auto verdict = consumer.apply(event);
        CHECK(verdict.outcome == RenderLossOutcome::Rejected);
        CHECK(verdict.refusal == refusal);
        CHECK(CursorView::of(consumer) == before);
    };

    auto quiescent_unsuppressed = loss(1, 7, 3);
    quiescent_unsuppressed.quiescence = RenderOwnerQuiescence::Quiescent;
    seal(quiescent_unsuppressed);
    expect_refusal(quiescent_unsuppressed, RenderLossRefusal::QuiescentRequiresCallbacksSuppressed);

    auto terminal_without_block = loss(1, 7, 3);
    make_terminal(terminal_without_block, RenderOwnerTerminalKind::DeviceLost);
    terminal_without_block.has_terminal = false;
    seal(terminal_without_block);
    expect_refusal(terminal_without_block, RenderLossRefusal::InvalidTerminalBlock);

    // Package section 3: a terminal block must itself say quiescent and
    // callbacks suppressed, and the event must be quiescent.
    auto block_not_quiescent = loss(1, 7, 3);
    make_terminal(block_not_quiescent, RenderOwnerTerminalKind::DeviceLost);
    block_not_quiescent.terminal.quiescent = false;
    seal(block_not_quiescent);
    expect_refusal(block_not_quiescent,
                   RenderLossRefusal::TerminalRequiresQuiescentAndCallbacksSuppressed);

    auto block_callbacks_live = loss(1, 7, 3);
    make_terminal(block_callbacks_live, RenderOwnerTerminalKind::DeviceLost);
    block_callbacks_live.terminal.callbacks_suppressed = false;
    seal(block_callbacks_live);
    expect_refusal(block_callbacks_live,
                   RenderLossRefusal::TerminalRequiresQuiescentAndCallbacksSuppressed);

    auto terminal_draining = loss(1, 7, 3);
    make_terminal(terminal_draining, RenderOwnerTerminalKind::DeviceLost);
    terminal_draining.quiescence = RenderOwnerQuiescence::Draining;
    seal(terminal_draining);
    expect_refusal(terminal_draining,
                   RenderLossRefusal::TerminalRequiresQuiescentAndCallbacksSuppressed);

    auto recoverable_with_block = loss(1, 7, 3);
    recoverable_with_block.has_terminal = true;
    seal(recoverable_with_block);
    expect_refusal(recoverable_with_block, RenderLossRefusal::InvalidTerminalBlock);

    auto teardown_reason_recoverable = loss(1, 7, 3);
    teardown_reason_recoverable.reason = RenderOwnerLossReason::OwnerTeardown;
    seal(teardown_reason_recoverable);
    expect_refusal(teardown_reason_recoverable, RenderLossRefusal::ReasonTerminalKindMismatch);

    auto loss_claiming_replay = loss(1, 7, 3);
    loss_claiming_replay.replay = RenderOwnerReplay::Succeeded;
    loss_claiming_replay.next_owner_generation = 8;
    loss_claiming_replay.next_surface_epoch = 4;
    seal(loss_claiming_replay);
    expect_refusal(loss_claiming_replay, RenderLossRefusal::InvalidLossReplayField);

    auto loss_with_replay_failed_kind = loss(1, 7, 3);
    make_terminal(loss_with_replay_failed_kind, RenderOwnerTerminalKind::ReplayFailed);
    expect_refusal(loss_with_replay_failed_kind, RenderLossRefusal::InvalidTerminalBlock);

    auto bad_enum = loss(1, 7, 3);
    bad_enum.quiescence = static_cast<RenderOwnerQuiescence>(3);
    seal(bad_enum);
    expect_refusal(bad_enum, RenderLossRefusal::InvalidEnumValue);

    for (auto mutate : std::vector<void (*)(RenderOwnerLossEvent&)>{
             [](RenderOwnerLossEvent& e) { e.event_seq = 0; },
             [](RenderOwnerLossEvent& e) { e.owner_generation = 0; },
             [](RenderOwnerLossEvent& e) { e.surface_epoch = 0; },
             [](RenderOwnerLossEvent& e) { e.event_id = {}; },
             [](RenderOwnerLossEvent& e) { e.resource_manifest = {}; },
         }) {
        auto malformed = loss(1, 7, 3);
        mutate(malformed);
        seal(malformed);
        expect_refusal(malformed, RenderLossRefusal::MalformedEvent);
    }
    auto unsealed = loss(1, 7, 3);
    unsealed.payload_digest_sha256 = {};
    expect_refusal(unsealed, RenderLossRefusal::MalformedEvent);

    RenderLossConsumer not_installed;
    CHECK(not_installed.apply(loss(1, 7, 3)).refusal == RenderLossRefusal::NoOwnerInstalled);
    CHECK(not_installed.admission_ticket() == 0);
}

TEST_CASE("terminal events map to the Pulp fallback reason and mark quiescence",
          "[gpu_audio][render_loss]") {
    RenderLossConsumer teardown;
    installed(teardown);
    auto event = loss(1, 7, 3);
    make_terminal(event, RenderOwnerTerminalKind::OwnerTeardown);
    REQUIRE(teardown.apply(event).accepted());
    CHECK(teardown.status().loss_fallback_reason == SharedIoFallbackReason::Teardown);
    CHECK(teardown.status().lost_generation_quiescent);
    CHECK(teardown.status().fence_transitions == 1);
    CHECK(teardown.admission() == RenderLossAdmission::Closed);

    for (const auto reason :
         {RenderOwnerLossReason::DeviceLost, RenderOwnerLossReason::DriverReset,
          RenderOwnerLossReason::OutOfMemory, RenderOwnerLossReason::ProviderRefused,
          RenderOwnerLossReason::Unknown})
        CHECK(fallback_reason_for(reason) == SharedIoFallbackReason::DeviceLost);
}

TEST_CASE("consumer cycle and realtime admission reads do not allocate",
          "[gpu_audio][render_loss][allocation]") {
    RenderLossConsumer consumer;
    installed(consumer);
    const auto first = loss(1, 7, 3);
    const auto replay = replay_success(2, 7, 3, 8, 4);
    const auto second = loss(3, 8, 4, "fixture.event.E2");
    const auto second_replay = replay_success(4, 8, 4, 9, 5, "fixture.event.E2");
    const auto stale = loss(1, 1, 1, "fixture.event.stale");
    const RenderOwnerHandoff a_to_b{kRenderOwnerLossContractVersion, owner_a(), owner_b()};

    bool ok = true;
    std::uint64_t observed_tickets = 0;
    std::size_t allocations = 1;
    {
        pulp::test::RtAllocationProbe probe;
        // Simulated audio callback: capture a ticket, check it at completion.
        const auto ticket = consumer.admission_ticket();
        ok = consumer.admission_ticket_current(ticket) && ok;
        ok = consumer.apply(first).accepted() && ok;
        ok = !consumer.admission_ticket_current(ticket) && ok;
        ok = consumer.apply(first).outcome == RenderLossOutcome::IgnoredDuplicate && ok;
        ok = !consumer.classify_completion(owner_a(), 7).accepted() && ok;
        ok = consumer.apply(replay).accepted() && ok;
        ok = consumer.apply(stale).outcome == RenderLossOutcome::IgnoredStale && ok;
        observed_tickets = consumer.admission_ticket();
        ok = consumer.classify_completion(owner_a(), 8).accepted() && ok;
        ok = consumer.apply(second).accepted() && ok;
        ok =
            consumer.handoff(a_to_b).refusal == RenderLossRefusal::HandoffDuringPendingReplay && ok;
        ok = consumer.apply(second_replay).accepted() && ok;
        ok = consumer.handoff(a_to_b).accepted() && ok;
        ok = consumer.status().handoffs == 1 && ok;
        allocations = probe.allocation_count();
    }
    CHECK(ok);
    CHECK(observed_tickets != 0);
    CHECK(allocations == 0);
}

TEST_CASE("vector fixtures are Pulp-side transcriptions", "[gpu_audio][render_loss]") {
    const auto v1_text =
        read_fixture("test/fixtures/gpu_audio/render_loss_consumer/v1-consumer-vectors.json");
    const auto package_text =
        read_fixture("test/fixtures/gpu_audio/render_loss_consumer/package-consumer-vectors.json");
    const auto v1 = choc::json::parse(v1_text);
    CHECK(v1["provenance"].toString().find("Transcribed from the Pulp/Vellum render-owner loss "
                                           "handoff package (2026-10-21)") == 0);
    CHECK(v1["cases"].size() == 20);
    const auto package = choc::json::parse(package_text);
    CHECK(package["provenance"].toString().find("Transcribed from") == 0);
    // No planning-internal paths or literal fixture digests are carried over.
    for (const auto* text : {&v1_text, &package_text}) {
        CHECK(text->find(".md") == std::string::npos);
        CHECK(text->find("research/") == std::string::npos);
        CHECK(text->find("_sha256") == std::string::npos);
    }
    // torn_down is a terminal kind, not a quiescence state (package section 3).
    CHECK(v1_text.find("torn_down") == std::string::npos);
    CHECK(static_cast<int>(RenderOwnerQuiescence::Quiescent) == 2);
}

TEST_CASE("v1 vectors replay against the consumer under package rules",
          "[gpu_audio][render_loss][vectors]") {
    const auto doc = choc::json::parse(read_fixture("test/fixtures/gpu_audio/render_loss_consumer/"
                                                    "v1-consumer-vectors.json"));
    const auto fixture = doc["fixture_identity"];
    const auto cases = doc["cases"];
    std::set<std::string> seen;
    for (std::uint32_t i = 0; i < cases.size(); ++i) {
        const auto c = cases[i];
        const auto id = c["id"].toString();
        seen.insert(id);
        DYNAMIC_SECTION(id) {
            V1Run run;
            const auto fixture_owner =
                identity(fixture["provider_id"].toString(), fixture["provider_version"].toString(),
                         fixture["owner_id"].toString());
            if (id == kSoftwareProviderCase) {
                RenderLossConsumer probe;
                REQUIRE(probe.install(fixture_owner).accepted());
                CHECK(probe
                          .handoff({kRenderOwnerLossContractVersion, fixture_owner,
                                    software_provider_a()})
                          .refusal == RenderLossRefusal::HandoffToRetiredOwner);
                REQUIRE(run.consumer.install(software_provider_a()).accepted());
            } else {
                REQUIRE(run.consumer.install(fixture_owner).accepted());
            }
            run_v1_trace(run, c, fixture);
            check_v1_expected(run, c);
            check_package_cursor_for_v1(run, id);
        }
    }
    CHECK(seen.size() == 20);
}

TEST_CASE("package P/N/R/T vectors replay against the consumer",
          "[gpu_audio][render_loss][vectors]") {
    const auto doc = choc::json::parse(
        read_fixture("test/fixtures/gpu_audio/render_loss_consumer/package-consumer-vectors.json"));
    const auto vectors = doc["vectors"];
    std::size_t executed = 0;
    for (std::uint32_t i = 0; i < vectors.size(); ++i) {
        const auto vector = vectors[i];
        const auto id = vector["id"].toString();
        const auto scope = vector["scope"].toString();
        if (scope == "vellum-gated")
            continue;
        DYNAMIC_SECTION(id) {
            REQUIRE((scope == "consumer" || scope == "consumer-half"));
            replay_package_vector(vector);
        }
        ++executed;
    }
    CHECK(executed == 21);
}

// Every vector, or vector half, that only a Vellum producer can execute is
// listed here with its reason, so the skip is visible in the ctest summary.
TEST_CASE("producer-side vectors and halves are SKIPPED-VELLUM-GATED",
          "[gpu_audio][render_loss][vectors]") {
    std::string skipped;
    std::size_t whole = 0;
    std::size_t halves = 0;

    const auto package = choc::json::parse(
        read_fixture("test/fixtures/gpu_audio/render_loss_consumer/package-consumer-vectors.json"));
    const auto vectors = package["vectors"];
    for (std::uint32_t i = 0; i < vectors.size(); ++i) {
        const auto vector = vectors[i];
        const auto scope = vector["scope"].toString();
        if (scope == "consumer")
            continue;
        const auto reason = vector["vellum_gated_reason"].toString();
        REQUIRE_FALSE(reason.empty());
        if (scope == "vellum-gated") {
            REQUIRE_FALSE(vector.hasObjectMember("trace"));
            ++whole;
            skipped += vector["id"].toString() + " (whole vector): " + reason + "\n";
        } else {
            REQUIRE(scope == "consumer-half");
            ++halves;
            skipped += vector["id"].toString() + " (producer half): " + reason + "\n";
        }
    }

    const auto v1 = choc::json::parse(read_fixture("test/fixtures/gpu_audio/render_loss_consumer/"
                                                   "v1-consumer-vectors.json"));
    const auto cases = v1["cases"];
    for (std::uint32_t i = 0; i < cases.size(); ++i) {
        const auto c = cases[i];
        const auto id = "v1:" + c["id"].toString();
        if (c.hasObjectMember("injection_point")) {
            ++halves;
            skipped += id + " (producer half): the loss injection at " +
                       c["injection_point"].toString() + " needs Vellum's fake backend.\n";
        }
        const auto expected = c["expected"];
        for (std::uint32_t k = 0; k < expected.size(); ++k) {
            const std::string key = expected.getObjectMemberAt(k).name;
            if (const auto gated = v1_vellum_gated_keys().find(key);
                gated != v1_vellum_gated_keys().end()) {
                ++halves;
                skipped += id + " expected." + key + ": " + gated->second + "\n";
            } else if (key == "callbacks_suppressed") {
                ++halves;
                skipped += id + " expected.callbacks_suppressed (producer half): that the owner "
                                "itself stops invoking callbacks needs Vellum's fake backend.\n";
            }
        }
    }
    ++halves;
    skipped += "v1:positive.terminal.software-provider-refused (producer half): that a software "
               "provider never reports replay=succeeded is producer behaviour.\n";
    ++halves;
    skipped += "v1:positive.terminal.software-provider-refused (OPEN CONTRACT QUESTION): the "
               "package installs the software provider by H9 under the same owner_id, which "
               "section 3/H11 forbid; the consumer refuses that handoff and runs the vector with "
               "the software provider installed directly.\n";

    CHECK(whole == 2);
    REQUIRE_FALSE(skipped.empty());
    SKIP("SKIPPED-VELLUM-GATED (" << whole << " whole vectors, " << halves << " producer halves)\n"
                                  << skipped);
}

TEST_CASE("consumer header stays off the render slice", "[gpu_audio][render_loss]") {
    const auto text = read_fixture("core/gpu_audio/src/detail/render_loss_consumer.hpp");
    for (const auto* forbidden : {"gpu_surface", "pulp/render/", "webgpu", "dawn/", "<vellum"})
        CHECK(text.find(forbidden) == std::string::npos);
}
