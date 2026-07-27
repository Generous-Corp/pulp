// The gate that keeps a host state restore off a Processor while the audio
// thread is inside process().
//
// Processor::deserialize_plugin_state() is documented as running "with the
// audio thread stopped", but VST3 setState, AU setFullState:, AU v2 class-info,
// and CLAP state.load can all arrive while the plug-in is active. These pin the
// two properties the adapters rely on: a render never blocks, and a restore
// never overlaps a render.

#include <catch2/catch_test_macros.hpp>

#include <pulp/format/state_restore_gate.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using pulp::format::StateRestoreGate;

TEST_CASE("Render lock is granted when no restore is in flight",
          "[format][state-restore-gate]") {
    StateRestoreGate gate;
    auto render = gate.lock_for_render();
    REQUIRE(static_cast<bool>(render));
    REQUIRE(gate.contended_blocks() == 0);
}

TEST_CASE("Renders share the gate with each other",
          "[format][state-restore-gate]") {
    StateRestoreGate gate;
    auto first = gate.lock_for_render();
    auto second = gate.lock_for_render();

    // Both are readers; neither excludes the other.
    REQUIRE(static_cast<bool>(first));
    REQUIRE(static_cast<bool>(second));
    REQUIRE(gate.contended_blocks() == 0);
}

TEST_CASE("A render never blocks behind an in-flight restore",
          "[format][state-restore-gate]") {
    StateRestoreGate gate;
    auto restore = gate.lock_for_restore();

    // The audio thread must fail fast rather than wait for the restore.
    const auto start = std::chrono::steady_clock::now();
    auto render = gate.lock_for_render();
    const auto elapsed = std::chrono::steady_clock::now() - start;

    REQUIRE_FALSE(static_cast<bool>(render));
    REQUIRE(gate.contended_blocks() == 1);
    REQUIRE(elapsed < std::chrono::milliseconds(50));
}

TEST_CASE("A restore waits for an in-flight render to finish",
          "[format][state-restore-gate]") {
    StateRestoreGate gate;
    std::atomic<bool> restore_acquired{false};

    std::thread restorer;
    {
        auto render = gate.lock_for_render();
        REQUIRE(static_cast<bool>(render));

        restorer = std::thread([&] {
            auto restore = gate.lock_for_restore();
            restore_acquired.store(true, std::memory_order_release);
        });

        // The restore must not proceed while the render holds the gate. This is
        // the whole point: acquiring the restore lock is what proves no audio
        // callback is inside process().
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        REQUIRE_FALSE(restore_acquired.load(std::memory_order_acquire));
    }

    restorer.join();
    REQUIRE(restore_acquired.load(std::memory_order_acquire));
}

TEST_CASE("Restores and renders never overlap under contention",
          "[format][state-restore-gate]") {
    StateRestoreGate gate;
    std::atomic<int> inside_restore{0};
    std::atomic<int> overlaps{0};
    std::atomic<int> renders_run{0};
    std::atomic<bool> writer_done{false};

    // One writer repeatedly restoring, several readers repeatedly rendering.
    //
    // The writer must release the gate between restores and stay off it long
    // enough for a reader to get in. A writer that re-acquires immediately
    // starves them: shared_mutex favours a waiting writer, so try_lock_shared
    // keeps failing and every render is refused — which would leave this test
    // asserting nothing at all (see the renders_run guard below).
    std::thread writer([&] {
        for (int i = 0; i < 50; ++i) {
            {
                auto restore = gate.lock_for_restore();
                inside_restore.fetch_add(1, std::memory_order_acq_rel);
                std::this_thread::yield();
                inside_restore.fetch_sub(1, std::memory_order_acq_rel);
            }
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });

    std::vector<std::thread> readers;
    for (int t = 0; t < 3; ++t) {
        readers.emplace_back([&] {
            while (!writer_done.load(std::memory_order_acquire)) {
                auto render = gate.lock_for_render();
                if (!render) {
                    std::this_thread::sleep_for(std::chrono::microseconds(50));
                    continue;
                }
                if (inside_restore.load(std::memory_order_acquire) != 0)
                    overlaps.fetch_add(1, std::memory_order_relaxed);
                renders_run.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::yield();
            }
        });
    }

    writer.join();
    writer_done.store(true, std::memory_order_release);
    for (auto& r : readers) r.join();

    // Assertions after every join, so a failure reports from the main thread.
    REQUIRE(overlaps.load() == 0);
    // Guard against the run proving nothing because every render was refused.
    REQUIRE(renders_run.load() > 0);
}

TEST_CASE("Passthrough copies matching channels and zeroes the rest",
          "[format][state-restore-gate]") {
    float in_l[] = {1.0f, 2.0f};
    const float* in_ptrs[] = {in_l};
    pulp::audio::BufferView<const float> in(in_ptrs, 1, 2);

    float out_l[] = {-1.0f, -1.0f};
    float out_r[] = {-1.0f, -1.0f};
    float* out_ptrs[] = {out_l, out_r};
    pulp::audio::BufferView<float> out(out_ptrs, 2, 2);

    pulp::format::passthrough_block(out, in);

    REQUIRE(out_l[0] == 1.0f);
    REQUIRE(out_l[1] == 2.0f);
    // A wider output bus must not carry whatever the host left in the buffer.
    REQUIRE(out_r[0] == 0.0f);
    REQUIRE(out_r[1] == 0.0f);
}

TEST_CASE("Silence zeroes every output channel",
          "[format][state-restore-gate]") {
    float out_l[] = {3.0f, 4.0f};
    float out_r[] = {5.0f, 6.0f};
    float* out_ptrs[] = {out_l, out_r};
    pulp::audio::BufferView<float> out(out_ptrs, 2, 2);

    pulp::format::silence_block(out);

    REQUIRE(out_l[0] == 0.0f);
    REQUIRE(out_l[1] == 0.0f);
    REQUIRE(out_r[0] == 0.0f);
    REQUIRE(out_r[1] == 0.0f);
}
