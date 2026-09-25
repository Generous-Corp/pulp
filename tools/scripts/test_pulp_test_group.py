#!/usr/bin/env python3
"""End-to-end test for grouped Catch2 suites (`pulp_add_test_suite(... GROUP)`).

A fixture project declares one group with three member suites, one of them
registered twice with different tag specs, prefixes and properties. The fake
Catch2 binary answers `--list-tests` only when `--filenames-as-tags` (`-#`)
is on the command line and only for the `[#<stem>]` tags it recognizes, so a
member whose discovery is not scoped to its own source lists nothing. The
checks then read what CTest registered:

  * one executable holds every member (a second TU contributes a symbol the
    fake main links against, so the link proves it),
  * each member's cases are registered under that member's own labels,
    timeout, prefix and properties, none of them leaking to a neighbour,
  * a member with no positive tag spec of its own is listed with `~[.]`, so
    the hidden cases a standalone suite never registered stay unregistered,
  * a member whose tags match nothing fails the BUILD with a named
    diagnostic rather than silently registering no tests,
  * a platform-gated member may explicitly opt into zero cases without
    weakening the fail-closed default for other members,
  * a member that links a library its group does not fails CONFIGURE,
  * the group reuses the shared Catch2 PCH even though its members'
    COMPILE_DEFINITIONS became per-source properties (the ledger names the
    group, every compile line names the carrier, and the definition still
    reaches its own TU), NO_PCH is accepted on the group, and NO_PCH on a
    member fails CONFIGURE pointing at the group.
"""

from __future__ import annotations

import json
import pathlib
import shutil
import subprocess
import sys
import tempfile
import textwrap


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "scripts"))
import pch_wiring_check  # noqa: E402

GROUP = "pulp-test-group-fixture"


def run(command: list[str], *, cwd: pathlib.Path | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, cwd=cwd, text=True, capture_output=True, check=False)


