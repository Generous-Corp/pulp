#!/usr/bin/env python3
"""Compile planted production-source mutations against an installed Pulp SDK."""

import argparse
import hashlib
import json
import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / "examples/g6-voice-bank-consumer"
PRODUCTION = ROOT / "core/host/src/voice_bank_binding.cpp"

MUTATIONS = {
    "interval_input_default_empty": (
        "audio::BufferView<const float> input(nullptr, 0, at - offset);",
        "audio::BufferView<const float> input;",
        "render-code=6",
    ),
    "tail_input_default_empty": (
        "audio::BufferView<const float> input(nullptr, 0, frames - offset);",
        "audio::BufferView<const float> input;",
        "render-code=6",
    ),
    "allow_aliased_slot_state": (
        "if (identity == owner->slot_state_identity(earlier)) {",
        "if (false) {",
        "aliased-state-refusal",
    ),
    "allow_wrong_binding_path": (
        "if (spec.path != definition.binding_path) {",
        "if (false) {",
        "wrong-path-refusal",
    ),
    "accept_stale_epoch": (
        "if (epoch != impl_->epoch)\n        return VoiceBankBindingCode::StaleEpoch;",
        "if (false)\n        return VoiceBankBindingCode::StaleEpoch;",
        "stale-epoch-refusal",
    ),
    "ignore_steal_policy": (
        "impl_->owner->set_steal_mode(mode);",
        "(void)mode;",
        "steal-victim",
    ),
    "suppress_note_release": (
        "owner->dispatch({0, midi::MpeExpressionEvent::Kind::NoteOff, note});",
        "(void)note;",
        "finite-release-reclaimed",
    ),
    "skip_tracker_capacity_refusal": (
        "if (free == membership.end()) return refuse(VoiceBankBindingCode::TrackerCapacity);",
        "if (free == membership.end()) continue;",
        "capacity-typed-refusal",
    ),
    "silently_accept_poly_pressure": (
        "const bool supported = kind == 0x80 || kind == 0x90 || kind == 0xd0 ||",
        "const bool supported = kind == 0x80 || kind == 0x90 || kind == 0xa0 || kind == 0xd0 ||",
        "poly-pressure-typed-refusal",
    ),
    "wrong_cc64_boundary": (
        "if (control == 64) {",
        "if (control == 64 && value != 64) {",
        "pedal-64-down",
    ),
    "skip_cc121_hold_drain": (
        "reset_orphan_expression(scope);\n            release_unheld();",
        "reset_orphan_expression(scope);",
        "cc121-drains-last-hold",
    ),
    "skip_cc123_note_off": (
        "} else if (control == 123) {\n            all_notes_off(scope);",
        "} else if (control == 123) {\n            (void)scope;",
        "cc123-dispatches-release",
    ),
    "widen_member_panic": (
        "if (!scope.manager) return scope.channel == channel;",
        "if (!scope.manager) return true;",
        "member-panic-preserves-other",
    ),
}


def run(command: list[str], log: Path, cwd: Path) -> subprocess.CompletedProcess[str]:
    outcome = subprocess.run(command, cwd=cwd, capture_output=True, text=True, timeout=300)
    log.write_text(outcome.stdout + outcome.stderr)
    return outcome


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk-prefix", type=Path, required=True)
    parser.add_argument("--scratch", type=Path, required=True)
    args = parser.parse_args()
    prefix = args.sdk_prefix.resolve(strict=True)
    scratch = args.scratch.resolve()
    scratch.mkdir(parents=True, exist_ok=True)
    assert (prefix / "lib/cmake/Pulp/PulpConfig.cmake").is_file()
    original = PRODUCTION.read_text()
    provenance = json.loads((prefix / "sdk-provenance.json").read_text())
    cases = [("positive", None), *MUTATIONS.items(), ("restored", None)]
    results = {}
    for name, mutation in cases:
        source = scratch / name
        source.mkdir(exist_ok=True)
        shutil.copyfile(FIXTURE / "main.cpp", source / "main.cpp")
        shutil.copyfile(FIXTURE / "fixture.hpp", source / "fixture.hpp")
        code = original
        if mutation:
            before, after, _ = mutation
            if code.count(before) != 1:
                raise AssertionError(f"{name}: production anchor count={code.count(before)}")
            code = code.replace(before, after, 1)
        (source / "voice_bank_binding.cpp").write_text(code)
        (source / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.24)\n"
            "project(G6ProductionMutation LANGUAGES CXX)\n"
            "find_package(Pulp CONFIG REQUIRED COMPONENTS host)\n"
            "add_executable(g6-production-mutation main.cpp voice_bank_binding.cpp)\n"
            "target_compile_features(g6-production-mutation PRIVATE cxx_std_20)\n"
            "target_link_libraries(g6-production-mutation PRIVATE Pulp::host)\n"
        )
        build = source / "build"
        configure = run(
            ["cmake", "-S", str(source), "-B", str(build), "-G", "Ninja",
             f"-DPulp_DIR={prefix / 'lib/cmake/Pulp'}",
             f"-DCMAKE_PREFIX_PATH={prefix}", "-DCMAKE_BUILD_TYPE=Release"],
            source / "configure.log", ROOT,
        )
        if configure.returncode:
            raise RuntimeError(f"{name}: configure failed; see {source / 'configure.log'}")
        build_step = run(
            ["/usr/bin/env", "PULP_BUILD_JOBS=1", "PULP_GOVERNED_BUILD_WAIT_SECS=0",
             str(ROOT / "tools/ci/governed-build.sh"),
             "cmake", "--build", str(build), "--target", "g6-production-mutation"],
            source / "build.log", ROOT,
        )
        if build_step.returncode:
            raise RuntimeError(f"{name}: build failed; see {source / 'build.log'}")
        binary = build / "g6-production-mutation"
        execution = run([str(binary)], source / "run.log", ROOT)
        output = execution.stdout + execution.stderr
        if mutation:
            marker = mutation[2]
            if execution.returncode == 0 or marker not in output:
                raise AssertionError(f"{name}: planted failure was not observed: {output}")
        elif (execution.returncode or "g6-voice-bank-public-consumer PASS" not in output
              or "sample10=0" not in output):
            raise AssertionError(f"{name}: positive numeric oracle failed: {output}")
        results[name] = {
            "exit_code": execution.returncode,
            "binary_sha256": sha256(binary),
            "source_sha256": sha256(source / "voice_bank_binding.cpp"),
            "output": output.strip(),
        }
        print(f"{name}: exit={execution.returncode} {output.strip()}", flush=True)
    if results["positive"]["source_sha256"] != results["restored"]["source_sha256"]:
        raise AssertionError("restored source differs from positive source")
    receipt = {
        "schema": "pulp.g6-production-mutation-proof.v1",
        "source_head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
        "source_git_dirty": bool(subprocess.check_output(
            ["git", "status", "--porcelain"], cwd=ROOT, text=True).strip()),
        "sdk_provenance": provenance,
        "cases": results,
    }
    (scratch / "receipt.json").write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n")
    print(f"PASS {len(results)} positive/planted/restored cases", flush=True)


if __name__ == "__main__":
    main()
