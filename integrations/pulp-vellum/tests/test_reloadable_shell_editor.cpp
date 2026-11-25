// A Vellum scripted editor over a real hot-reloadable shell.
//
// This translation unit sees Pulp's format core and Vellum, never Pulp's view
// layer: it includes the view-free shell, and the scripted editor root is the
// one the Vellum UI package builds. The shell swaps real DSP logic libraries
// through its background watcher, and each successful swap must reload the
// editor's script session exactly once on the editor's idle tick. A rejected
// build must leave the editor alone.
#include "../src/scripted_editor_root.hpp"

#include <catch2/catch_test_macros.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/format/reload/reloadable_shell_core.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/state/store.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <system_error>
#include <thread>

#if !defined(RELOAD_LOGIC_UNITY) || !defined(RELOAD_LOGIC_COMPATIBLE) ||                           \
    !defined(RELOAD_LOGIC_INCOMPATIBLE) || !defined(RELOAD_WATCH_DIR)
#error "RELOAD_LOGIC_{UNITY,COMPATIBLE,INCOMPATIBLE} and RELOAD_WATCH_DIR must be defined"
#endif

namespace fs = std::filesystem;
using pulp::format::reload::ReloadableShellCore;
using pulp::format::reload::ReloadOutcome;

namespace {

// Stands in for Vellum's ScriptedUiSession and counts what the root asks of it.
struct CountingSession {
    static inline int reloads = 0;
    static inline int polls = 0;
    static inline bool reload_result = true;
    CountingSession(::vellum::view::View&, ::vellum::value::ParamModel&,
                    ::vellum::view::ScriptedUiOptions) {}
    void set_host_kind(const char*) {}
    bool load(std::string*) {
        return true;
    }
    const char* script_label() const {
        return "counting session";
    }
    bool reload() {
        ++reloads;
        return reload_result;
    }
    void poll() {
        ++polls;
    }
    void attach_gpu_surface(::vellum::render::GpuSurface*) {}
};

class Context final : public pulp::format::EditorContext {
  public:
    explicit Context(pulp::format::ParamPort& port) : port_(port) {}
    pulp::format::ParamPort& params() override {
        return port_;
    }
    pulp::format::VisualPort* visuals() override {
        return nullptr;
    }
    bool request_resize(const pulp::format::EditorSize&) override {
        return false;
    }
    bool host_action(const pulp::format::HostActionRequest&) override {
        return false;
    }
    pulp::format::EditorHostKind host_kind() const override {
        return pulp::format::EditorHostKind::plugin;
    }

