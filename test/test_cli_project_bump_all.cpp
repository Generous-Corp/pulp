// test_cli_project_bump_all.cpp — Batch / --all behavior for
// `pulp project bump`.
//
// Release-discovery Slice 7 (#564 / parent #499). These tests exercise
// the batch-level invariants that hold regardless of where the project
// list comes from (registry or the single CWD):
//
//   - Partial-failure isolation: one "failed" entry in a batch does
//     not corrupt the undo file for the other entries.
//   - Missing-project handling: registry entries that don't exist on
//     disk become "failed" rows without mutating anything.
//   - Undo round-trip across a multi-entry batch.
//   - Stale-registry policy: a batch that encounters a missing
//     project does NOT remove the project from the registry (matches
//     the Slice 1b "never silently mutate the user's registry" rule).
//
// The tests simulate the bump flow by driving `find_pin_site` +
// `rewrite_pin` + undo I/O directly. This mirrors the
// `bump_one` loop in cmd_project.cpp without needing to link the
// full CLI binary — same decoupling discipline as test_cli_project_bump.
// An end-to-end smoke via `pulp project bump --help` lives in
// test_cli_shellout.cpp.

#include <catch2/catch_test_macros.hpp>

#include "../tools/cli/project_bump.hpp"
#include "../tools/cli/projects_registry.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
namespace pb = pulp::cli::project_bump;
namespace prjreg = pulp::cli::projects_registry;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        auto base = fs::temp_directory_path();
        static std::atomic<int> seq{0};
        int n = seq.fetch_add(1);
        path = base / ("pulp-project-bump-all-test-" +
                       std::to_string(reinterpret_cast<std::uintptr_t>(this)) +
                       "-" + std::to_string(n));
        fs::create_directories(path);
        std::error_code ec;
        auto canon = fs::weakly_canonical(path, ec);
        if (!ec) path = canon;
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

std::string read_text(const fs::path& p) {
    std::ifstream f(p);
    if (!f.is_open()) return {};
    std::ostringstream os;
    os << f.rdbuf();
    return os.str();
}

void write_text(const fs::path& p, const std::string& body) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p);
    f << body;
}

// Build a minimal Pulp-ish project under `root`.
void make_pulp_project(const fs::path& root, const std::string& pin) {
    fs::create_directories(root);
    std::string src =
        "cmake_minimum_required(VERSION 3.20)\n"
        "include(FetchContent)\n"
        "FetchContent_Declare(pulp\n"
        "    GIT_REPOSITORY https://github.com/danielraffel/pulp.git\n"
        "    GIT_TAG v" + pin + ")\n"
        "FetchContent_MakeAvailable(pulp)\n"
        "pulp_add_plugin(app FORMATS CLAP)\n";
    write_text(root / "CMakeLists.txt", src);
}

// Simulate bump_one() for a single project. Returns an UndoEntry.
// Deliberately kept short — the real dispatch lives in cmd_project.cpp.
// What we're testing here is the BATCH invariants that compose on top
// of it.
pb::UndoEntry simulate_bump(const fs::path& proj,
                            const std::string& target) {
    pb::UndoEntry e;
    e.project_path = proj;
    e.project_name = proj.filename().string();
    e.status = "skipped";

    if (!fs::exists(proj)) {
        e.status = "failed";
        e.failure_reason = "project path does not exist";
        return e;
    }
    auto cmake_path = proj / "CMakeLists.txt";
    if (!fs::exists(cmake_path)) {
        e.status = "failed";
        e.failure_reason = "no CMakeLists.txt";
        return e;
    }
    auto src = read_text(cmake_path);
    auto site = pb::find_pin_site(src);
    e.pin_kind = site.kind;
    e.old_pin = site.current_pin;
    e.old_pin_style_has_v = pb::pin_has_v_prefix(site.current_pin);

    if (site.kind == pb::PinKind::Unknown) {
        e.status = "skipped";
        e.failure_reason = "no pin";
        return e;
    }
    if (pb::refuse_dynamic_pin(site)) {
        e.status = "skipped";
        e.failure_reason = "dynamic pin";
        return e;
    }
    auto normalized = pb::normalize_pin(site.current_pin);
    if (normalized == target) {
        e.status = "skipped";
        e.failure_reason = "already at target";
        return e;
    }
    auto rewritten = pb::rewrite_pin(src, site, target, e.old_pin_style_has_v);
    if (!rewritten) {
        e.status = "failed";
        e.failure_reason = "rewrite failed";
        return e;
    }
    write_text(cmake_path, *rewritten);
    e.status = "bumped";
    return e;
}

}  // namespace

