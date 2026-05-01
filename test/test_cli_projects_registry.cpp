// test_cli_projects_registry.cpp — Unit tests for the projects registry
//
// Issue #499 / #552 Slice 1b. Covers:
//   - round-trip read/write of ~/.pulp/projects.json
//   - add_project semantics (dedupe by canonical path, refresh on
//     second add, name hint preserved)
//   - remove_project (returns false on unknown path)
//   - scan_parent_pulp_projects (ancestor walk for pulp_add_* macros)
//
// The registry lives under a per-test TempDir; we do NOT touch the
// developer's real ~/.pulp/projects.json. Tests set no global state.

#include <catch2/catch_test_macros.hpp>

#include "../tools/cli/projects_registry.hpp"

#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#if defined(_WIN32)
#  include <process.h>
#else
#  include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace pulp::cli::projects_registry;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        auto base = fs::temp_directory_path();
        static std::atomic<int> seq{0};
        int n = seq.fetch_add(1);
        int pid =
#if defined(_WIN32)
            _getpid();
#else
            getpid();
#endif
        path = base / ("pulp-projects-registry-test-" +
                       std::to_string(pid) + "-" +
                       std::to_string(reinterpret_cast<std::uintptr_t>(this)) +
                       "-" + std::to_string(n));
        fs::create_directories(path);
        // macOS's /var → /private/var symlink means the raw tmpdir
        // path won't equality-compare against the results of
        // scan_parent_pulp_projects (which canonicalises internally).
        // Normalise once up front so tests can compare paths directly.
        std::error_code ec;
        auto canon = fs::weakly_canonical(path, ec);
        if (!ec) path = canon;
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void write_file(const fs::path& p, const std::string& body) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p);
    f << body;
}

std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f),
                       std::istreambuf_iterator<char>());
}

struct ScopedEnv {
    std::string name;
    bool had_old = false;
    std::string old_value;

    ScopedEnv(std::string env_name, const std::string& value)
        : name(std::move(env_name)) {
        if (auto* old = std::getenv(name.c_str())) {
            had_old = true;
            old_value = old;
        }
#if defined(_WIN32)
        _putenv_s(name.c_str(), value.c_str());
#else
        setenv(name.c_str(), value.c_str(), 1);
#endif
    }

    ~ScopedEnv() {
#if defined(_WIN32)
        _putenv_s(name.c_str(), had_old ? old_value.c_str() : "");
#else
        if (had_old) {
            setenv(name.c_str(), old_value.c_str(), 1);
        } else {
            unsetenv(name.c_str());
        }
#endif
    }
};

struct ScopedCwd {
    fs::path old;

    explicit ScopedCwd(const fs::path& path) : old(fs::current_path()) {
        fs::current_path(path);
    }

    ~ScopedCwd() {
        std::error_code ec;
        fs::current_path(old, ec);
    }
};

}  // namespace

TEST_CASE("registry_path honors overrides and PULP_HOME",
          "[projects-registry][issue-643]") {
    TempDir tmp;

    REQUIRE(registry_path(tmp.path) == tmp.path / "projects.json");

    auto pulp_home = tmp.path / "pulp-home";
    ScopedEnv env("PULP_HOME", pulp_home.string());
    REQUIRE(registry_path() == pulp_home / "projects.json");
}

TEST_CASE("read_registry returns empty when file is missing",
          "[projects-registry][issue-552]") {
    TempDir tmp;
    auto reg = tmp.path / "projects.json";
    auto list = read_registry(reg);
    REQUIRE(list.empty());
}

