#!/usr/bin/env python3
"""End-to-end regression test for Pulp's Catch2 multi-label discovery."""

from __future__ import annotations

import json
import pathlib
import subprocess
import tempfile
import textwrap


ROOT = pathlib.Path(__file__).resolve().parents[2]


def run(command: list[str], *, cwd: pathlib.Path | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, cwd=cwd, text=True, capture_output=True, check=False)


def fixture_project(root: pathlib.Path, *, invalid_argument: bool = False) -> None:
    extra = 'TYPO "value"\n                ' if invalid_argument else ""
    (root / "CMakeLists.txt").write_text(
        textwrap.dedent(
            f"""\
            cmake_minimum_required(VERSION 3.20)
            project(pulp_catch_multilabel_probe LANGUAGES CXX)
            set(CMAKE_CXX_STANDARD 17)
            set(CMAKE_CXX_STANDARD_REQUIRED ON)
            enable_testing()

            add_library(Catch2WithMain INTERFACE)
            add_library(Catch2::Catch2WithMain ALIAS Catch2WithMain)
            set(_CATCH_DISCOVER_TESTS_SCRIPT
                "{ROOT / 'tools/cmake/PulpCatchAddTests.cmake'}")
            include("{ROOT / 'tools/cmake/PulpCatch.cmake'}")
            include("{ROOT / 'tools/cmake/PulpTestSuite.cmake'}")

            pulp_add_test_suite(pulp-test-fixture
                {extra}SOURCES "${{CMAKE_CURRENT_SOURCE_DIR}}/fixture.cpp"
                TEST_SPEC "~[slow]"
                TEST_PREFIX "fast::"
                LABELS "audio;quality-lab;performance"
                TIMEOUT 30
                PROPERTIES RUN_SERIAL TRUE)
            catch_discover_tests(pulp-test-fixture
                TEST_SPEC "[slow]"
                TEST_PREFIX "slow::"
                PROPERTIES COST 2
                LABELS slow)
            catch_discover_tests(pulp-test-fixture
                TEST_SPEC "[unlabeled]"
                TEST_PREFIX "unlabeled::"
                PROPERTIES TIMEOUT 11)
            """
        )
    )
    (root / "fixture.cpp").write_text(
        textwrap.dedent(
            """\
            #include <iostream>
            #include <string>
            #include <string_view>

            int main(int argc, char** argv) {
                std::string spec;
                for (int i = 1; i < argc; ++i) {
                    const std::string_view arg(argv[i]);
                    if (std::string_view(argv[i]) == "--list-tests") {
                        continue;
                    }
                    if (arg == "~[slow]" || arg == "[slow]" || arg == "[unlabeled]")
                        spec = arg;
                }
                if (spec == "~[slow]") std::cout << "fast case\\n";
                else if (spec == "[slow]") std::cout << "slow case\\n";
                else if (spec == "[unlabeled]") std::cout << "unlabeled case\\n";
                return 0;
            }
            """
        )
    )


def require_success(result: subprocess.CompletedProcess[str], step: str) -> None:
    if result.returncode != 0:
        raise AssertionError(
            f"{step} failed with {result.returncode}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )


def require_partition(
    tests: list[dict[str, object]], gated_label: str, expected_gated: set[str]
) -> None:
    def labels(test: dict[str, object]) -> set[str]:
        for prop in test["properties"]:
            if prop["name"] == "LABELS":
                return set(prop["value"])
        return set()

    gated = {test["name"] for test in tests if gated_label in labels(test)}
    off_gate = {test["name"] for test in tests if gated_label not in labels(test)}
    assert gated == expected_gated
    assert off_gate.isdisjoint(gated)
    assert off_gate | gated == {test["name"] for test in tests}


def test_multilabel_properties() -> None:
    with tempfile.TemporaryDirectory(prefix="pulp-catch-multilabel-") as temporary:
        source = pathlib.Path(temporary) / "source"
        build = pathlib.Path(temporary) / "build"
        source.mkdir()
        fixture_project(source)

        require_success(run(["cmake", "-S", str(source), "-B", str(build)]), "configure")
        require_success(run(["cmake", "--build", str(build), "--parallel", "2"]), "build")
        listing = run(["ctest", "--test-dir", str(build), "--show-only=json-v1"])
        require_success(listing, "ctest listing")

        tests = json.loads(listing.stdout)["tests"]
        assert len(tests) == 3, tests
        tests_by_name = {test["name"]: test for test in tests}
        assert set(tests_by_name) == {
            "fast::fast case",
            "slow::slow case",
            "unlabeled::unlabeled case",
        }, tests_by_name
        properties = {
            item["name"]: item["value"]
            for item in tests_by_name["fast::fast case"]["properties"]
        }
        assert properties["LABELS"] == ["audio", "performance", "quality-lab"], properties
        assert properties["RUN_SERIAL"] is True, properties
        assert properties["TIMEOUT"] == 30.0, properties
        for bogus in ("audio", "performance", "quality-lab"):
            assert bogus not in properties, properties
        slow = {
            item["name"]: item["value"]
            for item in tests_by_name["slow::slow case"]["properties"]
        }
        assert slow["LABELS"] == ["slow"], slow
        assert slow["COST"] == 2.0, slow
        unlabeled = {
            item["name"]: item["value"]
            for item in tests_by_name["unlabeled::unlabeled case"]["properties"]
        }
        assert "LABELS" not in unlabeled, unlabeled
        assert unlabeled["TIMEOUT"] == 11.0, unlabeled

        require_partition(tests, "slow", {"slow::slow case"})

        # Mutation control: stripping the routing label must make the inventory
        # oracle fail, rather than silently treating the expensive case as fast.
        mutated = json.loads(json.dumps(tests))
        mutated_slow = next(test for test in mutated if test["name"] == "slow::slow case")
        mutated_slow["properties"] = [
            prop for prop in mutated_slow["properties"] if prop["name"] != "LABELS"
        ]
        try:
            require_partition(mutated, "slow", {"slow::slow case"})
        except AssertionError:
            pass
        else:
            raise AssertionError("routing-label mutation escaped the inventory oracle")

        explicit = run([str(build / "pulp-test-fixture"), "[slow]"])
        require_success(explicit, "explicit slow execution")


def test_unparsed_arguments_fail_closed() -> None:
    with tempfile.TemporaryDirectory(prefix="pulp-catch-unparsed-") as temporary:
        source = pathlib.Path(temporary) / "source"
        build = pathlib.Path(temporary) / "build"
        source.mkdir()
        fixture_project(source, invalid_argument=True)

        configured = run(["cmake", "-S", str(source), "-B", str(build)])
        assert configured.returncode != 0, configured.stdout
        assert "unparsed arguments: TYPO;value" in configured.stderr, configured.stderr


def main() -> int:
    test_multilabel_properties()
    test_unparsed_arguments_fail_closed()
    print("catch multi-label property tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