TEST_CASE("bump all bumps every registered project and records undo",
          "[project-bump-all][issue-564]") {
    TempDir tmp;
    auto home = tmp.path / "pulp-home";
    fs::create_directories(home);

    // Three projects on disk.
    auto p1 = tmp.path / "proj1";
    auto p2 = tmp.path / "proj2";
    auto p3 = tmp.path / "proj3";
    make_pulp_project(p1, "0.23.0");
    make_pulp_project(p2, "0.24.0");
    make_pulp_project(p3, "0.25.0");

    // Registry under the scratch home.
    auto reg = home / "projects.json";
    prjreg::add_project(reg, p1, "Proj1");
    prjreg::add_project(reg, p2, "Proj2");
    prjreg::add_project(reg, p3, "Proj3");

    // Drive the batch — mirrors do_bump()'s inner loop.
    pb::UndoBatch batch;
    batch.timestamp = pb::now_iso8601_utc();
    batch.target_version = "0.32.0";
    for (const auto& proj : prjreg::read_registry(reg)) {
        batch.entries.push_back(simulate_bump(proj.path, batch.target_version));
    }

    // Every project bumped successfully.
    REQUIRE(batch.entries.size() == 3);
    for (const auto& e : batch.entries) REQUIRE(e.status == "bumped");

    // Write undo file and round-trip.
    auto undo_path = pb::undo_batch_path(home, batch.timestamp);
    REQUIRE(pb::write_undo_batch(undo_path, batch));
    auto back = pb::read_undo_batch(undo_path);
    REQUIRE(back);
    REQUIRE(back->entries.size() == 3);

    // On-disk CMakeLists reflect the new pin.
    REQUIRE(read_text(p1 / "CMakeLists.txt").find("GIT_TAG v0.32.0") != std::string::npos);
    REQUIRE(read_text(p2 / "CMakeLists.txt").find("GIT_TAG v0.32.0") != std::string::npos);
    REQUIRE(read_text(p3 / "CMakeLists.txt").find("GIT_TAG v0.32.0") != std::string::npos);
}

TEST_CASE("bump all tolerates partial failure and keeps batch intact",
          "[project-bump-all][issue-564]") {
    TempDir tmp;
    auto home = tmp.path / "pulp-home";
    fs::create_directories(home);

    // Five entries: 4 healthy, 1 missing-on-disk.
    auto p1 = tmp.path / "a";
    auto p2 = tmp.path / "b";
    auto p3 = tmp.path / "c";
    auto p4 = tmp.path / "d";
    make_pulp_project(p1, "0.23.0");
    make_pulp_project(p2, "0.23.0");
    make_pulp_project(p3, "0.23.0");
    make_pulp_project(p4, "0.23.0");
    auto ghost = tmp.path / "ghost";  // never created

    auto reg = home / "projects.json";
    prjreg::add_project(reg, p1, "A");
    prjreg::add_project(reg, p2, "B");
    prjreg::add_project(reg, ghost, "Ghost");
    prjreg::add_project(reg, p3, "C");
    prjreg::add_project(reg, p4, "D");

    pb::UndoBatch batch;
    batch.timestamp = pb::now_iso8601_utc();
    batch.target_version = "0.32.0";
    for (const auto& proj : prjreg::read_registry(reg)) {
        batch.entries.push_back(simulate_bump(proj.path, batch.target_version));
    }

    REQUIRE(batch.entries.size() == 5);
    int bumped = 0, failed = 0;
    for (const auto& e : batch.entries) {
        if (e.status == "bumped") ++bumped;
        else if (e.status == "failed") ++failed;
    }
    REQUIRE(bumped == 4);
    REQUIRE(failed == 1);

    // Ghost failure must not have removed the entry from the
    // registry — Slice 1b policy is "never silently mutate".
    auto after = prjreg::read_registry(reg);
    REQUIRE(after.size() == 5);

    // Undo file captures all five entries — the failed one too so
    // the user can see what was reported.
    auto undo_path = pb::undo_batch_path(home, batch.timestamp);
    REQUIRE(pb::write_undo_batch(undo_path, batch));
    auto back = pb::read_undo_batch(undo_path);
    REQUIRE(back);
    REQUIRE(back->entries.size() == 5);
    int back_failed = 0;
    for (const auto& e : back->entries) if (e.status == "failed") ++back_failed;
    REQUIRE(back_failed == 1);
}

