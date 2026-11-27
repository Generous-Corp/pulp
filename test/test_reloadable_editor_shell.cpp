// The reloadable editor shell: a hot-reloadable shell whose editor comes from the
// active logic library's editor_factory(), built at PULP_FORMAT_LEGACY_VIEW=0
// as in an SDK whose UI comes from Vellum. The logic fixtures
// (fixtures/reload_logic_editor.cpp) report every editor call to the host
// through EditorContext::host_action(), as generation * 100 + event.

#include <pulp/format/reload/reloadable_editor_shell.hpp>

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/format/editor_host.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/state/store.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <string>
#include <thread>
#include <vector>

#if !defined(RELOAD_EDITOR_GEN1) || !defined(RELOAD_EDITOR_GEN2) ||                                \
    !defined(RELOAD_EDITOR_LEGACY_MODE) || !defined(RELOAD_WATCH_DIR)
#error "RELOAD_EDITOR_GEN1, _GEN2, _LEGACY_MODE and RELOAD_WATCH_DIR must be defined"
#endif

static_assert(!PULP_FORMAT_LEGACY_VIEW, "this test covers the UI-free SDK");

using namespace pulp;
using format::reload::ReloadableEditorShellControl;
namespace fs = std::filesystem;

namespace {

enum Event : int { built = 1, attached = 2, detached = 3, destroyed = 4, ticked = 5, resized = 6 };

class RecordingContext final : public format::EditorContext {
  public:
    explicit RecordingContext(state::StateStore& store) : port_(store) {}
    format::ParamPort& params() override {
        return port_;
    }
    format::VisualPort* visuals() override {
        return nullptr;
    }
    bool request_resize(const format::EditorSize&) override {
        return true;
    }
    bool host_action(const format::HostActionRequest& request) override {
        events.push_back(static_cast<int>(request.param));
        if (on_event)
            on_event(static_cast<int>(request.param));
        return true;
    }
    format::EditorHostKind host_kind() const override {
        return format::EditorHostKind::plugin;
    }

    std::vector<int> events;
    std::function<void(int)> on_event;

  private:
    format::StateStoreParamPort port_;
};

void install(const fs::path& src, const fs::path& watched, int tick) {
    fs::copy_file(src, watched, fs::copy_options::overwrite_existing);
    fs::last_write_time(watched, fs::file_time_type{} + std::chrono::seconds(3000 + tick));
}

// Render past the shell's crossfade so the retired logic's fade completes.
void render_past_fade(format::Processor& shell) {
    constexpr int frames = 64;
    audio::Buffer<float> in(2, frames), out(2, frames);
    const float* ip[2] = {in.channel(0).data(), in.channel(1).data()};
    audio::BufferView<const float> iv(ip, 2, frames);
    auto ov = out.view();
    midi::MidiBuffer a, b;
    for (int i = 0; i < 64; ++i)
        shell.process(ov, iv, a, b, format::ProcessContext{});
}

struct Rig {
    fs::path watched;
    std::unique_ptr<format::Processor> shell;
    ReloadableEditorShellControl* control = nullptr;
    state::StateStore store;

    explicit Rig(const char* name, const char* initial) {
        watched = fs::path(RELOAD_WATCH_DIR) / name;
        install(initial, watched, 0);
        shell = format::reload::make_reloadable_editor_shell(watched.string());
        control = format::reload::reloadable_editor_shell_control(*shell);
        REQUIRE(control != nullptr);
        shell->define_parameters(store);
        shell->set_state_store(&store);
        format::PrepareContext ctx;
        ctx.sample_rate = 48000.0;
        ctx.max_buffer_size = 64;
        shell->prepare(ctx);
    }
    ~Rig() {
        shell.reset();
        std::error_code ec;
        fs::remove(watched, ec);
    }
};

int first_index(const std::vector<int>& events, int code) {
    const auto it = std::find(events.begin(), events.end(), code);
    return it == events.end() ? -1 : static_cast<int>(it - events.begin());
}

} // namespace

