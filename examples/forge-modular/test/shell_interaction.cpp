// Does the shell actually DO anything when you click it?
//
// This file exists because the answer was no and nobody noticed. The shell
// rendered correctly in a screenshot and was completely inert: wire() casts
// each control to ToggleButton, the controls had been rebuilt as styled rows,
// every cast returned null, and hook() reported the failure by returning false
// to nobody.
//
// A screenshot is proof of paint. It says nothing about whether a control is
// connected to anything. So this drives the real widgets at real coordinates
// and asserts on what the engine was asked to do.

#include <catch2/catch_test_macros.hpp>

#include "forge_modular/shell.hpp"

#include <pulp/state/store.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <thread>

#include <unistd.h>
#include <string>
#include <vector>

namespace {

using pulp::view::Rect;
using pulp::view::View;

/// Records what the shell asked for instead of spawning a compiler.
struct RecordingEngine final : forge_modular::EngineClient {
    struct Call {
        std::string prompt;
        bool patch_mode = false;
    };
    std::vector<Call> calls;
    bool startable = true;
    int ensure_calls = 0;
    std::string error;

    bool available() const override { return startable; }
    bool ensure_running() override { ++ensure_calls; return startable; }
    void submit(const std::string& prompt, bool patch_mode) override {
        if (!error.empty()) return;      // a failed submit records no call
        calls.push_back({prompt, patch_mode});
    }
    std::string last_error() const override { return error; }
};

View* find_by_id(View& root, const std::string& id) {
    if (root.id() == id) return &root;
    for (std::size_t i = 0; i < root.child_count(); ++i) {
        if (auto* hit = find_by_id(*root.child_at(i), id)) return hit;
    }
    return nullptr;
}

Rect absolute_bounds(const View& view) {
    auto out = view.bounds();
    for (auto* p = view.parent(); p != nullptr; p = p->parent()) {
        out.x += p->bounds().x;
        out.y += p->bounds().y;
    }
    return out;
}

/// The shell with its view built and laid out, plus what a test needs to drive
/// it. The size matches the standalone's default so controls land where they
/// land in the real window.
struct Harness {
    pulp::state::StateStore store;
    RecordingEngine engine;
    forge_modular::Shell shell;
    std::unique_ptr<View> root;

    Harness() {
        shell.set_engine(&engine);
        shell.define_parameters(store);
        root = shell.create_view();
        REQUIRE(root != nullptr);
        root->set_bounds({0.0f, 0.0f, 1280.0f, 800.0f});
        root->layout_children();
    }

    View* control(const std::string& id) const {
        return root ? find_by_id(*root, id) : nullptr;
    }

    void type(const std::string& text) {
        auto* ed = dynamic_cast<pulp::view::TextEditor*>(control("prompt"));
        REQUIRE(ed != nullptr);
        ed->set_text(text);
    }

    /// Click through the root at the control's centre, the way a mouse would.
    /// Calling the handler directly would pass even for a control that is
    /// unreachable, invisible or zero-sized -- the exact class of bug this file
    /// exists to catch -- so the geometry is asserted before the click.
    void click(const std::string& id) {
        auto* v = control(id);
        INFO("control: " << id);
        REQUIRE(v != nullptr);
        const auto b = absolute_bounds(*v);
        REQUIRE(b.width > 0.0f);
        REQUIRE(b.height > 0.0f);
        root->simulate_click({b.x + b.width * 0.5f, b.y + b.height * 0.5f});
    }

    std::string label(const std::string& id) const {
        auto* l = dynamic_cast<pulp::view::Label*>(control(id));
        REQUIRE(l != nullptr);
        return l->text();
    }
};

}  // namespace