TEST_CASE("write_registry then read_registry round-trips entries",
          "[projects-registry][issue-552]") {
    TempDir tmp;
    auto reg = tmp.path / "projects.json";

    // Create the project dirs so canonicalish() resolves them
    // consistently — write/read compare string form via generic_string.
    auto a = tmp.path / "a";
    auto b = tmp.path / "b";
    fs::create_directories(a);
    fs::create_directories(b);

    std::vector<Project> in = {
        {a, "ProjectA", "2026-04-21T00:00:00Z"},
        {b, "ProjectB", "2026-04-21T00:01:00Z"},
    };
    REQUIRE(write_registry(reg, in));

    auto out = read_registry(reg);
    REQUIRE(out.size() == 2);
    REQUIRE(out[0].name == "ProjectA");
    REQUIRE(out[1].name == "ProjectB");
    REQUIRE(out[0].registered_at == "2026-04-21T00:00:00Z");
    REQUIRE(out[0].path.filename() == "a");
    REQUIRE(out[1].path.filename() == "b");
}

TEST_CASE("write_registry escapes JSON string fields",
          "[projects-registry][issue-643]") {
    TempDir tmp;
    auto reg = tmp.path / "projects.json";
    auto project = tmp.path / "escaped";

    std::vector<Project> in = {
        {project, "Quote \" Slash \\ Newline\nTab\tControl\x01",
         "2026-04-21T00:00:00Z"},
    };
    REQUIRE(write_registry(reg, in));

    const auto raw = read_file(reg);
    REQUIRE(raw.find("\\\"") != std::string::npos);
    REQUIRE(raw.find("\\\\") != std::string::npos);
    REQUIRE(raw.find("\\n") != std::string::npos);
    REQUIRE(raw.find("\\t") != std::string::npos);
    REQUIRE(raw.find("\\u0001") != std::string::npos);

    auto out = read_registry(reg);
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].name.find("Quote \" Slash \\ Newline\nTab\tControl") == 0);
    REQUIRE(out[0].name.back() == '?');
}

TEST_CASE("add_project canonicalises and dedupes by path",
          "[projects-registry][issue-552]") {
    TempDir tmp;
    auto reg = tmp.path / "projects.json";
    auto proj = tmp.path / "my-plugin";
    fs::create_directories(proj);

    auto after_first = add_project(reg, proj, "MyPlugin");
    REQUIRE(after_first.size() == 1);
    REQUIRE(after_first[0].name == "MyPlugin");

    // Adding the same path again should refresh in place — no duplicate.
    auto after_second = add_project(reg, proj, "MyPlugin");
    REQUIRE(after_second.size() == 1);
    REQUIRE(after_second[0].name == "MyPlugin");

    // Read back from disk: still one entry.
    auto disk = read_registry(reg);
    REQUIRE(disk.size() == 1);
}

TEST_CASE("add_project falls back to directory basename when no name hint",
          "[projects-registry][issue-552]") {
    TempDir tmp;
    auto reg = tmp.path / "projects.json";
    auto proj = tmp.path / "some-project";
    fs::create_directories(proj);

    add_project(reg, proj, {});
    auto disk = read_registry(reg);
    REQUIRE(disk.size() == 1);
    REQUIRE(disk[0].name == "some-project");
}

TEST_CASE("remove_project drops matching entries and is idempotent",
          "[projects-registry][issue-552]") {
    TempDir tmp;
    auto reg = tmp.path / "projects.json";
    auto a = tmp.path / "a";
    auto b = tmp.path / "b";
    fs::create_directories(a);
    fs::create_directories(b);

    add_project(reg, a, "A");
    add_project(reg, b, "B");
    REQUIRE(read_registry(reg).size() == 2);

    REQUIRE(remove_project(reg, a));
    auto list = read_registry(reg);
    REQUIRE(list.size() == 1);
    REQUIRE(list[0].name == "B");

    // Removing the same path again reports "not found".
    REQUIRE_FALSE(remove_project(reg, a));
}