TEST_CASE("A hot swap rebuilds the shell's editor from the new logic", "[reload][editor-shell]") {
    Rig rig("pulp_editor_shell_swap.dylib", RELOAD_EDITOR_GEN1);
    REQUIRE(rig.control->has_active_dsp());
    auto* factory = rig.shell->editor_factory();
    REQUIRE(factory != nullptr);
    REQUIRE(factory->supports(format::EditorRole::editor));
    CHECK_FALSE(factory->supports(format::EditorRole::settings));

    RecordingContext context(rig.store);
    auto editor = factory->create(*rig.shell, context, format::EditorRole::editor);
    REQUIRE(editor != nullptr);
    int parent_view = 0;
    REQUIRE(editor->attach({.kind = format::NativeParentKind::cocoa_view, .handle = &parent_view}));
    REQUIRE(editor->set_size({640, 400, 2.0}));
    CHECK(context.events == std::vector<int>{101, 102, 106});

    install(RELOAD_EDITOR_GEN2, rig.watched, 1);
    REQUIRE(rig.control->reload_now());
    render_past_fade(*rig.shell);
    context.events.clear();

    editor->idle();
    // The old logic's editor detaches and is destroyed before the new one is
    // built; the new one takes the same size and parent, then ticks.
    CHECK(context.events == std::vector<int>{103, 104, 201, 206, 202, 205});
    CHECK(editor->size().width == 640);
    CHECK(editor->size().height == 400);
}

TEST_CASE("No editor call reaches a retired logic processor", "[reload][editor-shell]") {
    Rig rig("pulp_editor_shell_retire.dylib", RELOAD_EDITOR_GEN1);
    RecordingContext context(rig.store);
    auto editor =
        rig.shell->editor_factory()->create(*rig.shell, context, format::EditorRole::editor);
    REQUIRE(editor != nullptr);
    int parent_view = 0;
    REQUIRE(editor->attach({.kind = format::NativeParentKind::cocoa_view, .handle = &parent_view}));

    install(RELOAD_EDITOR_GEN2, rig.watched, 1);
    REQUIRE(rig.control->reload_now());
    render_past_fade(*rig.shell);

    // The fade is over, but the open editor still holds the old logic, so the
    // reclaim parks it instead of freeing it.
    rig.control->reclaim_retired_logic();
    CHECK(rig.control->held_retired_logic() == 1);

    // The rebuild releases it; the next reclaim frees it.
    editor->idle();
    rig.control->reclaim_retired_logic();
    CHECK(rig.control->held_retired_logic() == 0);

    // From the old editor's destruction on, every call is the new logic's.
    context.events.clear();
    editor->idle();
    REQUIRE(editor->set_size({500, 300, 1.0}));
    editor->detach();
    editor.reset();
    REQUIRE_FALSE(context.events.empty());
    for (int code : context.events)
        CHECK(code / 100 == 2);
    CHECK(first_index(context.events, 204) == static_cast<int>(context.events.size()) - 1);
}

TEST_CASE("A logic library built in the other view mode is refused", "[reload][editor-shell]") {
    // Control: a logic in this shell's mode loads and offers its editor.
    {
        Rig same("pulp_editor_shell_same_mode.dylib", RELOAD_EDITOR_GEN1);
        CHECK(same.control->has_active_dsp());
        CHECK(same.shell->editor_factory()->supports(format::EditorRole::editor));
    }
    // The same logic built with PULP_FORMAT_LEGACY_VIEW=1: its Processor's
    // view slots carry the other names, so the fingerprint refuses it and the
    // shell runs as passthrough, with no editor.
    Rig other("pulp_editor_shell_other_mode.dylib", RELOAD_EDITOR_LEGACY_MODE);
    CHECK_FALSE(other.control->has_active_dsp());
    CHECK_FALSE(other.shell->editor_factory()->supports(format::EditorRole::editor));

    // A hot swap to it from a live logic is refused the same way.
    Rig live("pulp_editor_shell_swap_mode.dylib", RELOAD_EDITOR_GEN1);
    install(RELOAD_EDITOR_LEGACY_MODE, live.watched, 1);
    std::string error;
    CHECK_FALSE(live.control->reload_now(&error));
    CHECK(error.find("fingerprint") != std::string::npos);
    CHECK(live.shell->editor_reload_generation() == 0);
}