TEST_CASE("every control the shell paints is a real, reachable widget",
          "[forge-modular][shell][interaction]") {
    Harness h;

    // These are the ids wire() looks for. If one is not a ToggleButton the
    // cast fails and that control is dead -- silently, which is how it shipped.
    for (const char* id : {"btn-build", "btn-ask", "btn-random", "btn-mention",
                           "tab-module", "tab-patch",
                           "rail-home", "rail-module", "rail-patch",
                           "rail-settings"}) {
        auto* v = h.control(id);
        INFO("control: " << id);
        REQUIRE(v != nullptr);
        REQUIRE(dynamic_cast<pulp::view::ToggleButton*>(v) != nullptr);

        // A control nothing can hit is not a control.
        const auto b = absolute_bounds(*v);
        REQUIRE(b.width > 0.0f);
        REQUIRE(b.height > 0.0f);
    }
}

TEST_CASE("Build submits exactly what was typed, once",
          "[forge-modular][shell][interaction]") {
    Harness h;
    h.type("a 12 hp wavefolder with drive and symmetry");
    h.click("btn-build");

    REQUIRE(h.engine.calls.size() == 1);
    REQUIRE(h.engine.calls[0].prompt == "a 12 hp wavefolder with drive and symmetry");
}

TEST_CASE("Build on an empty composer submits nothing",
          "[forge-modular][shell][interaction]") {
    Harness h;
    h.click("btn-build");
    REQUIRE(h.engine.calls.empty());
}

TEST_CASE("Ask can never rewrite the artifact",
          "[forge-modular][shell][interaction]") {
    // Ask and Build differ in one bit and it is carried, not inferred. An Ask
    // turn able to rewrite a patch would destroy work on a misread intent, so
    // the flag is asserted rather than the label trusted.
    Harness h;
    h.type("why did you pick a wavefolder here");
    h.click("btn-ask");

    REQUIRE(h.engine.calls.size() == 1);
    REQUIRE(h.engine.calls[0].patch_mode == false);
}

TEST_CASE("a submit starts the engine rather than assuming it is up",
          "[forge-modular][shell][interaction]") {
    Harness h;
    h.engine.startable = false;
    h.type("a 12 hp wavefolder");
    h.click("btn-build");

    REQUIRE(h.engine.ensure_calls == 1);
    REQUIRE(h.engine.calls.empty());   // could not start, so nothing submitted
}

TEST_CASE("the mode tabs move the whole screen, not just themselves",
          "[forge-modular][shell][interaction]") {
    // A tab that highlights without changing the heading, the button labels
    // and the artifact chip has told the user a lie about what Build will do.
    Harness h;
    REQUIRE(h.label("hero-title") == "What should the module do?");
    REQUIRE(h.label("btn-build-label") == "Build module");

    h.click("tab-patch");
    REQUIRE(h.label("hero-title") == "What should the patch do?");
    REQUIRE(h.label("btn-build-label") == "Build patch");
    REQUIRE(h.label("btn-random-label") == "Random patch");
    REQUIRE(h.label("chip-artifact-text") == "PATCH \u00b7 A WHOLE RACK");

    h.click("tab-module");
    REQUIRE(h.label("hero-title") == "What should the module do?");
    REQUIRE(h.label("btn-build-label") == "Build module");
}

TEST_CASE("Build carries the selected mode to the engine",
          "[forge-modular][shell][interaction]") {
    Harness h;
    h.click("tab-patch");
    h.type("an ambient generative drone");
    h.click("btn-build");

    REQUIRE(h.engine.calls.size() == 1);
    REQUIRE(h.engine.calls[0].patch_mode == true);
}

TEST_CASE("Build in module mode asks for a MODULE",
          "[forge-modular][shell][interaction]") {
    // The case that was missing. wire() hard-coded patch_mode to true, so Build
    // generated a patch whichever tab was selected -- a module could not be made
    // at all -- and the only mode assertion here clicked the Patch tab first,
    // so it saw the value it wanted and passed. Asserting one side of a boolean
    // is not asserting the boolean.
    Harness h;
    h.type("a 4 hp clock divider with reset");
    h.click("btn-build");

    REQUIRE(h.engine.calls.size() == 1);
    REQUIRE(h.engine.calls[0].patch_mode == false);
}

TEST_CASE("the rail reaches both modes",
          "[forge-modular][shell][interaction]") {
    Harness h;
    h.click("rail-patch");
    REQUIRE(h.label("hero-title") == "What should the patch do?");
    h.click("rail-module");
    REQUIRE(h.label("hero-title") == "What should the module do?");
}