TEST_CASE("scan_parent_pulp_projects finds ancestor CMakeLists.txt with pulp_add_*",
          "[projects-registry][issue-552]") {
    TempDir tmp;
    auto outer = tmp.path / "outer";
    auto inner = outer / "child";
    fs::create_directories(inner);

    write_file(outer / "CMakeLists.txt",
               "project(Outer)\npulp_add_plugin(target FOO)\n");
    write_file(inner / "CMakeLists.txt",
               "add_subdirectory(lib)\n");

    auto hits = scan_parent_pulp_projects(inner);
    REQUIRE_FALSE(hits.empty());
    // Inner directory has no pulp_add_*; outer directory is the one that
    // matches. We deliberately don't assert exhaustive ordering beyond
    // "outer is in the list."
    bool saw_outer = false;
    for (const auto& p : hits) if (p == outer) saw_outer = true;
    REQUIRE(saw_outer);
}

TEST_CASE("scan_parent_pulp_projects surfaces both nested projects (parent + child)",
          "[projects-registry][issue-552]") {
    // Locked-in design: both parent and child appear in the scan. The
    // caller (cmd_doctor) can dedupe against the active project. This
    // test documents that behaviour explicitly so the answer to "which
    // wins, nested parent or child?" is visible in CI.
    TempDir tmp;
    auto parent = tmp.path / "parent";
    auto child = parent / "nested";
    fs::create_directories(child);

    write_file(parent / "CMakeLists.txt",
               "pulp_add_plugin(outer FOO)\n");
    write_file(child / "CMakeLists.txt",
               "pulp_add_plugin(inner FOO)\n");

    auto hits = scan_parent_pulp_projects(child);
    REQUIRE(hits.size() >= 2);
    // Child is closest to start → first in deepest-first order.
    REQUIRE(hits.front() == child);
    bool saw_parent = false;
    for (const auto& p : hits) if (p == parent) saw_parent = true;
    REQUIRE(saw_parent);
}

TEST_CASE("scan_parent_pulp_projects skips dirs without the pulp_add_* macro",
          "[projects-registry][issue-552]") {
    TempDir tmp;
    auto dir = tmp.path / "not-a-pulp-project";
    fs::create_directories(dir);
    write_file(dir / "CMakeLists.txt", "project(Something)\nadd_library(foo foo.cpp)\n");

    auto hits = scan_parent_pulp_projects(dir);
    // Allow ancestor directories to match if they happen to contain a
    // CMakeLists.txt with pulp_add_* (unlikely in /tmp, but not an
    // invariant of this test). The specific assertion: `dir` itself is
    // NOT returned because its CMakeLists.txt lacks the macro.
    for (const auto& p : hits) {
        REQUIRE(p != dir);
    }
}

TEST_CASE("read_registry skips malformed and mixed-shape JSON entries",
          "[projects-registry][issue-643]") {
    TempDir tmp;

    {
        auto reg = tmp.path / "not-object.json";
        write_file(reg, R"(["not", "an", "object"])");
        REQUIRE(read_registry(reg).empty());
    }

    {
        auto reg = tmp.path / "missing-colon.json";
        write_file(reg, R"({"projects" ["missing colon"]})");
        REQUIRE(read_registry(reg).empty());
    }

    {
        auto reg = tmp.path / "mixed.json";
        write_file(reg, R"({
  "schema": {"version": 2, "tags": ["alpha", "beta"]},
  "enabled": true,
  "projects": [
    "not an object",
    42,
    {
      "path": "/tmp/unicode",
      "name": "Unicode \u2603",
      "registered_at": "2026-04-21T10:00:00Z"
    }
  ]
})");
        auto list = read_registry(reg);
        REQUIRE(list.size() == 1);
        REQUIRE(list[0].path == fs::path("/tmp/unicode"));
        REQUIRE(list[0].name == "Unicode ?");
    }

    {
        auto reg = tmp.path / "terminal-non-object.json";
        write_file(reg, R"({"projects": ["not an object"]})");
        REQUIRE(read_registry(reg).empty());
    }

    {
        auto reg = tmp.path / "bad-separator-after-non-object.json";
        write_file(reg, R"({"projects": ["not an object": true]})");
        REQUIRE(read_registry(reg).empty());
    }
}

