// SPDX-License-Identifier: MIT
#include "isolated_clap_endpoint.hpp"
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
using namespace pulp;
using namespace pulp::host;
TEST_CASE("Isolated CLAP callback validates adversarial peer publications", "[host][isolation]") {
    using namespace host::isolation;
    for (int defect = 0; defect < 9; ++defect) {
        Mapping mapping{};
        CallbackEndpoint endpoint;
        std::array<float, 64> in{}, left{}, right{};
        in.fill(0.25f);
        REQUIRE(endpoint.render(mapping, 17, left.data(), right.data(), in.data(), in.data()) ==
                IsolationReason::none);
        auto& completion = mapping.completions[0];
        completion.block.generation = 17;
        completion.block.sequence = 0;
        completion.block.audio.fill(0.125f);
        completion.ready.store(1, std::memory_order_release);
        if (defect == 0)
            completion.block.generation = 16;
        if (defect == 1)
            completion.block.sequence = 1;
        if (defect == 2)
            completion.block.frames = 63;
        if (defect == 3)
            completion.block.channels = 1;
        if (defect == 4)
            completion.block.result = static_cast<BlockResult>(99);
        if (defect == 5)
            completion.block.audio[127] = std::numeric_limits<float>::quiet_NaN();
        if (defect == 6)
            mapping.requests[1].ready.store(1, std::memory_order_release);
        if (defect == 7)
            completion.ready.store(0, std::memory_order_release);
        const auto reason =
            endpoint.render(mapping, 17, left.data(), right.data(), in.data(), in.data());
        if (defect == 8) {
            REQUIRE(reason == IsolationReason::none);
            REQUIRE(left[0] == 0.125f);
            REQUIRE(right[63] == 0.125f);
        } else {
            REQUIRE(reason != IsolationReason::none);
            for (float sample : left)
                REQUIRE(sample == 0);
            for (float sample : right)
                REQUIRE(sample == 0);
        }
    }
}

TEST_CASE("Isolated CLAP callback emits the completion it validated, not a later rewrite",
          "[host][isolation]") {
    using namespace host::isolation;
    Mapping mapping{};
    CallbackEndpoint endpoint;
    std::array<float, 64> in{}, left{}, right{};
    in.fill(0.25f);
    REQUIRE(endpoint.render(mapping, 17, left.data(), right.data(), in.data(), in.data()) ==
            IsolationReason::none);
    auto& completion = mapping.completions[0];
    completion.block.generation = 17;
    completion.block.sequence = 0;
    completion.block.audio.fill(0.125f);
    completion.ready.store(1, std::memory_order_release);
    // A peer that still writes the mapping replaces every sample with NaN
    // after the finiteness check has passed.
    int rewrites = 0;
    const auto reason =
        endpoint.render(mapping, 17, left.data(), right.data(), in.data(), in.data(), [&] {
            completion.block.audio.fill(std::numeric_limits<float>::quiet_NaN());
            ++rewrites;
        });
    REQUIRE(rewrites == 1);
    REQUIRE(reason == IsolationReason::none);
    for (float sample : left)
        REQUIRE(sample == 0.125f);
    for (float sample : right)
        REQUIRE(sample == 0.125f);
}

TEST_CASE("Isolated CLAP hello reply must echo every binding field", "[host][isolation]") {
    using namespace pulp::host::isolation;
    ControlRecord sent;
    sent.command = Command::hello;
    sent.generation = 41;
    sent.payload_bytes = kMappingBytes;
    sent.volume = 7;
    sent.file = 9;
    sent.file_generation = 11;
    sent.plugin_sha256.fill('a');
    sent.worker_sha256.fill('b');
    sent.build.fill('c');
    constexpr std::uint64_t pid = 1234;
    ControlRecord good = sent;
    good.result = 1;
    good.process_id = pid;
    CHECK(hello_reply_matches(sent, good, pid));

    // Every field the parent binds is checked on its own.
    const std::array<void (*)(ControlRecord&), 13> defects{
        [](ControlRecord& r) { r.command = Command::destroy; },
        [](ControlRecord& r) { r.result = 0; },
        [](ControlRecord& r) { ++r.generation; },
        [](ControlRecord& r) { ++r.process_id; },
        [](ControlRecord& r) { ++r.payload_bytes; },
        [](ControlRecord& r) { ++r.volume; },
        [](ControlRecord& r) { ++r.file; },
        [](ControlRecord& r) { ++r.file_generation; },
        [](ControlRecord& r) { r.plugin_sha256[3] = 'z'; },
        [](ControlRecord& r) { r.worker_sha256[3] = 'z'; },
        [](ControlRecord& r) { r.build[3] = 'z'; },
        [](ControlRecord& r) { r.plugin_sha256.back() = 'z'; },
        [](ControlRecord& r) { r.build.back() = 'z'; },
    };
    for (std::size_t i = 0; i < defects.size(); ++i) {
        INFO("defect=" << i);
        auto reply = good;
        defects[i](reply);
        CHECK_FALSE(hello_reply_matches(sent, reply, pid));
    }
    // A parent that sent a different mapping size is refused even when echoed.
    auto odd = sent;
    odd.payload_bytes = kMappingBytes / 2;
    auto echoed = good;
    echoed.payload_bytes = odd.payload_bytes;
    CHECK_FALSE(hello_reply_matches(odd, echoed, pid));
}

