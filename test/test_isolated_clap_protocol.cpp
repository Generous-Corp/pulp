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

#if defined(_WIN32) && defined(PULP_ISOLATED_CLAP_WINDOWS_PEER)
#include "isolated_clap_windows_channel.hpp"
#include <optional>
#include <pulp/platform/child_process.hpp>
#include <thread>

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