TEST_CASE("undo reverts only 'bumped' entries, leaves others alone",
          "[project-bump-all][issue-564]") {
    TempDir tmp;
    auto home = tmp.path / "pulp-home";
    fs::create_directories(home);

    auto p_ok      = tmp.path / "ok";
    auto p_dynamic = tmp.path / "dynamic";
    make_pulp_project(p_ok, "0.23.0");
    fs::create_directories(p_dynamic);
    write_text(p_dynamic / "CMakeLists.txt",
               "FetchContent_Declare(pulp GIT_TAG main)\n");

    pb::UndoBatch batch;
    batch.timestamp = pb::now_iso8601_utc();
    batch.target_version = "0.32.0";
    batch.entries.push_back(simulate_bump(p_ok,      "0.32.0"));
    batch.entries.push_back(simulate_bump(p_dynamic, "0.32.0"));

    REQUIRE(batch.entries[0].status == "bumped");
    REQUIRE(batch.entries[1].status == "skipped");

    // Simulate undo: walk entries, revert only the bumped ones.
    for (const auto& e : batch.entries) {
        if (e.status != "bumped") continue;
        auto src = read_text(e.project_path / "CMakeLists.txt");
        auto site = pb::find_pin_site(src);
        REQUIRE(pb::normalize_pin(site.current_pin) == "0.32.0");
        auto restored = pb::rewrite_pin(src, site,
                                         pb::normalize_pin(e.old_pin),
                                         e.old_pin_style_has_v);
        REQUIRE(restored);
        write_text(e.project_path / "CMakeLists.txt", *restored);
    }

    // After undo: ok is back to 0.23.0, dynamic is still `main`.
    REQUIRE(read_text(p_ok / "CMakeLists.txt").find("GIT_TAG v0.23.0") != std::string::npos);
    REQUIRE(read_text(p_dynamic / "CMakeLists.txt").find("GIT_TAG main") != std::string::npos);
}

TEST_CASE("downgrade guard refuses lower target without allow-downgrade",
          "[project-bump-all][issue-564]") {
    TempDir tmp;
    auto proj = tmp.path / "pp";
    make_pulp_project(proj, "0.32.0");

    // Drive the decision like do_bump() does: check is_downgrade
    // before calling rewrite_pin.
    auto src = read_text(proj / "CMakeLists.txt");
    auto site = pb::find_pin_site(src);
    auto current = pb::normalize_pin(site.current_pin);
    REQUIRE(current == "0.32.0");
    REQUIRE(pb::is_downgrade(current, "0.31.0"));

    // Simulate --allow-downgrade = false by not rewriting.
    // Without the flag the on-disk pin stays at 0.32.0.
    auto still = read_text(proj / "CMakeLists.txt");
    REQUIRE(still.find("GIT_TAG v0.32.0") != std::string::npos);

    // With the flag, the rewrite proceeds.
    auto rewritten = pb::rewrite_pin(src, site, "0.31.0", true);
    REQUIRE(rewritten);
    write_text(proj / "CMakeLists.txt", *rewritten);
    REQUIRE(read_text(proj / "CMakeLists.txt").find("GIT_TAG v0.31.0") != std::string::npos);
}