#if defined(_WIN32) && defined(PULP_ISOLATED_CLAP_WINDOWS_PEER)
#include "isolated_clap_windows_channel.hpp"
#include <optional>
#include <pulp/platform/child_process.hpp>
#include <thread>
#include <vector>

namespace {
using namespace pulp::host::isolation;
using pulp::platform::ChildProcess;
using pulp::platform::ChildProcessInputChannel;
using pulp::platform::ProcessOptions;
using pulp::platform::RetainedCleanupStatus;
using pulp::platform::RetainedProcessCustody;
using pulp::platform::RetainedProcessState;

constexpr std::uint64_t kTestGeneration = 0x5a17c0de12345678ull;

ControlRecord identity_record() {
    ControlRecord record;
    record.plugin_sha256.fill('a');
    record.worker_sha256.fill('b');
    record.build.fill('c');
    record.plugin_sha256.back() = record.worker_sha256.back() = record.build.back() = '\0';
    return record;
}

template <typename Predicate>
bool eventually(Predicate predicate, std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return predicate();
}

struct PeerLaunch {
    windows::SharedSection section;
    RetainedProcessCustody reservation;
    ChildProcess process;
    std::optional<ChildProcessInputChannel> channel;
    int process_id = -1;
    bool session_ran = false;

    bool start(const char* mode, int timeout_ms = 5000) {
        if (!section.create(kTestGeneration))
            return false;
        ProcessOptions options;
        options.capture_stdout = options.capture_stderr = false;
        options.standard_input_timeout_ms = timeout_ms;
        return process.start_retained_with_standard_input_channel(
            reservation, PULP_ISOLATED_CLAP_WINDOWS_PEER, {mode},
            [this](int pid, ChildProcessInputChannel received, auto deadline) {
                session_ran = true;
                process_id = pid;
                if (!windows::parent_hello(received.native_handle(), pid, section,
                                           identity_record(), deadline))
                    return false;
                channel.emplace(std::move(received));
                return true;
            },
            options);
    }
};
} // namespace

TEST_CASE("Windows isolated CLAP handshake shares one section with a retained worker",
          "[host][isolation][windows]") {
    PeerLaunch launch;
    REQUIRE(launch.start("ok"));
    REQUIRE(launch.channel);
    auto& completion = launch.section.mapping()->completions[0];
    REQUIRE(eventually([&] { return completion.ready.load(std::memory_order_acquire) == 1; },
                       std::chrono::seconds(5)));
    CHECK(completion.block.generation == kTestGeneration);
    CHECK(completion.block.audio[0] == 0.5f);

    const auto handle = launch.channel->native_handle();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    ControlRecord destroy;
    destroy.command = Command::destroy;
    destroy.generation = kTestGeneration;
    REQUIRE(windows::send_record(handle, destroy, deadline));
    ControlRecord reply;
    REQUIRE(windows::receive_record(handle, reply, deadline));
    CHECK(reply.command == Command::destroy);
    CHECK(reply.result == 1);
    REQUIRE(eventually(
        [&] {
            return launch.reservation.retained_process_observation().state ==
                   RetainedProcessState::Exited;
        },
        std::chrono::seconds(5)));
    CHECK(launch.reservation.retained_process_observation().exit_code == 0);
    const auto cleaned = launch.reservation.cleanup_retained_process_group(
        std::chrono::steady_clock::now() + std::chrono::seconds(2));
    CHECK(cleaned.status == RetainedCleanupStatus::Complete);
    CHECK(cleaned.child_reaped);
    CHECK(cleaned.descendants_gone);
}