TEST_CASE("A hot swap during an editor build neither waits for it nor stalls the audio path",
          "[reload][editor-shell]") {
    Rig rig("pulp_editor_shell_concurrent.dylib", RELOAD_EDITOR_GEN1);
    RecordingContext context(rig.store);
    install(RELOAD_EDITOR_GEN2, rig.watched, 1);

    // The logic's editor reports `built` from inside its factory's create().
    // While that call is still running, a control thread swaps the logic and
    // an audio thread renders.
    std::future<bool> swap;
    bool swap_finished_during_create = false;
    std::uint64_t passthrough_blocks_during_create = 0;
    context.on_event = [&](int code) {
        if (code != 101)
            return;
        swap = std::async(std::launch::async, [&] { return rig.control->reload_now(); });
        swap_finished_during_create =
            swap.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
        const auto before = rig.control->contention_blocks();
        std::thread audio([&] {
            constexpr int frames = 64;
            audio::Buffer<float> in(2, frames), out(2, frames);
            const float* ip[2] = {in.channel(0).data(), in.channel(1).data()};
            audio::BufferView<const float> iv(ip, 2, frames);
            auto ov = out.view();
            midi::MidiBuffer a, b;
            for (int i = 0; i < 32; ++i)
                rig.shell->process(ov, iv, a, b, format::ProcessContext{});
        });
        audio.join();
        passthrough_blocks_during_create = rig.control->contention_blocks() - before;
    };

    auto editor =
        rig.shell->editor_factory()->create(*rig.shell, context, format::EditorRole::editor);
    context.on_event = nullptr;
    REQUIRE(swap.valid());
    CHECK(swap.get());
    REQUIRE(editor != nullptr);
    CHECK(swap_finished_during_create);
    CHECK(passthrough_blocks_during_create == 0);

    // The editor was built from the logic the swap retired, so its next tick
    // replaces it with the new logic's.
    context.events.clear();
    editor->idle();
    CHECK(first_index(context.events, 104) >= 0);
    CHECK(first_index(context.events, 201) > first_index(context.events, 104));
}

TEST_CASE("An invalidated editor does not rebuild on a hot swap", "[reload][editor-shell]") {
    Rig rig("pulp_editor_shell_invalidated.dylib", RELOAD_EDITOR_GEN1);
    RecordingContext context(rig.store);
    auto editor =
        rig.shell->editor_factory()->create(*rig.shell, context, format::EditorRole::editor);
    REQUIRE(editor != nullptr);
    int parent_view = 0;
    REQUIRE(editor->attach({.kind = format::NativeParentKind::cocoa_view, .handle = &parent_view}));

    // The host's window is gone; the editor object outlives it until the host
    // gets round to destroying it.
    editor->invalidate();
    install(RELOAD_EDITOR_GEN2, rig.watched, 1);
    REQUIRE(rig.control->reload_now());
    context.events.clear();
    editor->idle();
    CHECK(context.events.empty());
    int other_parent = 0;
    CHECK_FALSE(
        editor->attach({.kind = format::NativeParentKind::cocoa_view, .handle = &other_parent}));
    CHECK(context.events.empty());

    // Control: an editor that was not invalidated does rebuild on the same swap.
    auto live =
        rig.shell->editor_factory()->create(*rig.shell, context, format::EditorRole::editor);
    REQUIRE(live != nullptr);
    install(RELOAD_EDITOR_GEN1, rig.watched, 2);
    REQUIRE(rig.control->reload_now());
    context.events.clear();
    live->idle();
    CHECK(first_index(context.events, 101) >= 0);
}
