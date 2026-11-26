// A Vellum scripted editor taking part in a live-swap transaction.
//
// The UI stage moves a real Vellum ScriptedUiSession from one script file to
// another through apply_live_swap(), and a later stage that fails must roll
// the session back to the script it ran before the transaction.
#include <catch2/catch_test_macros.hpp>
#include <pulp/format/reload/live_swap_transaction.hpp>
#include <pulp/vellum/scripted_ui_swap_unit.hpp>

#include <vellum/state/store.hpp>
#include <vellum/view/scripted_ui.hpp>
#include <vellum/view/widgets.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
using pulp::format::reload::apply_live_swap;
using pulp::format::reload::SwapStage;
using pulp::format::reload::SwapUnit;
using pulp::vellum::ScriptedUiSwapUnit;

namespace {

struct ScriptDir {
    fs::path root;
    ScriptDir() {
        static std::atomic<int> counter{0};
        root = fs::temp_directory_path() /
               ("pulp-vellum-ux-swap-" + std::to_string(counter.fetch_add(1)) + "-" +
                std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::create_directories(root);
    }
    ~ScriptDir() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    fs::path write(const std::string& name, const std::string& source) const {
        const auto path = root / name;
        std::ofstream(path, std::ios::binary) << source;
        return path;
    }
};

std::string label_text(::vellum::view::ScriptedUiSession& session) {
    REQUIRE(session.bridge() != nullptr);
    auto* label = dynamic_cast<::vellum::view::Label*>(session.bridge()->widget("v"));
    REQUIRE(label != nullptr);
    return std::string(label->text());
}

} // namespace

TEST_CASE("Vellum ScriptedUiSwapUnit applies and rolls back a UI swap",
          "[pulp-vellum][reload][swap-unit]") {
    ScriptDir dir;
    const auto ui_a = dir.write("a.js", "createLabel('v', 'A', '');");
    const auto ui_b = dir.write("b.js", "createLabel('v', 'B', '');");

    ::vellum::view::View root;
    root.set_bounds({0, 0, 320, 240});
    ::vellum::state::StateStore store;
    ::vellum::view::ScriptedUiSession session(root, store, {.script_path = ui_a});
    std::string error;
    REQUIRE(session.load(&error));
    REQUIRE(session.script_path() == ui_a);
    REQUIRE(label_text(session) == "A");

    SECTION("a UI-only transaction commits the swap") {
        ScriptedUiSwapUnit ux(session, ui_b);
        std::vector<SwapUnit*> units{&ux};
        REQUIRE(apply_live_swap(units).ok);
        CHECK(session.script_path() == ui_b);
        CHECK(label_text(session) == "B");
    }

    SECTION("a later stage failing rolls the UI back to the previous script") {
        ScriptedUiSwapUnit ux(session, ui_b);
        std::vector<SwapStage> stages{ux.to_stage(), SwapStage{"dsp", [] { return false; }, [] {}}};
        const auto result = apply_live_swap(stages);
        REQUIRE_FALSE(result.ok);
        CHECK(result.failed_stage == "dsp");
        CHECK(session.script_path() == ui_a);
        CHECK(label_text(session) == "A");
    }

    SECTION("a script that fails to evaluate fails the stage and keeps the UI") {
        const auto broken = dir.write("broken.js", "throw new Error('broken');");
        ScriptedUiSwapUnit ux(session, broken);
        std::vector<SwapUnit*> units{&ux};
        const auto result = apply_live_swap(units);
        REQUIRE_FALSE(result.ok);
        CHECK(result.failed_stage == "ux");
        CHECK(label_text(session) == "A");
    }
}