TEST_CASE("Windows isolated CLAP section read sees only what the worker wrote",
          "[host][isolation][windows][negative-control]") {
    // Instrument control for the shared-section reading above: a worker that
    // completes the handshake but never writes must leave the completion empty.
    PeerLaunch launch;
    REQUIRE(launch.start("skip-write"));
    auto& completion = launch.section.mapping()->completions[0];
    CHECK_FALSE(eventually([&] { return completion.ready.load(std::memory_order_acquire) != 0; },
                           std::chrono::milliseconds(300)));
    const auto cleaned = launch.reservation.cleanup_retained_process_group(
        std::chrono::steady_clock::now() + std::chrono::seconds(2));
    CHECK(cleaned.status == RetainedCleanupStatus::Complete);
    CHECK(cleaned.descendants_gone);
}

TEST_CASE("Windows isolated CLAP handshake refuses a defective worker reply",
          "[host][isolation][windows]") {
    for (const char* mode : {"wrong-section", "wrong-generation", "wrong-build", "wrong-pid"}) {
        INFO("mode=" << mode);
        PeerLaunch launch;
        CHECK_FALSE(launch.start(mode));
        CHECK(launch.session_ran);
        CHECK(launch.process_id > 0);
        CHECK_FALSE(launch.channel);
        // A refused handshake leaves no worker behind: cleanup already ran.
        CHECK(launch.reservation.retained_process_observation().state ==
              RetainedProcessState::Reaped);
        const auto receipt = launch.reservation.cleanup_retained_process_group(
            std::chrono::steady_clock::now() + std::chrono::seconds(2));
        CHECK(receipt.status == RetainedCleanupStatus::Complete);
        CHECK(receipt.descendants_gone);
    }
}

TEST_CASE("Windows isolated CLAP transfer cancels a timed-out write before returning",
          "[host][isolation][windows]") {
    RetainedProcessCustody reservation;
    ChildProcess process;
    ProcessOptions options;
    options.capture_stdout = options.capture_stderr = false;
    windows::TransferObservation observation;
    bool sent = true;
    std::chrono::steady_clock::duration elapsed{};
    // ping never reads its standard input, so 1 MiB outgrows the pipe buffer
    // and a write stays pending until the deadline expires.
    REQUIRE(process.start_retained_with_standard_input_channel(
        reservation, "ping.exe", {"-n", "60", "127.0.0.1"},
        [&](int, ChildProcessInputChannel channel, auto) {
            std::vector<char> payload(1 << 20, 'x');
            const auto start = std::chrono::steady_clock::now();
            sent = windows::transfer(channel.native_handle(), payload.data(), payload.size(), true,
                                     start + std::chrono::milliseconds(50), &observation);
            elapsed = std::chrono::steady_clock::now() - start;
            return true;
        },
        options));
    CHECK_FALSE(sent);
    CHECK(observation.timed_out);
    CHECK(observation.settled);
    CHECK(elapsed < std::chrono::seconds(4));
    const auto cleaned = reservation.cleanup_retained_process_group(
        std::chrono::steady_clock::now() + std::chrono::seconds(2));
    CHECK(cleaned.status == RetainedCleanupStatus::Complete);
}

TEST_CASE("Windows isolated CLAP waits stay finite and positive for any deadline",
          "[host][isolation][windows]") {
    using std::chrono::milliseconds;
    CHECK(windows::wait_milliseconds(milliseconds(-5)) == 1);
    CHECK(windows::wait_milliseconds(milliseconds(0)) == 1);
    CHECK(windows::wait_milliseconds(milliseconds(750)) == 750);
    CHECK(windows::wait_milliseconds(milliseconds(INFINITE - 1)) == INFINITE - 1);
    // Exactly INFINITE, and anything past 32 bits, clamps below INFINITE
    // instead of becoming an unbounded wait or truncating to a short one.
    CHECK(windows::wait_milliseconds(milliseconds(INFINITE)) == INFINITE - 1);
    CHECK(windows::wait_milliseconds(milliseconds(0x1'0000'0005LL)) == INFINITE - 1);
    CHECK(windows::wait_milliseconds(std::chrono::hours(24 * 60)) == INFINITE - 1);
}

TEST_CASE("Windows isolated CLAP handshake is bounded by its deadline",
          "[host][isolation][windows]") {
    PeerLaunch launch;
    const auto start = std::chrono::steady_clock::now();
    CHECK_FALSE(launch.start("silent", 750));
    const auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(elapsed >= std::chrono::milliseconds(700));
    CHECK(elapsed < std::chrono::seconds(4));
    CHECK(launch.session_ran);
    CHECK(launch.reservation.retained_process_observation().state == RetainedProcessState::Reaped);
}
#endif