TEST_CASE("read_registry tolerates forward-compatible non-string fields",
          "[projects-registry][codex-563]") {
    // Codex 2026-04-21 wave 2 P1 on #563: the schema documents
    // unknown fields as forward-compatible, so a future writer is
    // allowed to emit non-string values (objects, booleans, arrays).
    // The previous parser assumed every value was a string and did
    // not advance past unrecognised values — on a `"meta": {...}`
    // field the parse position stalled, which in turn corrupted
    // subsequent project entries or hung `pulp projects list` /
    // `pulp doctor --versions`.
    //
    // This case writes a hand-crafted projects.json carrying both
    // an object-valued field AND a boolean-valued field INSIDE a
    // project, followed by a second well-formed project. Both
    // projects must round-trip intact.
    TempDir tmp;
    auto reg = tmp.path / "projects.json";

    std::string body = R"({
  "projects": [
    {
      "path": "/fake/forward-compat",
      "name": "ForwardCompat",
      "registered_at": "2026-04-21T10:00:00Z",
      "meta": {"schema_rev": 2, "tags": ["alpha", "beta"]},
      "pinned": true
    },
    {
      "path": "/fake/second",
      "name": "Second",
      "registered_at": "2026-04-21T10:05:00Z"
    }
  ]
}
)";
    {
        std::ofstream f(reg);
        f << body;
    }

    auto list = read_registry(reg);
    REQUIRE(list.size() == 2);
    REQUIRE(list[0].name == "ForwardCompat");
    REQUIRE(list[0].registered_at == "2026-04-21T10:00:00Z");
    REQUIRE(list[1].name == "Second");
    REQUIRE(list[1].registered_at == "2026-04-21T10:05:00Z");
}

TEST_CASE("add_project reports write_registry failure via out_wrote_ok",
          "[projects-registry][codex-563]") {
    // Codex 2026-04-21 wave 2 P2 on #563: add_project previously
    // swallowed write_registry() failures — callers saw the
    // in-memory list as if the write had succeeded, producing
    // silent data loss on unwritable $PULP_HOME. The new
    // out-parameter surface lets callers distinguish "registered
    // and durable" from "registered in-memory only".
    TempDir tmp;

    // Happy path: out_wrote_ok should be true when the file is writable.
    {
        auto reg = tmp.path / "ok.json";
        auto proj = tmp.path / "ok-proj";
        fs::create_directories(proj);
        bool wrote_ok = false;
        add_project(reg, proj, "OK", &wrote_ok);
        REQUIRE(wrote_ok);
        REQUIRE(fs::exists(reg));
    }

    // Failure path: point the registry at a path whose parent
    // directory we can't create (an empty path resolves to "" in
    // write_registry → early false return). out_wrote_ok must be
    // surfaced as false.
    {
        fs::path bogus = {};  // empty — write_registry short-circuits
        auto proj = tmp.path / "fail-proj";
        fs::create_directories(proj);
        bool wrote_ok = true;  // preset to opposite of expected
        add_project(bogus, proj, "Fail", &wrote_ok);
        REQUIRE_FALSE(wrote_ok);
    }
}

TEST_CASE("now_iso8601_utc produces Z-suffixed timestamps",
          "[projects-registry][issue-552]") {
    auto s = now_iso8601_utc();
    REQUIRE(s.size() == 20);              // YYYY-MM-DDTHH:MM:SSZ
    REQUIRE(s.back() == 'Z');
    REQUIRE(s[4] == '-');
    REQUIRE(s[10] == 'T');
    REQUIRE(s[13] == ':');
}

TEST_CASE("scan_parent_pulp_projects uses current directory for empty starts",
          "[projects-registry][issue-643]") {
    TempDir tmp;
    auto project = tmp.path / "current";
    fs::create_directories(project);
    write_file(project / "CMakeLists.txt",
               "project(Current)\n"
               "pulp_add_ios_auv3(CurrentAUv3)\n");

    ScopedCwd cwd(project);
    auto hits = scan_parent_pulp_projects({});
    REQUIRE_FALSE(hits.empty());
    REQUIRE(hits.front() == project);
}
