#!/usr/bin/env python3
"""Compile planted production-source and consumer-fixture mutations against an installed Pulp SDK.

Production mutations edit scratch copies of the binding and voice-template
sources, which are compiled into the consumer ahead of the installed library.
Fixture mutations edit scratch copies of the consumer's own main.cpp or
fixture.hpp. The repository sources are never modified.
"""

import argparse
import hashlib
import json
import shutil
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / "examples/g6-voice-bank-consumer"
PRODUCTION = ROOT / "core/host/src/voice_bank_binding.cpp"
TEMPLATE = ROOT / "core/host/src/voice_template.cpp"

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
        "if (free == membership.end())\n                return refuse(VoiceBankBindingCode::TrackerCapacity);",
        "if (free == membership.end())\n                continue;",
        "capacity-typed-refusal",
    ),
    "skip_cc120_capacity_preview": (
        "message.cc_number() == 120 || message.cc_number() == 123",
        "false || message.cc_number() == 123",
        "capacity-cc120-reuse",
    ),
    "skip_cc123_capacity_preview": (
        "message.cc_number() == 120 || message.cc_number() == 123",
        "message.cc_number() == 120 || false",
        "capacity-cc123-reuse",
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
        "if (!scope.manager)\n            return scope.channel == channel;",
        "if (!scope.manager)\n            return true;",
        "member-panic-preserves-other",
    ),
    "accept_invalid_deferred_expression": (
        "if (!valid_deferred_expression(kind, state, impl_->tracker.member_bend_range(state.channel)))",
        "if (false)",
        "deferred-expression-refusal",
    ),
}

# Mutations of other scratch copies: {name: (target, before, after, marker)}.
# Targets: "binding" and "template" are production copies; "main" and
# "fixture" are the consumer's own sources.
TARGETED_MUTATIONS = {
    "activate_skips_owner_reset": (
        "binding",
        "    impl_->tracker.reset();\n    impl_->owner->reset();",
        "    impl_->tracker.reset();",
        "swap-voices-cut",
    ),
    "activate_keeps_epoch": (
        "binding",
        "    ++impl_->epoch;\n    impl_->ingress.reset(impl_->epoch);",
        "    impl_->ingress.reset(impl_->epoch);",
        "swap-epoch-advanced",
    ),
    "activate_inside_open_block": (
        "binding",
        "    if (impl_->block_open) {\n        result = {VoiceBankBindingCode::InvalidBlock",
        "    if (false) {\n        result = {VoiceBankBindingCode::InvalidBlock",
        "swap-activate-open-block-refusal",
    ),
    "skip_parameter_upper_bound": (
        "binding",
        "value > bounds->second.maximum)",
        "false)",
        "restore-out-of-range-reapply-refusal",
    ),
    "restore_ignores_manifest_identity": (
        "template",
        "candidate.manifest_sha256 != manifest.sha256)",
        "false)",
        "restore-sha-mismatch-refusal",
    ),
    "normalize_accepts_forged_slot": (
        "template",
        "if (value.slot_ordinal >= kSlots || !std::isfinite(value.value))",
        "if (!std::isfinite(value.value))",
        "restore-wire-forged-slot-refusal",
    ),
    "normalize_accepts_non_finite": (
        "template",
        "if (value.slot_ordinal >= kSlots || !std::isfinite(value.value))",
        "if (value.slot_ordinal >= kSlots)",
        "restore-non-finite-refusal",
    ),
    "drop_restore_reapply": (
        "fixture",
        "        if (!bank.set_slot_parameter(value.slot_ordinal, value.target, value.value))\n"
        "            return false;",
        "        (void)value;",
        "restore-audio-bit-exact",
    ),
    "restore_onto_live_bank": (
        "main",
        "VoiceBankBinding& target = *fresh;",
        "VoiceBankBinding& target = *live;",
        "restore-no-active-voices",
    ),
    "skip_replacement_epoch_advance": (
        "main",
        "    while (replacement->epoch() <= retired_epoch)\n"
        "        check(replacement->activate(status), \"cross-bank-advance-epoch\");\n",
        "",
        "cross-bank-retired-token-stale-epoch",
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
    originals = {
        "binding": ("voice_bank_binding.cpp", PRODUCTION.read_text()),
        "template": ("voice_template.cpp", TEMPLATE.read_text()),
        "main": ("main.cpp", (FIXTURE / "main.cpp").read_text()),
        "fixture": ("fixture.hpp", (FIXTURE / "fixture.hpp").read_text()),
    }
    subprocess.run(["python3", str(FIXTURE / "generate_a1_vectors.py"), "--check"], check=True)
    provenance = json.loads((prefix / "sdk-provenance.json").read_text())
    cases = [
        ("positive", None),
        *((name, ("binding", *mutation)) for name, mutation in MUTATIONS.items()),
        *TARGETED_MUTATIONS.items(),
        ("restored", None),
    ]
    results = {}
    for name, mutation in cases:
        source = scratch / name
        source.mkdir(exist_ok=True)
        shutil.copyfile(FIXTURE / "a1_reference_vectors.hpp", source / "a1_reference_vectors.hpp")
        texts = {target: text for target, (_, text) in originals.items()}
        if mutation:
            target, before, after, _ = mutation
            count = texts[target].count(before)
            if count != 1:
                raise AssertionError(f"{name}: {target} anchor count={count}")
            texts[target] = texts[target].replace(before, after, 1)
        for target, (filename, _) in originals.items():
            (source / filename).write_text(texts[target])
        (source / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.24)\n"
            "project(G6ProductionMutation LANGUAGES CXX)\n"
            "find_package(Pulp CONFIG REQUIRED COMPONENTS host)\n"
            "add_executable(g6-production-mutation main.cpp voice_bank_binding.cpp voice_template.cpp)\n"
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
            marker = mutation[3]
            if execution.returncode == 0 or marker not in output:
                raise AssertionError(f"{name}: planted failure was not observed: {output}")
        elif (execution.returncode or "g6-voice-bank-public-consumer PASS" not in output
              or "sample10=0" not in output):
            raise AssertionError(f"{name}: positive numeric oracle failed: {output}")
        results[name] = {
            "exit_code": execution.returncode,
            "binary_sha256": sha256(binary),
            "source_sha256": sha256(source / "voice_bank_binding.cpp"),
            "mutated_file": mutation[0] if mutation else None,
            "sources_sha256": {
                filename: sha256(source / filename) for filename, _ in originals.values()
            },
            "output": output.strip(),
        }
        print(f"{name}: exit={execution.returncode} {output.strip()}", flush=True)
    if results["positive"]["sources_sha256"] != results["restored"]["sources_sha256"]:
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