TEST_CASE("Random fills the composer instead of building",
          "[forge-modular][shell][interaction]") {
    // A suggestion that builds itself is a dice roll. It has to be readable
    // and editable before anything is committed to.
    Harness h;
    h.click("btn-random");

    auto* ed = dynamic_cast<pulp::view::TextEditor*>(h.control("prompt"));
    REQUIRE(ed != nullptr);
    REQUIRE_FALSE(ed->text().empty());
    REQUIRE(h.engine.calls.empty());
}



// ── the real engine ──────────────────────────────────────────────────────────
//
// The tests above stop at the EngineClient boundary, which leaves the most
// interesting question unanswered: does the real one actually run anything?
// "The button path reaches patch.py" was claimed for a while on the strength of
// reading the code. These drive the real SubprocessEngine, pointed by
// FORGE_MODULAR_TOOLS at stub scripts that record how they were invoked, so the
// spawn is genuine and only the generator is stood in for.

namespace {

/// A tools directory whose generate.py and patch.py record their arguments.
struct StubTools {
    std::filesystem::path dir;

    StubTools() {
        dir = std::filesystem::temp_directory_path() /
              ("fm-stub-" + std::to_string(::getpid()));
        std::filesystem::create_directories(dir);
        write("generate.py", "module");
        write("patch.py", "patch");
    }
    ~StubTools() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    void write(const char* name, const char* kind) {
        std::ofstream f(dir / name);
        // Record the kind and every argument, so the test can tell a module run
        // from a patch run and check the prompt survived the shell quoting.
        f << "import sys, os\n"
          << "open(os.path.join(os.path.dirname(os.path.abspath(__file__)),"
          << " 'invoked.txt'), 'w').write('" << kind << "\\n' + '\\n'.join(sys.argv[1:]))\n";
    }

