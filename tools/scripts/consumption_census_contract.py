#!/usr/bin/env python3
"""Prove the consumption-census gate can fail, one failure mode at a time.

A gate nobody has watched go red is a decoration. This driver feeds
`consumption_census.py --check` a census that has drifted, a census that
violates its schema, a build tree whose facts are missing, and a build tree
whose graph contains something the generator must refuse to guess at — and
asserts each one is rejected with the right exit status. It also regenerates
from a tree configured with different switches and requires the profile already
recorded for that platform to survive, because a census that lets one
configuration overwrite another goes quiet instead of going red. The first case
is a control: it feeds the unmodified inputs and requires them to PASS, so a
driver that rejects everything (including correct input) cannot be mistaken for
a working gate.

Exit statuses under test:
    0   the census matches the build tree
    1   the census is wrong: it drifted, is missing, violates its schema, or
        records a feature set its own profile key does not describe
    2   the census could not be measured at all
    77  this build tree is a profile the census does not record, so nothing
        was compared — visibly a skip, never a pass
"""

from __future__ import annotations

import argparse
import copy
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

CENSUS_RELPATH = Path("docs/status/consumption-profiles.json")
SCHEMA_RELPATH = Path("docs/status/consumption-profiles.schema.json")
FACTS_NAME = "consumption-census-facts.json"


class ContractFailure(RuntimeError):
    pass


def stage_build_dir(build_dir: Path, destination: Path) -> Path:
    """Copy the minimum a census run reads: the facts dump and the export file."""
    staged = destination / "build"
    staged.mkdir(parents=True)
    shutil.copy2(build_dir / FACTS_NAME, staged / FACTS_NAME)
    exports = sorted(build_dir.glob("CMakeFiles/Export/*/PulpTargets.cmake"))
    if not exports:
        raise ContractFailure(f"{build_dir} has no generated PulpTargets.cmake to stage")
    target_dir = staged / "CMakeFiles" / "Export" / exports[0].parent.name
    target_dir.mkdir(parents=True)
    shutil.copy2(exports[0], target_dir / "PulpTargets.cmake")
    return staged


def run_check(script: Path, build_dir: Path, census: Path, schema: Path) -> subprocess.CompletedProcess:
    return subprocess.run(
        [
            sys.executable,
            str(script),
            "--check",
            "--build-dir",
            str(build_dir),
            "--census",
            str(census),
            "--schema",
            str(schema),
        ],
        capture_output=True,
        text=True,
    )


def run_write(script: Path, build_dir: Path, census: Path, schema: Path) -> subprocess.CompletedProcess:
    return subprocess.run(
        [
            sys.executable,
            str(script),
            "--write",
            "--build-dir",
            str(build_dir),
            "--census",
            str(census),
            "--schema",
            str(schema),
        ],
        capture_output=True,
        text=True,
    )