  private:
    pulp::format::ParamPort& port_;
};

class NullHost final : public ::vellum::view::PluginViewHost {
  public:
    ::vellum::view::NativeViewHandle native_handle() override {
        return this;
    }
    void attach_to_parent(::vellum::view::NativeViewHandle) override {}
    bool is_attached() const noexcept override {
        return false;
    }
    void detach() override {}
    void repaint() override {}
    void set_size(uint32_t, uint32_t) override {}
    Size get_size() const override {
        return {};
    }
};

struct FactoryReset {
    ~FactoryReset() {
        ::vellum::view::PluginViewHost::clear_factory();
    }
};

// Render one block of all-ones input through the shell and return out[0][0].
float render_one(pulp::format::Processor& shell) {
    constexpr int frames = 64;
    pulp::audio::Buffer<float> in(2, frames), out(2, frames);
    for (int n = 0; n < frames; ++n) {
        in.channel(0)[n] = 1.0f;
        in.channel(1)[n] = 1.0f;
    }
    const float* inputs[2] = {in.channel(0).data(), in.channel(1).data()};
    pulp::audio::BufferView<const float> input(inputs, 2, frames);
    auto output = out.view();
    pulp::midi::MidiBuffer midi_in, midi_out;
    shell.process(output, input, midi_in, midi_out, pulp::format::ProcessContext{});
    return out.channel(0)[0];
}

// Render past the shell's swap crossfade so the steady-state DSP is measured.
float render_steady(pulp::format::Processor& shell) {
    float last = 0.0f;
    for (int i = 0; i < 64; ++i)
        last = render_one(shell);
    return last;
}

// Copy `src` over `watched` with a strictly increasing mtime, so the watcher
// sees the change whatever the filesystem's timestamp resolution.
void install(const fs::path& src, const fs::path& watched, int tick) {
    fs::copy_file(src, watched, fs::copy_options::overwrite_existing);
    fs::last_write_time(watched, fs::file_time_type{} + std::chrono::seconds(3000 + tick));
}

bool wait_for(const std::function<bool()>& done) {
    for (int i = 0; i < 200; ++i) { // up to ~10 s on a loaded host
        if (done())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return done();
}

} // namespace

TEST_CASE("Vellum scripted editor reloads once per hot swap of a reloadable shell",
          "[pulp-vellum][scripted-wiring][reload]") {
    FactoryReset reset;
    CountingSession::reloads = 0;
    CountingSession::polls = 0;
    CountingSession::reload_result = true;
    ::vellum::view::PluginViewHost::set_factory(
        [](::vellum::view::View&, const ::vellum::view::PluginViewHost::Options&) {
            return std::make_unique<NullHost>();
        });

    const fs::path watched = fs::path(RELOAD_WATCH_DIR) / "pulp_vellum_shell_watched.dylib";
    install(RELOAD_LOGIC_UNITY, watched, 0);

    ReloadableShellCore shell(watched.string());
    pulp::state::StateStore store;
    shell.define_parameters(store);
    REQUIRE(store.all_params().size() == 1); // the initial logic loaded and passed its gates
    shell.set_state_store(&store);
    store.set_value(1, 0.5f);
    pulp::format::PrepareContext prepare;
    prepare.sample_rate = 48000.0;
    prepare.max_buffer_size = 64;
    shell.prepare(prepare);
    REQUIRE(shell.has_active_dsp());
    REQUIRE(render_one(shell) == 0.5f); // unity logic

    pulp::format::StateStoreParamPort port(store);
    Context context(port);
    using Root = pulp::vellum::detail::ScriptedEditorRoot<CountingSession>;
    pulp::vellum::Editor<Root> editor(context, shell);
    editor.idle();
    CHECK(CountingSession::reloads == 0);
    CHECK(CountingSession::polls == 1);

    SECTION("a successful swap reloads the editor once") {
        const auto before = shell.editor_reload_generation();
        install(RELOAD_LOGIC_COMPATIBLE, watched, 1);
        REQUIRE(wait_for([&] { return shell.editor_reload_generation() != before; }));
        REQUIRE(shell.successful_reloads() >= 1);
        REQUIRE(render_steady(shell) == 1.0f); // the 2x logic is live
        editor.idle();
        CHECK(CountingSession::reloads == 1);
        editor.idle();
        editor.idle();
        CHECK(CountingSession::reloads == 1);
        CHECK(CountingSession::polls == 4);

        // A rejected build bumps nothing, so the editor keeps its session.
        const auto attempts = shell.reload_attempts();
        install(RELOAD_LOGIC_INCOMPATIBLE, watched, 2);
        REQUIRE(wait_for([&] { return shell.reload_attempts() > attempts; }));
        REQUIRE(shell.last_status() == ReloadOutcome::Status::RejectedContract);
        REQUIRE(render_steady(shell) == 1.0f);
        editor.idle();
        CHECK(CountingSession::reloads == 1);
    }

    SECTION("a failed editor reload retries until it succeeds, then stops") {
        CountingSession::reload_result = false;
        const auto before = shell.editor_reload_generation();
        install(RELOAD_LOGIC_COMPATIBLE, watched, 1);
        REQUIRE(wait_for([&] { return shell.editor_reload_generation() != before; }));
        editor.idle();
        editor.idle();
        CHECK(CountingSession::reloads == 2);
        CountingSession::reload_result = true;
        editor.idle();
        CHECK(CountingSession::reloads == 3);
        editor.idle();
        CHECK(CountingSession::reloads == 3);
    }

    shell.release();
    std::error_code ec;
    fs::remove(watched, ec);
}