    /// Wait for the detached child. submit() returns immediately by design --
    /// a generation outlives the click -- so the test has to wait rather than
    /// assume, and must fail rather than hang if nothing ever appears.
    std::string invocation(int timeout_ms = 5000) const {
        const auto marker = dir / "invoked.txt";
        for (int waited = 0; waited < timeout_ms; waited += 50) {
            if (std::filesystem::exists(marker)) {
                std::ifstream f(marker);
                std::string out((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
                if (!out.empty()) return out;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return {};
    }
};

}  // namespace

TEST_CASE("the real engine runs generate.py for a module",
          "[forge-modular][engine]") {
    StubTools tools;
    ::setenv("FORGE_MODULAR_TOOLS", tools.dir.c_str(), 1);
    auto engine = forge_modular::make_engine();
    REQUIRE(engine != nullptr);
    REQUIRE(engine->available());          // both scripts are present

    engine->submit("a 12 hp wavefolder with drive and symmetry", false);

    const auto seen = tools.invocation();
    REQUIRE_FALSE(seen.empty());
    CHECK(seen.find("module") == 0);
    // The prompt has to survive shell quoting intact -- spaces and all.
    CHECK(seen.find("a 12 hp wavefolder with drive and symmetry") != std::string::npos);
    ::unsetenv("FORGE_MODULAR_TOOLS");
}

TEST_CASE("the real engine runs patch.py build for a patch",
          "[forge-modular][engine]") {
    StubTools tools;
    ::setenv("FORGE_MODULAR_TOOLS", tools.dir.c_str(), 1);
    auto engine = forge_modular::make_engine();
    REQUIRE(engine != nullptr);

    engine->submit("an ambient generative drone", true);

    const auto seen = tools.invocation();
    REQUIRE_FALSE(seen.empty());
    CHECK(seen.find("patch") == 0);
    // patch.py is subcommand-driven; without "build" it would inventory rather
    // than generate, and quietly succeed at doing nothing.
    CHECK(seen.find("build") != std::string::npos);
    CHECK(seen.find("an ambient generative drone") != std::string::npos);
    ::unsetenv("FORGE_MODULAR_TOOLS");
}

TEST_CASE("the real engine reports a tools directory that is not there",
          "[forge-modular][engine]") {
    // Not-installed has to be distinguishable from not-yet-running, or the UI
    // shows a button that silently cannot work.
    ::setenv("FORGE_MODULAR_TOOLS", "/nonexistent/forge-modular-tools", 1);
    auto engine = forge_modular::make_engine();
    REQUIRE(engine != nullptr);
    CHECK_FALSE(engine->available());
    ::unsetenv("FORGE_MODULAR_TOOLS");
}

TEST_CASE("driven: a click generates a real module",
          "[.e2e]") {
    // The two halves proven separately -- a click reaching the engine, and the
    // engine's generator producing a module -- joined into one run. Hidden
    // behind [.e2e] because it spawns the real generator: minutes of wall
    // clock and a paid API call, which has no business in ctest.
    //
    //   pulp-test-forge-modular-shell "[.e2e]"
    //
    // Counts module manifests before and after rather than looking for a
    // specific slug: the generator names what it makes, and demanding a name
    // would make the test a lie about what was asked for.
    const std::filesystem::path modules{
        std::filesystem::path(FORGE_MODULAR_TOOLS_DIR).parent_path().parent_path()
        / "examples" / "forge-modular" / "modules"};
    REQUIRE(std::filesystem::exists(modules));

    const auto count = [&] {
        std::size_t n = 0;
        for (const auto& e : std::filesystem::directory_iterator(modules))
            if (e.path().extension() == ".json") ++n;
        return n;
    };
    const std::size_t before = count();

    pulp::state::StateStore store;
    auto engine = forge_modular::make_engine();
    REQUIRE(engine != nullptr);
    REQUIRE(engine->available());

    forge_modular::Shell shell;
    shell.set_engine(engine.get());
    shell.define_parameters(store);
    auto root = shell.create_view();
    REQUIRE(root != nullptr);
    root->set_bounds({0.0f, 0.0f, 1280.0f, 800.0f});
    root->layout_children();

    auto* ed = dynamic_cast<pulp::view::TextEditor*>(find_by_id(*root, "prompt"));
    REQUIRE(ed != nullptr);
    ed->set_text("a 4 HP clock divider with reset and four divisions");

    auto* build = find_by_id(*root, "btn-build");
    REQUIRE(build != nullptr);
    const auto b = absolute_bounds(*build);
    root->simulate_click({b.x + b.width * 0.5f, b.y + b.height * 0.5f});

    // The generator is detached and slow. Wait, but bounded, and fail rather
    // than hang -- an e2e check that can hang forever gets disabled and then
    // nobody runs it at all.
    bool grew = false;
    for (int waited = 0; waited < 600 && !grew; ++waited) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        grew = count() > before;
    }
    INFO("modules before " << before << ", after " << count());
    REQUIRE(grew);
}

TEST_CASE("Random offers a different prompt each time, and never builds",
          "[forge-modular][shell][interaction]") {
    // It was hard-coded to element [0] -- the same suggestion forever, which is
    // what a user reported. Ten draws from a pool of ten must not all match.
    Harness h;
    auto* ed = dynamic_cast<pulp::view::TextEditor*>(h.control("prompt"));
    REQUIRE(ed != nullptr);

    std::set<std::string> seen;
    for (int i = 0; i < 10; ++i) {
        h.click("btn-random");
        REQUIRE_FALSE(ed->text().empty());
        seen.insert(ed->text());
    }
    CHECK(seen.size() > 1);              // not one value forever
    CHECK(h.engine.calls.empty());       // a suggestion is not a build
}

TEST_CASE("a Build that cannot start says so",
          "[forge-modular][shell][interaction]") {
    // The failure that shipped: on a machine without the generator, Build did
    // nothing at all -- no log, no message. Silence is the worst outcome.
    Harness h;
    h.engine.error = "the generator is not installed (/nope/generate.py)";
    h.type("a 4 hp clock divider");
    h.click("btn-build");

    const auto status = h.label("rack-status");
    INFO("status line: " << status);
    CHECK(status.find("not installed") != std::string::npos);
}