def expect(case: str, result: subprocess.CompletedProcess, status: int) -> None:
    if result.returncode != status:
        raise ContractFailure(
            f"case {case}: expected exit {status}, got {result.returncode}\n"
            f"stdout: {result.stdout}\nstderr: {result.stderr}"
        )
    print(f"consumption_census_contract_case={case}")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--repo-root", type=Path, required=True)
    args = parser.parse_args(argv)

    repo_root = args.repo_root.resolve()
    script = repo_root / "tools" / "scripts" / "consumption_census.py"
    schema = repo_root / SCHEMA_RELPATH
    document = json.loads((repo_root / CENSUS_RELPATH).read_text())

    try:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            staged = stage_build_dir(args.build_dir.resolve(), root)
            census_path = root / "census.json"

            def write(doc: dict) -> Path:
                census_path.write_text(json.dumps(doc, indent=2) + "\n")
                return census_path

            # Control. If the untouched inputs do not pass, every rejection
            # below is meaningless — the driver would be measuring itself.
            baseline = run_check(script, staged, write(document), schema)
            if baseline.returncode == 77:
                print(
                    "consumption_census_contract_skipped reason=profile "
                    "detail=the staged build tree is not a recorded profile",
                    file=sys.stderr,
                )
                return 77
            expect("valid-current", baseline, 0)

            key = next(iter(document["profiles"]))
            # A key is "<system>-<processor>-<enabled features>". Synthetic
            # profiles below move a real one to another platform, so they keep
            # the feature half and swap only the platform half.
            recorded = document["profiles"][key]
            platform_half = (
                f"{recorded['system_name'].lower()}-{recorded['system_processor'].lower()}-"
            )
            feature_half = key[len(platform_half):]

            def mutated(change) -> dict:
                copy_of = copy.deepcopy(document)
                change(copy_of["profiles"][key])
                return copy_of

            missing = root / "absent.json"
            expect("census-missing", run_check(script, staged, missing, schema), 1)

            name = sorted(document["profiles"][key]["targets"])[0]

            def drift(profile: dict) -> None:
                profile["targets"][name]["closure"]["node_count"] += 1

            expect("closure-count-drift", run_check(script, staged, write(mutated(drift)), schema), 1)

            def remove(profile: dict) -> None:
                profile["targets"].pop(name)
                profile["summary"]["installed_target_count"] -= 1
                profile["summary"]["ranked_by_closure"] = [
                    row for row in profile["summary"]["ranked_by_closure"]
                    if row["exported_as"] != f"Pulp::{name}"
                ]

            expect(
                "exported-target-removed",
                run_check(script, staged, write(mutated(remove)), schema),
                1,
            )

            def add(profile: dict) -> None:
                invented = copy.deepcopy(profile["targets"][name])
                invented["exported_as"] = "Pulp::invented"
                profile["targets"]["invented"] = invented

            expect(
                "exported-target-added",
                run_check(script, staged, write(mutated(add)), schema),
                1,
            )

            invalid = copy.deepcopy(document)
            invalid["derivation"]["build_time_derived"] = False
            expect("schema-violation", run_check(script, staged, write(invalid), schema), 1)

            # A census that records no profile for this host states an absence
            # rather than a match, so it must SKIP and never read as a pass.
            renamed = copy.deepcopy(document)
            foreign_key = f"somethingelse-riscv64-{feature_half}"
            renamed["profiles"] = {foreign_key: renamed["profiles"].pop(key)}
            renamed["profiles"][foreign_key]["key"] = foreign_key
            renamed["profiles"][foreign_key]["system_name"] = "SomethingElse"
            renamed["profiles"][foreign_key]["system_processor"] = "riscv64"
            expect("profile-not-recorded", run_check(script, staged, write(renamed), schema), 77)

            # A profile whose key says one feature set and whose body says
            # another contradicts itself. Nothing can be compared against it,
            # and calling that "a different profile" would retire the gate.
            def flip_feature(profile: dict) -> None:
                feature = sorted(profile["features"])[0]
                profile["features"][feature] = "OFF" if profile["features"][feature] == "ON" else "ON"

            expect(
                "profile-feature-mismatch",
                run_check(script, staged, write(mutated(flip_feature)), schema),
                1,
            )

            # Regenerating from a different feature set must ADD a profile, not
            # replace the one already recorded for this platform. A census keyed
            # on the platform alone loses the first silently: the two closures
            # genuinely differ, and afterwards nothing says they disagreed.
            clobber = root / "clobber"
            clobber_build = stage_build_dir(args.build_dir.resolve(), clobber)
            clobber_facts = json.loads((clobber_build / FACTS_NAME).read_text())
            enabled = sorted(
                name
                for name in document["profiles"][key]["features"]
                if clobber_facts["features"].get(name) == "ON"
            )
            if not enabled:
                raise ContractFailure(
                    "case profile-not-clobbered: this tree enables no feature to switch off, "
                    "so a second configuration cannot be simulated"
                )
            switched_off = enabled[0]
            clobber_facts["features"][switched_off] = "OFF"
            (clobber_build / FACTS_NAME).write_text(json.dumps(clobber_facts))

            clobber_census = root / "clobber.json"
            clobber_census.write_text(json.dumps(document, indent=2) + "\n")
            written = run_write(script, clobber_build, clobber_census, schema)
            if written.returncode != 0:
                raise ContractFailure(
                    f"case profile-not-clobbered: --write from a {switched_off}=OFF tree "
                    f"exited {written.returncode}\nstderr: {written.stderr}"
                )
            reloaded = json.loads(clobber_census.read_text())["profiles"]
            if key not in reloaded:
                raise ContractFailure(
                    f"case profile-not-clobbered: writing a {switched_off}=OFF profile "
                    f"removed {key}; recorded now: {', '.join(sorted(reloaded))}"
                )
            added = sorted(set(reloaded) - set(document["profiles"]))
            if len(added) != 1:
                raise ContractFailure(
                    f"case profile-not-clobbered: expected one added profile, got {added}"
                )
            if reloaded[key] != document["profiles"][key]:
                raise ContractFailure(
                    f"case profile-not-clobbered: {key} was rewritten in place"
                )
            # And the surviving profile still answers for the tree it describes,
            # so the gate is live rather than merely present.
            expect(
                "profile-not-clobbered",
                run_check(script, staged, clobber_census, schema),
                0,
            )

            # A profile for a platform this host cannot measure must not make
            # this host's check fail — several profiles share one document.
            foreign = copy.deepcopy(document)
            other = copy.deepcopy(foreign["profiles"][key])
            other["key"] = f"linux-x86_64-{feature_half}"
            other["system_name"] = "Linux"
            other["system_processor"] = "x86_64"
            foreign["profiles"][other["key"]] = other
            expect("foreign-profile-ignored", run_check(script, staged, write(foreign), schema), 0)

            # A build tree with no facts cannot be measured, and the census
            # must say so rather than fall back to reading the CMakeLists.
            factless = root / "factless"
            (factless / "build").mkdir(parents=True)
            expect(
                "facts-missing",
                run_check(script, factless / "build", write(document), schema),
                2,
            )

            # A generator expression the resolver does not understand is a
            # dependency edge measured wrong. Refusing beats guessing.
            unknown = root / "unknown"
            unknown_build = stage_build_dir(args.build_dir.resolve(), unknown)
            facts = json.loads((unknown_build / FACTS_NAME).read_text())
            victim = next(
                target for target, value in facts["targets"].items()
                if value["interface_link_libraries"]
            )
            facts["targets"][victim]["interface_link_libraries"].append(
                "$<HOST_LINK:pulp-runtime>"
            )
            (unknown_build / FACTS_NAME).write_text(json.dumps(facts))
            expect(
                "unknown-generator-expression",
                run_check(script, unknown_build, write(document), schema),
                2,
            )

            # An export that names a library the facts dump does not carry
            # means the two halves of the instrument disagree, so no closure
            # read from that tree can be trusted.
            orphan = root / "orphan"
            orphan_build = stage_build_dir(args.build_dir.resolve(), orphan)
            export_file = next(orphan_build.glob("CMakeFiles/Export/*/PulpTargets.cmake"))
            export_file.write_text(
                export_file.read_text() + "\nadd_library(Pulp::phantom STATIC IMPORTED)\n"
            )
            expect(
                "export-without-target",
                run_check(script, orphan_build, write(document), schema),
                2,
            )
    except ContractFailure as error:
        print(f"consumption_census_contract: {error}", file=sys.stderr)
        return 1

    print("consumption_census_contract_verified=true")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
