#include <pulp/playback/program_compiler.hpp>
#include <pulp/playback/stable_renderer_shell.hpp>

#include "harness/scoped_rt_process_probe.hpp"
#include "timebase_test_helpers.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <memory>
#include <thread>
#include <vector>

using namespace pulp;
using namespace pulp::playback;
using namespace pulp::timeline;
using namespace pulp::timebase;

namespace {

template <typename T, typename E>
T take(runtime::Result<T, E> result) {
    if (!result) std::abort();
    return std::move(result).value();
}

Clip make_clip(std::uint64_t id, std::int64_t start) {
    return take(Clip::create({id}, {start}, {1}, EmptyContent{}));
}

std::shared_ptr<const Project> make_project(std::size_t clip_count = 2,
                                            std::uint64_t project_id = 1) {
    std::vector<Clip> clips;
    clips.reserve(clip_count);
    for (std::size_t i = 0; i < clip_count; ++i)
        clips.push_back(make_clip(100 + i, static_cast<std::int64_t>(i * 2)));
    auto first = take(Track::create({10}, "one", std::move(clips)));
    auto second = take(Track::create({20}, "two", {make_clip(90, 0)}));
    auto sequence = take(Sequence::create({2}, "root", std::nullopt,
                                          std::vector<Track>{first, second}));
    ProjectInput input;
    input.id = {project_id};
    input.name = "test";
    input.next_item_id = 100 + clip_count + 1;
    input.root_sequence_id = {2};
    input.sequences.push_back(std::move(sequence));
    return std::make_shared<const Project>(take(Project::create(std::move(input))));
}

std::shared_ptr<const Project> make_many_track_project(std::size_t track_count) {
    std::vector<Track> tracks;
    tracks.reserve(track_count);
    for (std::size_t i = 0; i < track_count; ++i)
        tracks.push_back(take(Track::create({10 + i}, "track", {})));
    auto sequence = take(Sequence::create({2}, "root", std::nullopt, std::move(tracks)));
    ProjectInput input;
    input.id = {1};
    input.name = "many";
    input.next_item_id = 10 + track_count + 1;
    input.root_sequence_id = {2};
    input.sequences.push_back(std::move(sequence));
    return std::make_shared<const Project>(take(Project::create(std::move(input))));
}

std::shared_ptr<const CompiledTempoMap> tempo_map() {
    const std::array points{TempoPoint{{0}, 120.0}};
    return shared_compiled_tempo_map(points, RationalRate{48'000, 1});
}

void drain(DeferredCompileExecutor& executor, PlaybackProgramCompiler& compiler) {
    for (int i = 0; i < 1000 && compiler.status().busy; ++i)
        executor.run_for(std::chrono::milliseconds(2), 64);
    REQUIRE_FALSE(compiler.status().busy);
}

ProgramCompileRequest request(std::shared_ptr<const Project> project,
                              std::shared_ptr<const CompiledTempoMap> map,
                              std::uint64_t revision, DirtyTrackSet dirty) {
    ProgramCompileRequest result;
    result.project = std::move(project);
    result.sequence_id = {2};
    result.tempo_map = std::move(map);
    result.document_revision = revision;
    result.dirty = std::move(dirty);
    return result;
}

class InlineExecutor final : public CompileExecutor {
  public:
    bool submit(std::unique_ptr<CompileTask> task,
                std::chrono::steady_clock::time_point) override {
        if (!task) return false;
        while (task->run_slice({std::chrono::steady_clock::now() + std::chrono::seconds(1),
                                10'000}) == CompileTaskStatus::Pending) {}
        return true;
    }
};

class HoldingExecutor final : public CompileExecutor {
  public:
    bool submit(std::unique_ptr<CompileTask> task,
                std::chrono::steady_clock::time_point) override {
        if (reject || !task) return false;
        tasks.push_back(std::move(task));
        return true;
    }
    void drain() {
        while (!tasks.empty()) {
            auto task = std::move(tasks.front());
            tasks.erase(tasks.begin());
            while (task->run_slice({std::chrono::steady_clock::now() + std::chrono::seconds(1),
                                    10'000}) == CompileTaskStatus::Pending) {}
        }
    }
    void run_one_slice() {
        REQUIRE_FALSE(tasks.empty());
        auto task = std::move(tasks.front());
        tasks.erase(tasks.begin());
        if (task->run_slice({std::chrono::steady_clock::now() + std::chrono::seconds(1), 1}) ==
            CompileTaskStatus::Pending)
            tasks.insert(tasks.begin(), std::move(task));
    }
    bool reject = false;
    std::vector<std::unique_ptr<CompileTask>> tasks;
};

} // namespace

static_assert(std::is_trivially_copyable_v<RendererCarryState>);

TEST_CASE("program generation refuses wraparound") {
    REQUIRE(next_program_generation(41).value() == 42);
    REQUIRE_FALSE(next_program_generation(std::numeric_limits<ProgramGeneration>::max()));
}

TEST_CASE("renderer adoption rejects wrong identities and nonmonotonic generations") {
    REQUIRE(is_monotonic_renderer_adoption({}, {{10}, 7}));
    REQUIRE(is_monotonic_renderer_adoption({{10}, 7}, {{10}, 9}));
    REQUIRE_FALSE(is_monotonic_renderer_adoption({{10}, 7}, {{20}, 8}));
    REQUIRE_FALSE(is_monotonic_renderer_adoption({{10}, 7}, {{10}, 7}));
    REQUIRE_FALSE(is_monotonic_renderer_adoption({{10}, 7}, {{10}, 6}));
    ProviderSelectorProgram malformed;
    malformed.selected = ProviderKind::Launcher;
    REQUIRE_FALSE(malformed.available(malformed.selected));
    REQUIRE_FALSE(malformed.available(static_cast<ProviderKind>(255)));
    malformed.available_mask = 1u << 3u;
    REQUIRE_FALSE(malformed.available(static_cast<ProviderKind>(3)));
}

TEST_CASE("program compiler coalesces snapshots and reuses clean track programs") {
    PlaybackProgramStore store;
    DeferredCompileExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    const auto project = make_project(1'000);
    const auto map = tempo_map();

    REQUIRE(compiler.submit(request(project, map, 1, {.all = true})));
    drain(executor, compiler);
    auto first = store.read();
    REQUIRE(first);
    REQUIRE(first->generation() == 1);
    REQUIRE(first->document_revision() == 1);
    const auto* first_dirty = first->find_track({10});
    const auto* first_clean = first->find_track({20});
    REQUIRE(first_dirty);
    REQUIRE(first_clean);

    REQUIRE(compiler.submit(request(project, map, 2, {false, {{10}}})));
    REQUIRE(compiler.submit(request(project, map, 3, {false, {{10}}})));
    drain(executor, compiler);
    auto next = store.read();
    REQUIRE(next->document_revision() == 3);
    REQUIRE(next->generation() == 2);
    REQUIRE(next->find_track({10}) != first_dirty);
    REQUIRE(next->find_track({20}) == first_clean);
    REQUIRE(compiler.status().coalesced_requests == 1);
}

TEST_CASE("compiler rejects stale malformed and unknown-dirty requests") {
    PlaybackProgramStore store;
    DeferredCompileExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    const auto project = make_project();
    const auto map = tempo_map();
    REQUIRE(compiler.submit(request(project, map, 1, {.all = true})));
    const auto stale = compiler.submit(request(project, map, 1, {.all = true}));
    REQUIRE_FALSE(stale);
    REQUIRE(stale.error().code == CompileErrorCode::StaleRevision);
    const auto unknown = compiler.submit(request(project, map, 2, {false, {{999}}}));
    REQUIRE_FALSE(unknown);
    REQUIRE(unknown.error().code == CompileErrorCode::InvalidRequest);
    drain(executor, compiler);
    REQUIRE(store.read()->document_revision() == 1);
}

TEST_CASE("one block pin gives shells coherent skipped-generation adoption without RT locks") {
    PlaybackProgramStore store;
    DeferredCompileExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    const auto project = make_project();
    const auto map = tempo_map();
    StableRendererShell shell({10});
    PlaybackProgramBlockLatch latch;

    REQUIRE(compiler.submit(request(project, map, 1, {.all = true})));
    drain(executor, compiler);
    {
        auto block = latch.begin_block(store);
        REQUIRE(shell.begin_block(block).adoption == ShellAdoptionResult::Adopted);
        auto state = shell.state_snapshot();
        state.event_cursor = 37;
        REQUIRE(shell.end_block(state));
        auto wrong_key = state;
        wrong_key.key.item_id = {999};
        REQUIRE_FALSE(shell.end_block(wrong_key));
        auto invalid = state;
        invalid.valid = false;
        REQUIRE_FALSE(shell.end_block(invalid));
        StableRendererShell missing({999});
        REQUIRE(missing.begin_block(block).adoption == ShellAdoptionResult::Missing);
    }
    REQUIRE(compiler.submit(request(project, map, 2, {false, {{10}}})));
    drain(executor, compiler);
    REQUIRE(compiler.submit(request(project, map, 3, {false, {{10}}})));
    drain(executor, compiler);

    ShellAdoptionResult adoption = ShellAdoptionResult::Missing;
    ProgramGeneration adopted_generation = 0;
    bool ended = false;
    std::size_t allocations = 1;
    {
        test::ScopedRtProcessProbe probe;
        auto block = latch.begin_block(store);
        const auto view = shell.begin_block(block);
        adoption = view.adoption;
        adopted_generation = view.program ? view.program->generation() : 0;
        auto state = shell.state_snapshot();
        state.event_cursor += 1;
        ended = shell.end_block(state);
        allocations = probe.allocation_count();
    }
    REQUIRE(ended);
    REQUIRE(allocations == 0);
    REQUIRE(adoption == ShellAdoptionResult::Adopted);
    REQUIRE(adopted_generation == 3);
    REQUIRE(shell.state_snapshot().event_cursor == 38);
}

TEST_CASE("deferred compiler handles ten thousand clips and one hundred coalesced edits") {
    PlaybackProgramStore store;
    DeferredCompileExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    const auto project = make_project(10'000);
    const auto map = tempo_map();
    for (std::uint64_t revision = 1; revision <= 100; ++revision)
        REQUIRE(compiler.submit(request(project, map, revision, {.all = true})));
    const auto started = std::chrono::steady_clock::now();
    drain(executor, compiler);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    REQUIRE(store.read()->document_revision() == 100);
#if PULP_TEST_WITH_SANITIZER
    REQUIRE(elapsed < std::chrono::milliseconds(500));
#else
    REQUIRE(elapsed < std::chrono::milliseconds(30));
#endif
}

TEST_CASE("worker executor publishes while newer requests arrive") {
    PlaybackProgramStore store;
    WorkerCompileExecutor executor;
    REQUIRE(executor.supported());
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    const auto project = make_project(1'000);
    const auto map = tempo_map();
    for (std::uint64_t revision = 1; revision <= 20; ++revision)
        REQUIRE(compiler.submit(request(project, map, revision, {.all = true})));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (compiler.status().busy && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    REQUIRE_FALSE(compiler.status().busy);
    const auto program = store.read();
    REQUIRE(program);
    REQUIRE(program->document_revision() == 20);
}

TEST_CASE("inline execution cannot reenter a compiler mutex") {
    PlaybackProgramStore store;
    InlineExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    REQUIRE(compiler.submit(request(make_project(), tempo_map(), 1, {.all = true})));
    REQUIRE_FALSE(compiler.status().busy);
    REQUIRE(store.read()->document_revision() == 1);
}

TEST_CASE("queued compile state survives facade destruction") {
    PlaybackProgramStore store;
    HoldingExecutor executor;
    {
        PlaybackProgramCompiler compiler(store, executor, std::chrono::seconds(1));
        REQUIRE(compiler.submit(request(make_project(), tempo_map(), 1, {.all = true})));
    }
    {
        DeferredCompileExecutor other_executor;
        PlaybackProgramCompiler blocked(store, other_executor, std::chrono::microseconds(0));
        const auto result = blocked.submit(request(make_project(), tempo_map(), 2, {.all = true}));
        REQUIRE_FALSE(result);
        REQUIRE(result.error().code == CompileErrorCode::CompilerAlreadyBound);
    }
    executor.drain();
    REQUIRE(store.read()->document_revision() == 1);
}

TEST_CASE("reschedule rejection clears busy and permits revision retry") {
    PlaybackProgramStore store;
    HoldingExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    const auto project = make_project();
    const auto map = tempo_map();
    REQUIRE(compiler.submit(request(project, map, 1, {.all = true})));
    executor.run_one_slice();
    REQUIRE(compiler.submit(request(project, map, 2, {.all = true})));
    executor.reject = true;
    executor.drain();
    REQUIRE_FALSE(compiler.status().busy);
    REQUIRE(compiler.status().has_error);
    REQUIRE(compiler.status().last_error.code == CompileErrorCode::ExecutorUnavailable);
    executor.reject = false;
    REQUIRE(compiler.submit(request(project, map, 2, {.all = true})));
    executor.drain();
    REQUIRE(store.read()->document_revision() == 2);
}

TEST_CASE("initial executor rejection does not consume the revision") {
    PlaybackProgramStore store;
    HoldingExecutor executor;
    executor.reject = true;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    const auto project = make_project();
    const auto map = tempo_map();
    const auto rejected = compiler.submit(request(project, map, 1, {.all = true}));
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == CompileErrorCode::ExecutorUnavailable);
    REQUIRE_FALSE(compiler.status().busy);
    REQUIRE(compiler.status().latest_submitted_revision == 0);
    executor.reject = false;
    REQUIRE(compiler.submit(request(project, map, 1, {.all = true})));
    executor.drain();
    REQUIRE(store.read()->document_revision() == 1);
}

TEST_CASE("tempo identity changes force every track program to rebuild") {
    PlaybackProgramStore store;
    DeferredCompileExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    const auto project = make_project();
    const auto first_map = tempo_map();
    REQUIRE(compiler.submit(request(project, first_map, 1, {.all = true})));
    drain(executor, compiler);
    auto first = store.read();
    const auto* formerly_clean = first->find_track({20});
    const auto second_map = tempo_map();
    REQUIRE(compiler.submit(request(project, second_map, 2, {false, {{10}}})));
    drain(executor, compiler);
    const auto second = store.read();
    REQUIRE(second->find_track({20}) != formerly_clean);
}

TEST_CASE("a block pin remains coherent across a mid-callback publication") {
    PlaybackProgramStore store;
    DeferredCompileExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    const auto project = make_project();
    const auto map = tempo_map();
    REQUIRE(compiler.submit(request(project, map, 1, {.all = true})));
    drain(executor, compiler);
    PlaybackProgramBlockLatch latch;
    auto pinned = latch.begin_block(store);
    REQUIRE(pinned.program()->document_revision() == 1);
    REQUIRE(compiler.submit(request(project, map, 2, {.all = true})));
    drain(executor, compiler);
    REQUIRE(store.read()->document_revision() == 2);
    REQUIRE(pinned.program()->document_revision() == 1);
}

TEST_CASE("compiled stateless policy resets carry state on adoption") {
    PlaybackProgramStore store;
    DeferredCompileExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    const auto project = make_project();
    const auto map = tempo_map();
    REQUIRE(compiler.submit(request(project, map, 1, {.all = true})));
    drain(executor, compiler);
    PlaybackProgramBlockLatch latch;
    StableRendererShell shell({10});
    {
        auto block = latch.begin_block(store);
        REQUIRE(shell.begin_block(block).adoption == ShellAdoptionResult::Adopted);
        auto state = shell.state_snapshot();
        state.event_cursor = 99;
        REQUIRE(shell.end_block(state));
    }
    auto stateless = request(project, map, 2, {false, {{10}}});
    stateless.track_policies.push_back({{10}, {}, RendererStatePolicy::Stateless});
    REQUIRE(compiler.submit(std::move(stateless)));
    drain(executor, compiler);
    auto block = latch.begin_block(store);
    REQUIRE(shell.begin_block(block).adoption == ShellAdoptionResult::Adopted);
    REQUIRE(shell.state_snapshot().event_cursor == 0);
    REQUIRE(shell.state_snapshot().valid);
    auto state = shell.state_snapshot();
    state.event_cursor = 7;
    REQUIRE(shell.end_block(state));
    REQUIRE(compiler.submit(request(project, map, 3, {false, {{10}}})));
    drain(executor, compiler);
    auto next_block = latch.begin_block(store);
    const auto next = shell.begin_block(next_block);
    REQUIRE(next.adoption == ShellAdoptionResult::Adopted);
    REQUIRE(next.program->state_policy() == RendererStatePolicy::Stateless);
    REQUIRE(shell.state_snapshot().event_cursor == 0);
}

TEST_CASE("compiler rejects malformed provider selections") {
    PlaybackProgramStore store;
    DeferredCompileExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    auto malformed = request(make_project(), tempo_map(), 1, {false, {{10}}});
    TrackCompilePolicy policy;
    policy.track_id = {10};
    policy.provider.selected = static_cast<ProviderKind>(3);
    policy.provider.available_mask = 1u << 3u;
    malformed.track_policies.push_back(policy);
    const auto result = compiler.submit(std::move(malformed));
    REQUIRE_FALSE(result);
    REQUIRE(result.error().code == CompileErrorCode::InvalidRequest);
    REQUIRE(result.error().item == ItemId{10});

    auto invalid_state = request(make_project(), tempo_map(), 1, {false, {{10}}});
    policy.provider = {};
    policy.state_policy = static_cast<RendererStatePolicy>(2);
    invalid_state.track_policies.push_back(policy);
    const auto state_result = compiler.submit(std::move(invalid_state));
    REQUIRE_FALSE(state_result);
    REQUIRE(state_result.error().code == CompileErrorCode::InvalidRequest);

    auto unsupported = request(make_project(), tempo_map(), 1, {false, {{10}}});
    policy.state_policy = RendererStatePolicy::CarryByItemId;
    policy.provider.selected = ProviderKind::Launcher;
    policy.provider.available_mask = 1u << static_cast<unsigned>(ProviderKind::Launcher);
    unsupported.track_policies.push_back(policy);
    const auto unsupported_result = compiler.submit(std::move(unsupported));
    REQUIRE_FALSE(unsupported_result);
    REQUIRE(unsupported_result.error().code == CompileErrorCode::InvalidRequest);
}

TEST_CASE("coalescing preserves sparse policy deltas with latest-track wins") {
    PlaybackProgramStore store;
    DeferredCompileExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    const auto project = make_project();
    const auto map = tempo_map();
    REQUIRE(compiler.submit(request(project, map, 1, {.all = true})));
    drain(executor, compiler);

    auto policy_change = request(project, map, 2, {false, {{10}}});
    policy_change.track_policies.push_back({{10}, {}, RendererStatePolicy::Stateless});
    REQUIRE(compiler.submit(std::move(policy_change)));
    REQUIRE(compiler.submit(request(project, map, 3, {false, {{20}}})));
    drain(executor, compiler);
    const auto program = store.read();
    REQUIRE(program->document_revision() == 3);
    REQUIRE(program->find_track({10})->state_policy() == RendererStatePolicy::Stateless);
    REQUIRE(program->find_track({20})->state_policy() == RendererStatePolicy::CarryByItemId);
}

TEST_CASE("many-track linking and validation advance in charged work units") {
    PlaybackProgramStore store;
    HoldingExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    REQUIRE(compiler.submit(request(make_many_track_project(128), tempo_map(), 1,
                                    {.all = true})));
    std::size_t slices = 0;
    executor.run_one_slice();
    ++slices;
    REQUIRE(compiler.status().active_tracks_completed == 1);
    while (compiler.status().busy) {
        executor.run_one_slice();
        ++slices;
        REQUIRE(slices < 10'000);
    }
    REQUIRE(slices > 1'000);
    const auto program = store.read();
    REQUIRE(program->tracks().size() == 128);
    for (std::size_t i = 1; i < program->tracks().size(); ++i)
        REQUIRE(program->tracks()[i - 1]->id() < program->tracks()[i]->id());
}

TEST_CASE("one store binds exactly one compiler publisher core") {
    PlaybackProgramStore store;
    DeferredCompileExecutor first_executor;
    DeferredCompileExecutor second_executor;
    {
        PlaybackProgramCompiler first(store, first_executor, std::chrono::microseconds(0));
        PlaybackProgramCompiler second(store, second_executor, std::chrono::microseconds(0));
        const auto rejected = second.submit(
            request(make_project(), tempo_map(), 1, {.all = true}));
        REQUIRE_FALSE(rejected);
        REQUIRE(rejected.error().code == CompileErrorCode::CompilerAlreadyBound);
    }
    PlaybackProgramCompiler replacement(store, first_executor, std::chrono::microseconds(0));
    REQUIRE(replacement.submit(request(make_project(), tempo_map(), 1, {.all = true})));
    drain(first_executor, replacement);
    REQUIRE(store.read()->document_revision() == 1);
}

TEST_CASE("replacement compiler inherits the live revision and generation floor") {
    PlaybackProgramStore store;
    DeferredCompileExecutor executor;
    const auto project = make_project();
    const auto map = tempo_map();
    {
        PlaybackProgramCompiler first(store, executor, std::chrono::microseconds(0));
        REQUIRE(first.submit(request(project, map, 10, {.all = true})));
        drain(executor, first);
    }
    PlaybackProgramCompiler replacement(store, executor, std::chrono::microseconds(0));
    const auto old = replacement.submit(request(project, map, 9, {.all = true}));
    REQUIRE_FALSE(old);
    REQUIRE(old.error().code == CompileErrorCode::StaleRevision);
    const auto repeated = replacement.submit(request(project, map, 10, {.all = true}));
    REQUIRE_FALSE(repeated);
    REQUIRE(repeated.error().code == CompileErrorCode::StaleRevision);
    REQUIRE(replacement.submit(request(project, map, 11, {.all = true})));
    drain(executor, replacement);
    REQUIRE(store.read()->document_revision() == 11);
    REQUIRE(store.read()->generation() == 2);
    REQUIRE(replacement.status().latest_published_revision == 11);
    REQUIRE(replacement.status().latest_published_generation == 2);
}

TEST_CASE("coalesced policies never cross project identity") {
    PlaybackProgramStore store;
    DeferredCompileExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    auto first = request(make_project(2, 1), tempo_map(), 1, {.all = true});
    first.track_policies.push_back({{10}, {}, RendererStatePolicy::Stateless});
    REQUIRE(compiler.submit(std::move(first)));
    REQUIRE(compiler.submit(request(make_project(2, 3), tempo_map(), 2, {.all = true})));
    drain(executor, compiler);
    const auto program = store.read();
    REQUIRE(program->project_id() == ItemId{3});
    REQUIRE(program->find_track({10})->state_policy() == RendererStatePolicy::CarryByItemId);
}

TEST_CASE("concurrent readers pin programs while worker publication reclaims") {
    PlaybackProgramStore store;
    WorkerCompileExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    const auto project = make_project(500);
    const auto map = tempo_map();
    std::atomic<bool> stop = false;
    std::thread reader([&] {
        while (!stop.load(std::memory_order_acquire)) {
            auto pin = store.read();
            if (pin) (void)pin->generation();
        }
    });
    for (std::uint64_t revision = 1; revision <= 30; ++revision)
        REQUIRE(compiler.submit(request(project, map, revision, {.all = true})));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (compiler.status().busy && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    stop.store(true, std::memory_order_release);
    reader.join();
    REQUIRE_FALSE(compiler.status().busy);
    REQUIRE(store.read()->document_revision() == 30);
}

TEST_CASE("shell inspectors race safely with audio-thread adoption") {
    PlaybackProgramStore store;
    DeferredCompileExecutor executor;
    PlaybackProgramCompiler compiler(store, executor, std::chrono::microseconds(0));
    const auto project = make_project();
    const auto map = tempo_map();
    REQUIRE(compiler.submit(request(project, map, 1, {.all = true})));
    drain(executor, compiler);
    StableRendererShell shell({10});
    PlaybackProgramBlockLatch latch;
    std::atomic<bool> stop = false;
    std::thread audio([&] {
        while (!stop.load(std::memory_order_acquire)) {
            auto block = latch.begin_block(store);
            const auto view = shell.begin_block(block);
            if (view.program) {
                auto state = shell.state_snapshot();
                if (state.valid) (void)shell.end_block(state);
            }
        }
    });
    std::thread inspector([&] {
        while (!stop.load(std::memory_order_acquire)) {
            (void)shell.active_key();
            (void)shell.state_snapshot();
        }
    });
    for (std::uint64_t revision = 2; revision <= 40; ++revision) {
        REQUIRE(compiler.submit(request(project, map, revision, {false, {{10}}})));
        drain(executor, compiler);
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (shell.active_key().generation != store.read()->generation() &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    stop.store(true, std::memory_order_release);
    audio.join();
    inspector.join();
    REQUIRE(shell.active_key().generation == store.read()->generation());
}