def require_success(result: subprocess.CompletedProcess[str], step: str) -> None:
    if result.returncode != 0:
        raise AssertionError(
            f"{step} failed with {result.returncode}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )


def fixture_project(
    root: pathlib.Path,
    *,
    empty_member: bool = False,
    empty_member_may_be_empty: bool = False,
    foreign_library: bool = False,
    cxx_standard: int = 17,
    group_no_pch: bool = False,
    member_no_pch: bool = False,
) -> None:
    group_options = " NO_PCH" if group_no_pch else ""
    member_options = "\n    NO_PCH" if member_no_pch else ""
    empty = ""
    if empty_member:
        may_be_empty = " MAY_BE_EMPTY" if empty_member_may_be_empty else ""
        empty = (
            "pulp_add_test_suite(pulp-test-gamma GROUP pulp-test-group-fixture "
            f"SOURCES gamma.cpp{may_be_empty})"
        )
    foreign = ""
    if foreign_library:
        foreign = (
            "add_library(elsewhere INTERFACE)\n"
            "pulp_add_test_suite(pulp-test-delta GROUP pulp-test-group-fixture "
            "SOURCES beta.cpp LIBRARIES elsewhere)"
        )
    (root / "CMakeLists.txt").write_text(
        textwrap.dedent(
            f"""\
            cmake_minimum_required(VERSION 3.20)
            project(pulp_test_group_probe LANGUAGES CXX)
            set(CMAKE_CXX_STANDARD {cxx_standard})
            set(CMAKE_CXX_STANDARD_REQUIRED ON)
            enable_testing()

            add_library(Catch2 INTERFACE)
            add_library(Catch2::Catch2 ALIAS Catch2)
            add_library(Catch2WithMain INTERFACE)
            add_library(Catch2::Catch2WithMain ALIAS Catch2WithMain)
            add_library(shared_lib INTERFACE)
            set(_CATCH_DISCOVER_TESTS_SCRIPT
                "{ROOT / 'tools/cmake/PulpCatchAddTests.cmake'}")
            include("{ROOT / 'tools/cmake/PulpCatch.cmake'}")
            include("{ROOT / 'tools/cmake/PulpTestSuite.cmake'}")

            pulp_add_test_group(pulp-test-group-fixture LIBRARIES shared_lib{group_options})
            pulp_add_test_suite(pulp-test-alpha GROUP pulp-test-group-fixture
                SOURCES alpha.cpp
                LIBRARIES shared_lib
                LABELS "alpha;widgets"
                TIMEOUT 7
                COMPILE_DEFINITIONS ALPHA_ONLY=1{member_options})
            pulp_add_test_suite(pulp-test-beta GROUP pulp-test-group-fixture
                SOURCES beta.cpp
                TEST_SPEC "~[slow]"
                PROPERTIES RESOURCE_LOCK beta-lock)
            pulp_add_test_suite(pulp-test-beta GROUP pulp-test-group-fixture
                SOURCES beta.cpp
                TEST_SPEC "[slow]"
                TEST_PREFIX "slow::"
                LABELS slow
                PROPERTIES RESOURCE_LOCK beta-lock)
            {empty}
            {foreign}
            """
        )
    )
    # The fake Catch2: lists cases only under --filenames-as-tags and only for
    # the per-file tag expressions a scoped discovery would send.
    (root / "alpha.cpp").write_text(
        textwrap.dedent(
            """\
            #include <iostream>
            #include <string>
            #include <string_view>

            #ifndef ALPHA_ONLY
            #error "the member's COMPILE_DEFINITIONS did not reach its own source"
            #endif

            int beta_symbol();

            int main(int argc, char** argv) {
                bool filenames_as_tags = false;
                std::string spec;
                for (int i = 1; i < argc; ++i) {
                    const std::string_view arg(argv[i]);
                    if (arg == "-#") filenames_as_tags = true;
                    else if (arg.substr(0, 1) == "[") spec = std::string(arg);
                }
                if (!filenames_as_tags) return 0;
                // A positive tag admits hidden ([.tag]) cases in Catch2, so a
                // member spec with no positive pattern of its own must carry
                // ~[.]; the unguarded spec lists the hidden case to prove it.
                if (spec == "[#alpha]~[.]") std::cout << "alpha one\\nalpha two\\n";
                else if (spec == "[#alpha]") std::cout << "alpha one\\nalpha two\\nalpha hidden\\n";
                else if (spec == "[#beta]~[slow]~[.]") std::cout << "beta fast\\n";
                else if (spec == "[#beta][slow]") std::cout << "beta slow\\n";
                return beta_symbol() == 42 ? 0 : 3;
            }
            """
        )
    )
    (root / "beta.cpp").write_text("int beta_symbol() { return 42; }\n")
    (root / "gamma.cpp").write_text("int gamma_symbol() { return 0; }\n")


def properties_of(test: dict[str, object]) -> dict[str, object]:
    return {item["name"]: item["value"] for item in test["properties"]}


def pch_configure_args(source: pathlib.Path) -> list[str]:
    """Point the shared-PCH machinery at the fixture: the carriers are created
    in the fixture's own directory and precompile standard headers only (the
    fake Catch2 has no headers). PULP_TEST_PCH itself is left at its default,
    so the test asserts the classifier's real verdict for this compiler."""
    args = [
        f"-DPULP_TEST_PCH_CARRIER_DIR={source}",
        "-DPULP_TEST_PCH_HEADERS=<string>;<vector>",
    ]
    if shutil.which("ninja"):
        args += ["-G", "Ninja"]
    return args


def pch_option_is_on(build: pathlib.Path) -> bool:
    cache = (build / "CMakeCache.txt").read_text()
    for line in cache.splitlines():
        if line.startswith("PULP_TEST_PCH:BOOL="):
            return line.split("=", 1)[1].strip().upper() in {"ON", "TRUE", "1", "YES"}
    raise AssertionError("PULP_TEST_PCH missing from the fixture's CMakeCache.txt")


def test_group_registers_each_member_with_its_own_properties() -> None:
    with tempfile.TemporaryDirectory(prefix="pulp-test-group-") as temporary:
        source = pathlib.Path(temporary) / "source"
        build = pathlib.Path(temporary) / "build"
        source.mkdir()
        fixture_project(source)

        require_success(run(["cmake", "-S", str(source), "-B", str(build)]), "configure")
        require_success(run(["cmake", "--build", str(build), "--parallel", "2"]), "build")
        listing = run(["ctest", "--test-dir", str(build), "--show-only=json-v1"])
        require_success(listing, "ctest listing")

        tests = json.loads(listing.stdout)["tests"]
        by_name = {test["name"]: test for test in tests}
        assert set(by_name) == {"alpha one", "alpha two", "beta fast", "slow::beta slow"}, by_name

        executables = {pathlib.Path(test["command"][0]).name for test in tests}
        assert executables == {"pulp-test-group-fixture"}, executables
        binaries = sorted(
            p.name for p in build.iterdir() if p.name.startswith("pulp-test-") and not p.suffix
        )
        assert binaries == ["pulp-test-group-fixture"], binaries

        alpha = properties_of(by_name["alpha one"])
        assert alpha["LABELS"] == ["alpha", "widgets"], alpha
        assert alpha["TIMEOUT"] == 7.0, alpha
        assert "RESOURCE_LOCK" not in alpha, alpha

        beta_fast = properties_of(by_name["beta fast"])
        assert beta_fast["RESOURCE_LOCK"] == ["beta-lock"], beta_fast
        assert "LABELS" not in beta_fast, beta_fast
        assert "TIMEOUT" not in beta_fast, beta_fast

        beta_slow = properties_of(by_name["slow::beta slow"])
        assert beta_slow["LABELS"] == ["slow"], beta_slow
        assert beta_slow["RESOURCE_LOCK"] == ["beta-lock"], beta_slow
        assert "TIMEOUT" not in beta_slow, beta_slow

        # The registered command is the plain test name; the scoping flags are
        # for discovery only and must not leak into how a case is run.
        assert by_name["beta fast"]["command"][1:] == ["beta fast"], by_name["beta fast"]

        # Mutation control on the oracle: a property that migrated to a
        # neighbour must be visible to these checks.
        leaked = dict(alpha)
        leaked["RESOURCE_LOCK"] = ["beta-lock"]
        try:
            assert "RESOURCE_LOCK" not in leaked
        except AssertionError:
            pass
        else:
            raise AssertionError("a leaked RESOURCE_LOCK escaped the property check")


def test_member_matching_nothing_fails_the_build() -> None:
    with tempfile.TemporaryDirectory(prefix="pulp-test-group-empty-") as temporary:
        source = pathlib.Path(temporary) / "source"
        build = pathlib.Path(temporary) / "build"
        source.mkdir()
        fixture_project(source, empty_member=True)

        require_success(run(["cmake", "-S", str(source), "-B", str(build)]), "configure")
        built = run(["cmake", "--build", str(build), "--parallel", "2"])
        assert built.returncode != 0, "a member with no matching cases must fail the build"
        combined = built.stdout + built.stderr
        assert "matched no test cases" in combined, combined
        assert "[#gamma]" in combined, combined


def test_member_may_be_empty_is_explicit() -> None:
    with tempfile.TemporaryDirectory(prefix="pulp-test-group-may-empty-") as temporary:
        source = pathlib.Path(temporary) / "source"
        build = pathlib.Path(temporary) / "build"
        source.mkdir()
        fixture_project(source, empty_member=True, empty_member_may_be_empty=True)

        require_success(run(["cmake", "-S", str(source), "-B", str(build)]), "configure")
        require_success(run(["cmake", "--build", str(build), "--parallel", "2"]), "build")
        listing = run(["ctest", "--test-dir", str(build), "--show-only=json-v1"])
        require_success(listing, "ctest listing")
        tests = json.loads(listing.stdout)["tests"]
        assert {test["name"] for test in tests} == {
            "alpha one",
            "alpha two",
            "beta fast",
            "slow::beta slow",
        }, tests


def test_member_may_not_link_beyond_its_group() -> None:
    with tempfile.TemporaryDirectory(prefix="pulp-test-group-foreign-") as temporary:
        source = pathlib.Path(temporary) / "source"
        build = pathlib.Path(temporary) / "build"
        source.mkdir()
        fixture_project(source, foreign_library=True)

        configured = run(["cmake", "-S", str(source), "-B", str(build)])
        assert configured.returncode != 0, configured.stdout
        # CMake re-wraps message() text, so match the pieces rather than the line.
        assert "links elsewhere, which group" in configured.stderr, configured.stderr
        assert "pulp-test-group-fixture does not" in configured.stderr.replace("\n  ", " "), (
            configured.stderr
        )


def test_group_reuses_the_shared_pch_with_per_source_definitions() -> None:
    with tempfile.TemporaryDirectory(prefix="pulp-test-group-pch-") as temporary:
        source = pathlib.Path(temporary) / "source"
        build = pathlib.Path(temporary) / "build"
        source.mkdir()
        fixture_project(source, cxx_standard=20)

        configure = ["cmake", "-S", str(source), "-B", str(build), *pch_configure_args(source)]
        require_success(run(configure), "configure")
        option_on = pch_option_is_on(build)
        ledger = pch_wiring_check.read_ledger(build)
        assert GROUP in ledger.entries, sorted(ledger.entries)
        status, detail = ledger.entries[GROUP]
        if option_on:
            # The member's COMPILE_DEFINITIONS are per-source properties on
            # the group; that must not read as a per-source flag and cost the
            # group its PCH.
            assert (status, detail) == ("pch", "pulp-test-pch-cxx20"), (status, detail)
            problems = pch_wiring_check.check(
                build, True, {GROUP: "pulp-test-pch-cxx20"}
            )
            assert not problems, problems
        else:
            assert status == "off", (status, detail)
        # alpha.cpp #errors unless ALPHA_ONLY reached it, so a green build is
        # the proof that a per-source definition survives next to the PCH.
        require_success(run(["cmake", "--build", str(build), "--parallel", "2"]), "build")
        print(f"  pch fixture: PULP_TEST_PCH={'ON' if option_on else 'OFF'}, {GROUP} -> {status} {detail}")


def test_no_pch_is_a_group_option() -> None:
    with tempfile.TemporaryDirectory(prefix="pulp-test-group-nopch-") as temporary:
        source = pathlib.Path(temporary) / "source"
        build = pathlib.Path(temporary) / "build"
        source.mkdir()
        fixture_project(source, cxx_standard=20, group_no_pch=True)
        configure = ["cmake", "-S", str(source), "-B", str(build), *pch_configure_args(source)]
        require_success(run(configure), "configure")
        status, detail = pch_wiring_check.read_ledger(build).entries[GROUP]
        if pch_option_is_on(build):
            assert (status, detail) == ("skip", "no-pch-requested"), (status, detail)
        else:
            assert status == "off", (status, detail)

    with tempfile.TemporaryDirectory(prefix="pulp-test-group-member-nopch-") as temporary:
        source = pathlib.Path(temporary) / "source"
        build = pathlib.Path(temporary) / "build"
        source.mkdir()
        fixture_project(source, cxx_standard=20, member_no_pch=True)
        configured = run(["cmake", "-S", str(source), "-B", str(build), *pch_configure_args(source)])
        assert configured.returncode != 0, configured.stdout
        flat = configured.stderr.replace("\n  ", " ")
        assert "NO_PCH on a GROUP member" in flat, configured.stderr
        assert f"pulp_add_test_group({GROUP})" in flat, configured.stderr


def main() -> int:
    test_group_registers_each_member_with_its_own_properties()
    test_member_matching_nothing_fails_the_build()
    test_member_may_be_empty_is_explicit()
    test_member_may_not_link_beyond_its_group()
    test_group_reuses_the_shared_pch_with_per_source_definitions()
    test_no_pch_is_a_group_option()
    print("pulp test group tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
