#!/usr/bin/env python3
"""Execute a Shipyard-selected literal CTest file without regex construction.

This adapter is intentionally narrow. Shipyard owns exact-head selection and
writes one literal test name per line. This script independently proves that
the current CTest registrations match the protected base's (derived by
configuring the exact base commit, never a committed list), that every
requested name expands to the expected registrations (including duplicate
display names), and only then invokes CTest with ``--tests-from-file`` as an
argv element. Any ambiguity exits before a test process starts.
"""

from __future__ import annotations

import argparse
import base64
import binascii
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import tomllib
from pathlib import Path
from typing import Any, Sequence

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "ci"))

import build_dir_lock
import changed_surface_inventory as inventory
import lane_reuse_record


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_CONFIG = REPO_ROOT / ".shipyard" / "config.toml"
# Base inventories are derived once per (base, configure flags) and kept beside
# the build tree, so later selections against the same base reuse them.
BASE_INVENTORY_CACHE = ".changed-surface-base"
EXCLUDE_NAME = inventory.EXCLUDED_NAME_REGEX
EXCLUDE_LABEL = inventory.EXCLUDED_LABEL_REGEX
MINIMUM_CTEST_VERSION = (3, 29)
# Shipyard keeps the base64-expanded command below cmd.exe's 8,191-character
# ceiling. Larger selections conservatively stay on the full validation path.
MAX_SELECTED_TEST_BYTES = 4 * 1024


class SelectionExecutionError(ValueError):
    """The literal selection cannot safely replace the full test stage."""


class FullAuthorityExecutionError(RuntimeError):
    """The ordinary full validation failed before it could report a result."""


def parse_literal_selection(payload: bytes) -> list[str]:
    """Parse a UTF-8, newline-delimited set of literal test names."""

    if b"\0" in payload or b"\r" in payload:
        raise SelectionExecutionError("selected-tests file contains an invalid line boundary")
    try:
        text = payload.decode("utf-8", errors="strict")
    except UnicodeDecodeError as error:
        raise SelectionExecutionError("selected-tests file is not UTF-8") from error
    if not text.endswith("\n"):
        raise SelectionExecutionError("selected-tests file must end with one newline")
    names = text[:-1].split("\n")
    if not names or any(not name.strip() for name in names):
        raise SelectionExecutionError("selected-tests file contains an empty test name")
    if len(names) != len(set(names)):
        raise SelectionExecutionError("selected-tests file contains duplicate names")
    return names


def decode_selection_receipt(
    encoded: str, expected_sha256: str
) -> tuple[list[str], bytes, list[str], bytes, dict[str, Any]]:
    """Authenticate and parse Shipyard's exact-identity selection receipt."""

    if not re.fullmatch(r"[0-9a-f]{64}", expected_sha256):
        raise SelectionExecutionError("selected-tests SHA-256 is malformed")
    if not encoded or not re.fullmatch(r"[A-Za-z0-9_-]+", encoded):
        raise SelectionExecutionError("selected-tests payload is not canonical URL-safe base64")
    try:
        padding = "=" * (-len(encoded) % 4)
        payload = base64.b64decode(encoded + padding, altchars=b"-_", validate=True)
    except (ValueError, binascii.Error) as error:
        raise SelectionExecutionError("selected-tests payload is malformed") from error
    canonical = base64.urlsafe_b64encode(payload).decode("ascii").rstrip("=")
    if canonical != encoded:
        raise SelectionExecutionError("selected-tests payload is not canonically encoded")
    if len(payload) > MAX_SELECTED_TEST_BYTES:
        raise SelectionExecutionError("selected-tests payload exceeds the safe execution limit")
    if hashlib.sha256(payload).hexdigest() != expected_sha256:
        raise SelectionExecutionError("selected-tests payload digest mismatch")
    try:
        receipt = json.loads(payload)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise SelectionExecutionError("selection receipt is not valid UTF-8 JSON") from error
    identity = {
        "schema_version",
        "repository",
        "pull_request",
        "target",
        "base_sha",
        "head_sha",
        "tree_sha",
        "policy_digest",
        "selection_receipt_digest",
        "validation_contract_digest",
        "workflow_digest",
    }
    full = isinstance(receipt, dict) and "disposition" in receipt
    if full:
        # A full plan selects nothing: the runner runs the configured stages
        # and derives the executable-keyed selection beside them.
        if set(receipt) != identity | {"disposition", "executable_reuse"} \
                or receipt["disposition"] != "full" or receipt["schema_version"] != 2:
            raise SelectionExecutionError("full selection receipt has an unexpected schema")
        validate_executable_reuse_binding(receipt["executable_reuse"])
    required = identity | {"selected_tests_digest", "selected_tests"}
    if isinstance(receipt, dict) and receipt.get("schema_version") == 2:
        required.update({"selected_build_targets_digest", "selected_build_targets"})
    if not full and (not isinstance(receipt, dict) or set(receipt) - {"executable_reuse"} != required):
        raise SelectionExecutionError("selection receipt has an unexpected schema")
    if not full and "executable_reuse" in receipt:
        validate_executable_reuse_binding(receipt["executable_reuse"])
    if receipt["schema_version"] not in (1, 2):
        raise SelectionExecutionError("selection receipt schema version is unsupported")
    if not isinstance(receipt["pull_request"], int) or receipt["pull_request"] <= 0:
        raise SelectionExecutionError("selection receipt PR identity is invalid")
    for key in ("repository", "target"):
        if not isinstance(receipt[key], str) or not receipt[key]:
            raise SelectionExecutionError(f"selection receipt {key} is invalid")
    for key in ("base_sha", "head_sha", "tree_sha"):
        if not isinstance(receipt[key], str) or not re.fullmatch(
            r"[0-9a-fA-F]{40}", receipt[key]
        ):
            raise SelectionExecutionError(f"selection receipt {key} is invalid")
    for key in (
        "policy_digest",
        "selection_receipt_digest",
        "validation_contract_digest",
        "workflow_digest",
        *(() if full else ("selected_tests_digest",)),
    ):
        if not isinstance(receipt[key], str) or not re.fullmatch(
            r"[0-9a-f]{64}", receipt[key]
        ):
            raise SelectionExecutionError(f"selection receipt {key} is invalid")
    if full:
        return [], b"", [], b"", receipt
    selected_tests = receipt["selected_tests"]
    if not isinstance(selected_tests, list) or not all(
        isinstance(name, str) for name in selected_tests
    ):
        raise SelectionExecutionError("selection receipt selected_tests is invalid")
    literal_payload = "".join(f"{name}\n" for name in selected_tests).encode("utf-8")
    names = parse_literal_selection(literal_payload)
    if hashlib.sha256(literal_payload).hexdigest() != receipt["selected_tests_digest"]:
        raise SelectionExecutionError("selection receipt literal-test digest mismatch")
    build_targets: list[str] = []
    build_target_payload = b""
    if receipt["schema_version"] == 2:
        build_targets_value = receipt["selected_build_targets"]
        if not isinstance(build_targets_value, list) or not all(
            isinstance(target, str) for target in build_targets_value
        ):
            raise SelectionExecutionError("selection receipt build targets are invalid")
        build_target_payload = "".join(
            f"{target}\n" for target in build_targets_value
        ).encode("utf-8")
        build_targets = parse_literal_selection(build_target_payload)
        if not build_targets or any(
            target.startswith("-")
            or re.fullmatch(r"[A-Za-z0-9_.:+-]+", target) is None
            for target in build_targets
        ):
            raise SelectionExecutionError("selection receipt build target is not canonical")
        digest = receipt["selected_build_targets_digest"]
        if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
            raise SelectionExecutionError("selection receipt build-target digest is invalid")
        if hashlib.sha256(build_target_payload).hexdigest() != digest:
            raise SelectionExecutionError("selection receipt build-target digest mismatch")
    return names, literal_payload, build_targets, build_target_payload, receipt


def git_value(*args: str) -> str:
    try:
        result = subprocess.run(
            ["git", "-C", str(REPO_ROOT), *args],
            check=True,
            capture_output=True,
            text=True,
            shell=False,
        )
    except (OSError, subprocess.CalledProcessError) as error:
        raise SelectionExecutionError(f"cannot verify checkout identity: {error}") from error
    return result.stdout.strip()


def validate_receipt_identity(receipt: dict[str, Any], target: str) -> None:
    """Bind the receipt to the clean Pulp checkout consumed by this adapter."""

    if receipt["repository"] != "Generous-Corp/pulp" or receipt["target"] != target:
        raise SelectionExecutionError("selection receipt repository or target mismatch")
    current_head = git_value("rev-parse", "HEAD")
    current_tree = git_value("rev-parse", "HEAD^{tree}")
    if current_head != receipt["head_sha"] or current_tree != receipt["tree_sha"]:
        raise SelectionExecutionError("selection receipt does not match checkout HEAD and tree")
    if git_value("status", "--porcelain", "--untracked-files=all"):
        raise SelectionExecutionError("selection execution checkout is dirty")


def require_ctest_version() -> tuple[int, int]:
    """Require the literal-file selector introduced by CTest 3.29."""

    try:
        result = subprocess.run(
            ["ctest", "--version"],
            check=True,
            capture_output=True,
            text=True,
            shell=False,
        )
    except (OSError, subprocess.CalledProcessError) as error:
        raise SelectionExecutionError(f"cannot determine CTest version: {error}") from error
    match = re.search(r"ctest version (\d+)\.(\d+)", result.stdout)
    if match is None:
        raise SelectionExecutionError("cannot parse CTest version")
    version = (int(match.group(1)), int(match.group(2)))
    if version < MINIMUM_CTEST_VERSION:
        raise SelectionExecutionError(
            "authoritative literal selection requires CTest 3.29 or newer"
        )
    return version


def write_private_selection(directory: Path, payload: bytes) -> Path:
    """Materialize one owner-private, read-only snapshot for this process."""

    snapshot = directory / "selected-tests.txt"
    descriptor = os.open(snapshot, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(descriptor, "wb") as selection_file:
            selection_file.write(payload)
            selection_file.flush()
            os.fsync(selection_file.fileno())
    except BaseException:
        snapshot.unlink(missing_ok=True)
        raise
    snapshot.chmod(0o400)
    return snapshot


def load_policy(config_path: Path, target: str) -> dict[str, Any]:
    with config_path.open("rb") as config_file:
        config = tomllib.load(config_file)
    try:
        policy = config["targets"][target]["changed_surface_selection"]
    except (KeyError, TypeError) as error:
        raise SelectionExecutionError(
            f"config has no changed-surface policy for target {target!r}"
        ) from error
    if not isinstance(policy, dict):
        raise SelectionExecutionError("changed-surface policy is not a table")
    return merge_families_file(policy, config_path.resolve().parent.parent)


def merge_families_file(policy: dict[str, Any], repo_root: Path) -> dict[str, Any]:
    """Apply a `families_file` the way Shipyard's planner does: its
    `[[families]]` tables follow the inline families, and its path joins
    `policy_paths`. The file must be a relative `.toml` path under `.shipyard/`
    and hold only `families`."""

    policy = dict(policy)
    path = policy.pop("families_file", None)
    if path is None:
        return policy
    parts = str(path).split("/")
    if (not isinstance(path, str) or not path.startswith(".shipyard/")
            or not path.endswith(".toml") or any(p in {"", ".", ".."} for p in parts)):
        raise SelectionExecutionError(f"families_file {path!r} must be a .toml path under .shipyard/")
    try:
        with (repo_root / path).open("rb") as handle:
            extra = tomllib.load(handle)
    except (OSError, tomllib.TOMLDecodeError) as error:
        raise SelectionExecutionError(f"families_file {path!r} is unreadable: {error}") from error
    if set(extra) != {"families"} or not isinstance(extra["families"], list) or not extra["families"]:
        raise SelectionExecutionError(f"families_file {path!r} must hold only a nonempty [[families]] list")
    policy["families"] = [*policy.get("families", []), *extra["families"]]
    paths = list(policy.get("policy_paths", []))
    if path not in paths:
        paths.append(path)
    policy["policy_paths"] = paths
    return policy


def declared_literal_tests(policy: dict[str, Any]) -> set[str]:
    names = set(policy.get("baseline_tests", []))
    for family in policy.get("families", []):
        names.update(family.get("tests", []))
        names.update(family.get("extended_tests", []))
    if not names or not all(isinstance(name, str) and name for name in names):
        raise SelectionExecutionError("policy has no valid literal test inventory")
    return names


def validate_build_configuration(build_dir: Path, policy: dict[str, Any]) -> None:
    """Prove the live CMake cache matches every selector build declaration."""

    cache_path = build_dir / "CMakeCache.txt"
    try:
        lines = cache_path.read_text(encoding="utf-8", errors="strict").splitlines()
    except OSError as error:
        raise SelectionExecutionError(f"cannot read CMake cache: {error}") from error
    observed: dict[str, str] = {}
    for line in lines:
        if line.startswith(("//", "#")) or ":" not in line or "=" not in line:
            continue
        key = line.split(":", 1)[0]
        observed[key] = line.split("=", 1)[1]

    build_types = {
        "debug": "Debug",
        "release": "Release",
        "rel_with_deb_info": "RelWithDebInfo",
        "min_size_rel": "MinSizeRel",
    }
    declared_type = policy.get("build_type")
    if declared_type not in build_types:
        raise SelectionExecutionError("policy build_type is missing or unsupported")
    expected = {"CMAKE_BUILD_TYPE": build_types[declared_type]}
    flags = policy.get("build_flags")
    if not isinstance(flags, list):
        raise SelectionExecutionError("policy build_flags is not an array")
    for flag in flags:
        if not isinstance(flag, str) or not flag.startswith("-D") or "=" not in flag[2:]:
            raise SelectionExecutionError(
                f"policy build flag is not an exact -DKEY=VALUE declaration: {flag!r}"
            )
        key, value = flag[2:].split("=", 1)
        if not key or (key in expected and expected[key] != value):
            raise SelectionExecutionError(f"conflicting policy build flag: {flag!r}")
        expected[key] = value

    mismatches = [
        f"{key}: expected {value!r}, observed {observed.get(key)!r}"
        for key, value in sorted(expected.items())
        if observed.get(key) != value
    ]
    if mismatches:
        raise SelectionExecutionError(
            "live CMake configuration differs from selector policy: " + "; ".join(mismatches)
        )


# The inputs Shipyard binds for the executable-keyed selection before any
# stage runs. The runner derives the selection from them after configure and
# echoes them verbatim; Shipyard re-derives from the files it copies out and
# refuses a difference.
EXECUTABLE_REUSE_BINDING = {
    "candidates": list,
    "rules_digest": str,
    "derivation_code_dir": str,
    "derivation_code_sha256": str,
    "sample_seed": str,
    "sample_percent": int,
    "build_dir": str,
}
# Base-record candidates, newest first. Shipyard filters them by platform and
# merge rules; the toolchain rule needs this head's configure, so the runner
# applies it and uses the first candidate built by the lane's toolchain.
EXECUTABLE_REUSE_CANDIDATE = {"run_id": str, "record_sha256": str, "record_path": str, "commit": str}
MAX_REUSE_CANDIDATES = 8
# Run from Shipyard's extracted base copies, never from the checkout.
DERIVATION_SCRIPTS = {
    "codemodel": "tools/ci/codemodel_digest.py",
    "keys": "tools/ci/executable_keys.py",
    "selection": "tools/ci/executable_selection.py",
}


def validate_executable_reuse_binding(binding: Any) -> None:
    if not isinstance(binding, dict) or set(binding) != set(EXECUTABLE_REUSE_BINDING):
        raise SelectionExecutionError("selection receipt executable_reuse has an unexpected schema")
    for key, kind in EXECUTABLE_REUSE_BINDING.items():
        value = binding[key]
        if not isinstance(value, kind) or isinstance(value, bool) or value == "":
            raise SelectionExecutionError(f"selection receipt executable_reuse.{key} is invalid")
    for key in ("rules_digest", "derivation_code_sha256"):
        if not re.fullmatch(r"[0-9a-f]{64}", binding[key]):
            raise SelectionExecutionError(f"selection receipt executable_reuse.{key} is invalid")
    candidates = binding["candidates"]
    if len(candidates) > MAX_REUSE_CANDIDATES:
        raise SelectionExecutionError("selection receipt executable_reuse.candidates is too long")
    for candidate in candidates:
        if not isinstance(candidate, dict) or set(candidate) != set(EXECUTABLE_REUSE_CANDIDATE) or any(
                not isinstance(candidate[k], t) or not candidate[k] for k, t in EXECUTABLE_REUSE_CANDIDATE.items()) \
                or not re.fullmatch(r"[0-9a-f]{64}", candidate["record_sha256"]) \
                or not re.fullmatch(r"[0-9a-fA-F]{40}", candidate["commit"]):
            raise SelectionExecutionError("selection receipt executable_reuse.candidates is invalid")
    if not 1 <= binding["sample_percent"] <= 100:
        raise SelectionExecutionError("selection receipt executable_reuse.sample_percent is invalid")


class DerivationError(Exception):
    pass


def _sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def derive_executable_reuse(
    binding: dict[str, Any], head_sha: str, build_dir: Path, result_dir: Path, runner=subprocess.run,
    *, base_sha: str = "",
) -> dict[str, Any]:
    """Derive the executable-keyed selection for the configured head and
    record it beside the result receipt. Shadow only: the stages run
    unchanged, so a failure here is recorded as the status and never raised.
    """
    try:
        return {"status": "derived", **_derive(binding, head_sha, base_sha, build_dir, result_dir, runner)}
    except DerivationError as error:
        return {"status": f"error: {error}"}
    except (OSError, ValueError, subprocess.SubprocessError, SelectionExecutionError) as error:
        return {"status": f"error: {type(error).__name__}: {error}"}


# Runs from the derivation dir with the base's key code: the lane's toolchain
# for this configured build, and the first candidate record built by it.
PICK_SCRIPT = """
import inspect, json, sys
from pathlib import Path
sys.path.insert(0, 'tools/ci')
import executable_keys as k
probe = k.probe_toolchain
lane = probe(Path(sys.argv[1])) if inspect.signature(probe).parameters else probe()
pick = None
for index, path in enumerate(json.loads(sys.argv[2])):
    record, digest = k.load_record(Path(path))
    if lane is not None and record is not None and record.get('toolchain') == lane:
        pick = {'index': index, 'digest': digest}
        break
print(json.dumps({'toolchain': lane, 'pick': pick}, sort_keys=True))
"""


def _derive(binding, head_sha, base_sha, build_dir, result_dir, runner) -> dict[str, Any]:
    if Path(binding["build_dir"]).resolve() != build_dir.resolve():
        raise DerivationError("the bound build directory is not this lane's")
    code = Path(binding["derivation_code_dir"])
    scripts = {k: code / v for k, v in DERIVATION_SCRIPTS.items()}
    absent = [str(p) for p in scripts.values() if not p.is_file()]
    if absent:
        raise DerivationError(f"derivation code is missing {absent[0]}")
    # A resumed or prepared build directory may hold a reply from another
    # configure: drop it and reconfigure, so the codemodel read is this tree's.
    api = build_dir / ".cmake" / "api" / "v1"
    shutil.rmtree(api / "reply", ignore_errors=True)
    (api / "query").mkdir(parents=True, exist_ok=True)
    (api / "query" / "codemodel-v2").touch()
    reconfigure = runner([str(REPO_ROOT / "tools" / "ci" / "governed-build.sh"), "cmake", str(build_dir)],
                         capture_output=True, text=True, shell=False)
    if reconfigure.returncode != 0:
        raise DerivationError(f"reconfigure exited {reconfigure.returncode}")
    if not (api / "reply").is_dir():
        raise DerivationError("reconfigure wrote no codemodel reply")
    result_dir.mkdir(parents=True, exist_ok=True)
    files = {name: result_dir / name for name in (
        "ctest-listing.json", "toolchain.json", "codemodel-digest.json", "executable-keys.json",
        "selection.json")}
    listing = runner(["ctest", "--test-dir", str(build_dir), "--show-only=json-v1"],
                     capture_output=True, text=True, shell=False)
    if listing.returncode != 0:
        raise DerivationError(f"ctest listing exited {listing.returncode}")
    files["ctest-listing.json"].write_text(listing.stdout, encoding="utf-8")
    python = [sys.executable, "-I"]

    def step(argv: list[str], what: str) -> str:
        done = runner(python + argv, capture_output=True, text=True, shell=False, cwd=str(code),
                      env={**os.environ, **STAGE_ENV})
        if done.returncode != 0:
            raise DerivationError(f"{what} exited {done.returncode}: {done.stderr.strip()[-300:]}")
        return done.stdout

    # The key code's own probe reads the compiler this build directory
    # recorded; an older base copy takes no argument.
    candidates = binding["candidates"]
    picked = json.loads(step(["-c", PICK_SCRIPT, binding["build_dir"],
                              json.dumps([c["record_path"] for c in candidates])], "toolchain pick"))
    files["toolchain.json"].write_text(json.dumps(picked["toolchain"], sort_keys=True) + "\n", encoding="utf-8")
    pick = candidates[picked["pick"]["index"]] if picked["pick"] is not None else None
    if pick is not None and picked["pick"]["digest"] != pick["record_sha256"]:
        raise DerivationError(f"candidate record {pick['run_id']} is not the one Shipyard bound")
    # With no candidate on this toolchain the first one still keys, so every
    # executable carries base_other_toolchain and the reason names a record.
    used = pick or (candidates[0] if candidates else None)
    bound_build = binding["build_dir"]
    step([str(scripts["codemodel"]), "--build-dir", bound_build, "--source-root", str(REPO_ROOT),
          "--ctest-json", str(files["ctest-listing.json"]), "--out", str(files["codemodel-digest.json"])],
         "codemodel digest")
    if used is None and not base_sha:
        raise DerivationError("no base commit to key against")
    step([str(scripts["keys"]), "--source-root", str(REPO_ROOT),
          "--base-sha", used["commit"] if used else base_sha, "--head-sha", head_sha,
          *(["--base-record", used["record_path"], "--base-record-run-id", used["run_id"]] if used else []),
          "--head-codemodel", str(files["codemodel-digest.json"]),
          "--ctest-json", str(files["ctest-listing.json"]), "--build-dir", bound_build,
          "--toolchain-json", str(files["toolchain.json"]), "--out", str(files["executable-keys.json"])],
         "key manifest")
    manifest = json.loads(files["executable-keys.json"].read_text(encoding="utf-8"))
    if (manifest.get("producer") or {}).get("base_record_sha256") != (used or {}).get("record_sha256"):
        raise DerivationError("the base record is not the one Shipyard bound")
    step([str(scripts["selection"]), "--manifest", str(files["executable-keys.json"]),
          "--head-codemodel", str(files["codemodel-digest.json"]),
          "--ctest-json", str(files["ctest-listing.json"]), "--seed", binding["sample_seed"],
          "--percent", str(binding["sample_percent"]), "--out", str(files["selection.json"])],
         "selection")
    selection = json.loads(files["selection.json"].read_text(encoding="utf-8"))
    return {
        # The candidate the lane's toolchain picked, or null when none matched.
        "base_record_run_id": pick["run_id"] if pick else None,
        "base_record_sha256": pick["record_sha256"] if pick else None,
        "cmake_cache_sha256": _sha256_file(build_dir / "CMakeCache.txt"),
        "ctest_listing_sha256": _sha256_file(files["ctest-listing.json"]),
        "toolchain_sha256": _sha256_file(files["toolchain.json"]),
        "codemodel_digest_sha256": _sha256_file(files["codemodel-digest.json"]),
        "key_manifest_sha256": _sha256_file(files["executable-keys.json"]),
        "selection_sha256": _sha256_file(files["selection.json"]),
        "would_skip_count": len(selection["would_skip"]),
        "sampled_count": len(selection["sampled_executables"]),
        "reasons": manifest.get("reasons"),
    }


def ctest_json(build_dir: Path, selected_file: Path | None = None) -> list[dict[str, Any]]:
    return ctest_payload(build_dir, selected_file)["tests"]


def ctest_payload(build_dir: Path, selected_file: Path | None = None) -> dict[str, Any]:
    command = ["ctest", "--test-dir", str(build_dir), "--show-only=json-v1"]
    if selected_file is not None:
        command.extend(["--tests-from-file", str(selected_file)])
    try:
        result = subprocess.run(
            command,
            check=True,
            capture_output=True,
            text=True,
            shell=False,
        )
        payload = json.loads(result.stdout)
    except (OSError, subprocess.CalledProcessError, json.JSONDecodeError) as error:
        raise SelectionExecutionError(f"CTest inventory query failed: {error}") from error
    tests = payload.get("tests")
    if not isinstance(tests, list):
        raise SelectionExecutionError("CTest JSON has no tests array")
    return payload


def _cache_entries(build_dir: Path) -> dict[str, str]:
    observed: dict[str, str] = {}
    cache = build_dir / "CMakeCache.txt"
    if cache.is_file():
        for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith(("//", "#")) or ":" not in line or "=" not in line:
                continue
            observed[line.split(":", 1)[0]] = line.split("=", 1)[1]
    return observed


# Cache entries a checkout's environment decides rather than its sources:
# which SDKs setup.sh linked and whether Skia and WebGPU resolved (PULP_HAS_*),
# the dependency pins it linked them at, and the generator, Python and build
# type. A base configured under a different environment registers different
# tests for reasons no plan can see, so these must agree before any
# registration is compared.
PROVISIONING_SWITCH = re.compile(r"PULP_HAS_[A-Z0-9_]+")
ENVIRONMENT_ENTRIES = ("PULP_CHECKOUT_DEPENDENCY_CONTRACT", "CMAKE_GENERATOR",
                       "Python3_EXECUTABLE", "CMAKE_BUILD_TYPE")


def provisioning(cache_entries: dict[str, str]) -> dict[str, str]:
    return {k: v for k, v in sorted(cache_entries.items())
            if PROVISIONING_SWITCH.fullmatch(k) or k in ENVIRONMENT_ENTRIES}


def linked_externals(tree: Path) -> list[str]:
    """The external/ entries setup.sh linked into the shared source cache."""
    external = tree / "external"
    return sorted(entry.name for entry in external.iterdir()
                  if entry.is_symlink()) if external.is_dir() else []


def validate_provisioning(base: dict[str, Any], build_dir: Path) -> None:
    """Refuse, by name, a base whose provisioning differs from this tree's."""

    head = provisioning(_cache_entries(build_dir))
    recorded = base.get("provisioning")
    if not isinstance(recorded, dict):
        raise SelectionExecutionError("inventory: base_provisioning_mismatch: base provisioning not recorded")
    differing = [f"{name} base={recorded.get(name, '<unset>')} head={head.get(name, '<unset>')}"
                 for name in sorted(set(recorded) | set(head)) if recorded.get(name) != head.get(name)]
    if differing:
        raise SelectionExecutionError(
            "inventory: base_provisioning_mismatch: " + "; ".join(differing))


def base_projection(
    base_sha: str,
    policy: dict[str, Any],
    build_dir: Path,
    repo_root: Path = REPO_ROOT,
    runner: Any = subprocess.run,
) -> dict[str, Any]:
    """The protected base's ctest registrations, in this tree's configuration.

    Configures the exact base commit in a scratch worktree with the policy's
    build flags and this build's generator and Python, then projects its
    listing. A configure lists registrations without building anything, which
    is all a registration-level comparison needs. Any failure raises; the
    caller falls back to the full suite ("inventory: base not recorded")."""

    if not re.fullmatch(r"[0-9a-f]{40}", base_sha or ""):
        raise SelectionExecutionError(f"inventory: base not recorded: invalid base {base_sha!r}")
    flags = [str(flag) for flag in policy.get("build_flags", [])]
    cache_entries = _cache_entries(build_dir)
    # A universal macOS build lipos a second WebGPU slice that
    # tools/cmake/PulpWgpuUniversal.cmake downloads with file(DOWNLOAD), which
    # FETCHCONTENT_FULLY_DISCONNECTED does not govern. Refuse rather than let a
    # base configure reach the network.
    architectures = [
        flag.split("=", 1)[1] for flag in flags if flag.startswith("-DCMAKE_OSX_ARCHITECTURES=")
    ] + [cache_entries.get("CMAKE_OSX_ARCHITECTURES", "")]
    if any(len([arch for arch in value.split(";") if arch.strip()]) > 1 for value in architectures):
        raise SelectionExecutionError(
            "inventory: base not recorded: a universal build cannot be configured disconnected"
        )
    # The plan verified this base as the PR's merge base on the protected ref;
    # a checkout whose merge base is anything else is not the tree it planned.
    merge_base = runner(["git", "-C", str(repo_root), "merge-base", "HEAD", base_sha],
                        capture_output=True, text=True, shell=False)
    if merge_base.returncode != 0 or (merge_base.stdout or "").strip() != base_sha:
        raise SelectionExecutionError(
            f"inventory: base mismatch: {base_sha} is not this checkout's merge base"
        )
    generator = cache_entries.get("CMAKE_GENERATOR", "")
    python = cache_entries.get("Python3_EXECUTABLE", "")
    head_provisioning = [f"{k}={v}" for k, v in provisioning(cache_entries).items()]
    key = hashlib.sha256(
        "\0".join([base_sha, "shape=configure", generator, python, *flags,
                   *head_provisioning]).encode()
    ).hexdigest()[:16]
    cache_dir = build_dir / BASE_INVENTORY_CACHE
    cached = cache_dir / f"{base_sha}-{key}.json"
    if cached.is_file():
        reused = json.loads(cached.read_text(encoding="utf-8"))
        reused["configure_seconds"] = 0.0
        validate_provisioning(reused, build_dir)
        return reused
    started = time.monotonic()
    cache_dir.mkdir(parents=True, exist_ok=True)
    tree = Path(tempfile.mkdtemp(prefix=f"base-{base_sha[:12]}-", dir=cache_dir))
    try:
        steps = [
            ["git", "-C", str(repo_root), "worktree", "add", "--detach", "--force", str(tree), base_sha],
            # Provision the base as the head checkout was: setup.sh links the
            # external SDKs from the shared source cache. Git may only read
            # local objects here, so a pin missing from the cache fails this
            # step (the plan selects full) instead of cloning mid-plan.
            ["bash", str(tree / "setup.sh"), "--deps-only", "--non-interactive"],
            # Through the host build governor, like every other build-tree
            # command the lane runs.
            # FetchContent is disconnected: dependencies, including the
            # prebuilt WebGPU runtime archive, resolve from the machine's
            # shared source cache, and a miss fails this configure (so the
            # plan selects full) instead of downloading mid-plan.
            ["bash", str(repo_root / "tools" / "ci" / "governed-build.sh"),
             "cmake", "-S", str(tree), "-B", str(tree / "build"),
             *(["-G", generator] if generator else []), *flags,
             *([f"-DPython3_EXECUTABLE={python}"] if python else []),
             "-DFETCHCONTENT_FULLY_DISCONNECTED=ON"],
        ]
        offline = {**os.environ, "GIT_ALLOW_PROTOCOL": "file"}
        for step in steps:
            result = runner(step, capture_output=True, text=True, shell=False, env=offline)
            if result.returncode != 0:
                tail = (result.stderr or result.stdout or "").strip().splitlines()[-1:]
                raise SelectionExecutionError(
                    f"inventory: base not recorded: {' '.join(step[:4])} failed {tail}"
                )
        try:
            projected = inventory.project_registrations(
                ctest_payload(tree / "build"), tree, tree / "build", shape="configure"
            )
            projected["provisioning"] = provisioning(_cache_entries(tree / "build"))
            projected["linked_externals"] = linked_externals(tree)
        except SelectionExecutionError as error:
            raise SelectionExecutionError(f"inventory: base not recorded: {error}") from error
    finally:
        runner(["git", "-C", str(repo_root), "worktree", "remove", "--force", str(tree)],
               capture_output=True, text=True, shell=False)
        shutil.rmtree(tree, ignore_errors=True)
    projected["base_sha"] = base_sha
    projected["configure"] = {"generator": generator, "python": python, "flags": flags}
    cached.write_text(json.dumps(projected, sort_keys=True), encoding="utf-8")
    projected["configure_seconds"] = round(time.monotonic() - started, 1)
    validate_provisioning(projected, build_dir)
    print(f"changed-surface: base inventory for {base_sha[:12]} configured in "
          f"{projected['configure_seconds']}s ({projected['row_count']} rows)", file=sys.stderr)
    return projected


PROTECTED_BASE = "the protected base"
PREBUILD_SNAPSHOT = "this tree before the build"


def validate_registrations_match_base(
    full_payload: dict[str, Any], source_root: Path, build_dir: Path, base: dict[str, Any],
    require_built: bool = True, reference_label: str = PROTECTED_BASE,
) -> None:
    """This tree's registrations must equal the reference's: the base's, or
    after the full build this tree's own pre-build snapshot. A bounded plan
    never carries a registration change: CMake and test/cmake edits are
    test-topology paths, which select the full suite, so any difference here
    is drift the plan cannot account for.

    Both sides are compared in configure shape, so a cold, partly built and
    fully built tree compare alike; only `require_built` (after the full
    build) refuses a registration still without a command. Against the
    pre-build snapshot the comparison catches what a build can change: a
    CMake re-run mid-build that registers tests the earlier check never saw."""

    live = inventory.project_registrations(full_payload, source_root, build_dir, shape="configure")
    # A registration the authoritative filter excludes is never selected or
    # run by a bounded plan; a validator the host lacks lists no command.
    excluded = {test.get("name") for test in full_payload.get("tests", [])
                if inventory.excluded_from_inventory(test)}
    unresolved = [name for name in live["incomplete"]
                  if not inventory.NOT_BUILT_PLACEHOLDER.match(name) and name not in excluded]
    if require_built and unresolved:
        raise SelectionExecutionError(
            "ctest registrations have no command after the build; require full suite: "
            + ", ".join(unresolved[:12])
        )
    differences = inventory.projection_differences(base, live)
    if differences:
        raise SelectionExecutionError(
            f"ctest registrations differ from {reference_label}; require full suite: "
            + "; ".join(differences)
        )


def validate_selection(
    *,
    selected_names: Sequence[str],
    full_payload: dict[str, Any],
    selected_tests: list[dict[str, Any]],
    source_root: Path,
    build_dir: Path,
    policy: dict[str, Any],
    base: dict[str, Any],
    target: str,
    require_built: bool = True,
    reference_label: str = PROTECTED_BASE,
) -> None:
    """Fail closed unless CTest's file selection equals the reviewed expansion."""

    baseline = policy.get("baseline_tests")
    if not isinstance(baseline, list) or not set(baseline).issubset(selected_names):
        raise SelectionExecutionError("selected-tests file omits the mandatory baseline")
    undeclared = sorted(set(selected_names) - declared_literal_tests(policy))
    if undeclared:
        raise SelectionExecutionError(f"selection contains undeclared names: {undeclared}")

    full_tests = full_payload["tests"]
    validate_registrations_match_base(full_payload, source_root, build_dir, base, require_built,
                                      reference_label)
    live_manifest = inventory.build_manifest(
        full_tests,
        source_root,
        build_dir,
        policy,
        target=target,
    )
    inventory.require_unambiguous(live_manifest)
    expected_groups = inventory.expand_literal_selection(live_manifest, selected_names)
    observed_groups = inventory.inventory_groups(selected_tests, source_root, build_dir)
    if inventory.canonical_json(expected_groups) != inventory.canonical_json(observed_groups):
        raise SelectionExecutionError(
            "CTest --tests-from-file expansion differs from the reviewed literal selection"
        )


def validate_deferred_shadow_selection(
    *,
    selected_names: Sequence[str],
    full_tests: list[dict[str, Any]],
    selected_tests: list[dict[str, Any]],
    source_root: Path,
    build_dir: Path,
    policy: dict[str, Any],
) -> int:
    """Validate the runnable subset while exact inventory awaits a full build.

    This path is shadow-only.  The authoritative full build must replace every
    provenance-backed Catch2 placeholder, after which ``validate_selection``
    performs the ordinary comparison with the base before full tests run.
    """

    baseline = policy.get("baseline_tests")
    if not isinstance(baseline, list) or not set(baseline).issubset(selected_names):
        raise SelectionExecutionError("selected-tests file omits the mandatory baseline")
    undeclared = sorted(set(selected_names) - declared_literal_tests(policy))
    if undeclared:
        raise SelectionExecutionError(f"selection contains undeclared names: {undeclared}")
    ready_tests, placeholders = inventory.split_proven_unbuilt_placeholders(
        full_tests, build_dir
    )
    if not placeholders:
        raise SelectionExecutionError("deferred inventory validation has no placeholders")
    selected_groups = inventory.inventory_groups(selected_tests, source_root, build_dir)
    ready_groups = inventory.inventory_groups(ready_tests, source_root, build_dir)
    selected_counts = {
        group["fingerprint"]: group["multiplicity"] for group in selected_groups
    }
    ready_counts = {group["fingerprint"]: group["multiplicity"] for group in ready_groups}
    if any(
        count > ready_counts.get(fingerprint, 0)
        for fingerprint, count in selected_counts.items()
    ):
        raise SelectionExecutionError(
            "CTest --tests-from-file expansion is not a subset of runnable registrations"
        )
    observed_names = {group["composite"]["name"] for group in selected_groups}
    if observed_names != set(selected_names):
        raise SelectionExecutionError(
            "CTest --tests-from-file expansion differs from the reviewed literal selection"
        )
    return len(placeholders)


def validate_after_selected_build(
    *,
    selected_names: Sequence[str],
    full_payload: dict[str, Any],
    selected_tests: list[dict[str, Any]],
    source_root: Path,
    build_dir: Path,
    policy: dict[str, Any],
    base: dict[str, Any],
    target: str,
    selected_build_targets: Sequence[str],
) -> None:
    """Validate the refreshed inventory at its strongest available level."""

    full_tests = full_payload["tests"]
    _, placeholders = inventory.split_proven_unbuilt_placeholders(
        full_tests, build_dir
    )
    if placeholders:
        validate_deferred_shadow_selection(
            selected_names=selected_names,
            full_tests=full_tests,
            selected_tests=selected_tests,
            source_root=source_root,
            build_dir=build_dir,
            policy=policy,
        )
    else:
        validate_selection(
            selected_names=selected_names,
            full_payload=full_payload,
            selected_tests=selected_tests,
            source_root=source_root,
            build_dir=build_dir,
            policy=policy,
            base=base,
            target=target,
            # Only the selected targets are built at this point.
            require_built=False,
        )
    validate_build_target_projection(
        build_dir=build_dir,
        selected_tests=selected_tests,
        selected_build_targets=selected_build_targets,
    )


def cmake_artifact_targets(build_dir: Path) -> tuple[set[str], dict[Path, str]]:
    """Read the configure-produced CMake File API target/artifact projection."""

    try:
        model = inventory.load_codemodel_targets(build_dir)
    except inventory.InventoryError as error:
        raise SelectionExecutionError(str(error)) from error
    target_names: set[str] = set()
    artifacts: dict[Path, str] = {}
    for name, target in model.targets.items():
        target_names.add(name)
        for path in target.artifacts:
            resolved = Path(path).resolve()
            prior = artifacts.get(resolved)
            if prior is not None and prior != name:
                raise SelectionExecutionError(
                    f"CMake artifact {resolved} is produced by multiple targets"
                )
            artifacts[resolved] = name
    return target_names, artifacts


def validate_build_target_projection(
    *,
    build_dir: Path,
    selected_tests: list[dict[str, Any]],
    selected_build_targets: Sequence[str],
) -> None:
    """Prove selected native test commands are materialized by selected targets."""

    if not selected_build_targets:
        raise SelectionExecutionError("selected build execution has no producer targets")
    target_names, artifacts = cmake_artifact_targets(build_dir)
    missing_targets = sorted(set(selected_build_targets) - target_names)
    if missing_targets:
        raise SelectionExecutionError(
            f"selected CMake targets are absent from the codemodel: {missing_targets}"
        )
    selected = set(selected_build_targets)
    build_root = build_dir.resolve()
    for test in selected_tests:
        command = test.get("command")
        if not isinstance(command, list) or not command or not isinstance(command[0], str):
            raise SelectionExecutionError("selected CTest command is malformed")
        executable = Path(command[0])
        if not executable.is_absolute():
            continue
        resolved = executable.resolve()
        if not resolved.is_relative_to(build_root):
            continue
        producer = artifacts.get(resolved)
        if producer is None:
            raise SelectionExecutionError(
                f"selected native CTest executable has no CMake producer: {resolved}"
            )
        if producer not in selected:
            raise SelectionExecutionError(
                f"selected native CTest executable requires undeclared target {producer}"
            )


def execution_argv(
    build_dir: Path, selected_file: Path | None = None, junit: Path | None = None
) -> list[str]:
    """Return the exact shell-free governed CTest argv."""

    command = [
        str(REPO_ROOT / "tools" / "ci" / "governed-build.sh"),
        "ctest",
        "--test-dir",
        str(build_dir),
        "--output-on-failure",
        "--repeat",
        "until-pass:2",
        "--exclude-regex",
        EXCLUDE_NAME,
        "--label-exclude",
        EXCLUDE_LABEL,
        "--no-tests=error",
    ]
    if selected_file is not None:
        command.extend(["--tests-from-file", str(selected_file)])
    if junit is not None:
        command.extend(["--output-junit", str(junit)])
    return command


def build_argv(build_dir: Path, targets: Sequence[str] = ()) -> list[str]:
    command = [
        str(REPO_ROOT / "tools" / "ci" / "governed-build.sh"),
        "cmake",
        "--build",
        str(build_dir),
    ]
    if targets:
        command.extend(["--target", *targets])
    return command


def clear_build_sentinel(build_dir: Path) -> int:
    return subprocess.run(
        [
            str(REPO_ROOT / "tools" / "ci" / "build-dir-sentinel.sh"),
            "clear",
            str(build_dir),
        ],
        shell=False,
    ).returncode


# The macOS lane's configured build and test stages, which a keyed full run
# execs in place of those stages. The test stage's build_dir_lock.py prefix is
# dropped because the runner holds that lock for the whole run, and its
# lane_reuse_record.py wrapper because the runner records the suite itself;
# test_keyed_full_execs_the_configured_stages compares these to the config.
STAGE_ENV = {"PULP_BUILD_CLASS": "background"}
STAGE_TEST_FLAGS = ("--output-on-failure", "--repeat", "until-pass:2", "--exclude-regex",
                    EXCLUDE_NAME, "--label-exclude", EXCLUDE_LABEL)


def stage_test_argv(build_dir: Path, junit: Path | None = None) -> list[str]:
    command = [str(REPO_ROOT / "tools" / "ci" / "governed-build.sh"), "ctest", "--test-dir",
               str(build_dir), *STAGE_TEST_FLAGS]
    return command + (["--output-junit", str(junit)] if junit is not None else [])


def failure_coverage(selected_result: int, full_result: int | None) -> str:
    if full_result is None:
        return "not_compared"
    if selected_result != 0 and full_result != 0:
        return "failure_observed_by_selected"
    if selected_result == 0 and full_result != 0:
        return "missed_full_failure"
    if selected_result != 0 and full_result == 0:
        return "selected_only_failure"
    return "no_failure_observed"


# The protected base's named reds the lane's full suite carries regardless of
# the change. A full-suite failure outside the selection may be one of these and
# nothing else for a matched_fail to count; the file is a policy path, so a PR
# that edits it selects the full suite, and it is read from the base, never the
# head.
LANE_RED_ALLOWLIST = "tools/ci/changed_surface_lane_reds.json"


def _junit_case_failed(case: Any) -> bool:
    """Whether ctest counts this case as failed. A "Not Run" test (a missing
    executable, a failed fixture dependency) is in ctest's FAILED list and exit
    code but its JUnit row is `notrun` with a <skipped> child, the same shape as
    a real skip; only the message tells them apart, and a real skip's message
    (SKIP_RETURN_CODE, SKIP_REGULAR_EXPRESSION) starts with SKIP_. A disabled
    test is `disabled` and not failed."""
    status = case.get("status")
    if status == "fail" or case.find("failure") is not None:
        return True
    if status == "notrun":
        skipped = case.find("skipped")
        return not (skipped is not None and skipped.get("message", "").startswith("SKIP_"))
    return False


def junit_failures(path: Path) -> set[str] | None:
    """Names of the tests a ctest --output-junit report records as failed
    (including Not Run), or None when the report is absent or unreadable."""
    import xml.etree.ElementTree as ElementTree

    try:
        root = ElementTree.parse(path).getroot()
    except (OSError, ElementTree.ParseError):
        return None
    return {case.get("name", "") for case in root.iter("testcase") if _junit_case_failed(case)}


UNMEASURED_SKIPS = {"would_skip_tests": None, "false_skip_count": None, "false_skips": None,
                    "sampled_failures": None}


def false_skips(result_dir: Path, full_junit: Path) -> dict[str, Any] | None:
    """The tests the derived selection would have skipped (registrations of
    would-skip executables it did not sample) and which of them the full run
    failed; and which tests of the sampled would-skips failed, the negative
    control's catch. None when there is no derived selection or no full
    report."""
    try:
        selection = json.loads((result_dir / "selection.json").read_text(encoding="utf-8"))
        manifest = json.loads((result_dir / "executable-keys.json").read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    failed = junit_failures(full_junit)
    if failed is None:
        return None
    executables = manifest.get("executables") or {}
    sampled = set(selection.get("sampled_executables") or [])
    skipped = set(selection.get("would_skip") or []) - sampled

    def registered(artifacts: set[str]) -> set[str]:
        return {name for a in artifacts for name in (executables.get(a) or {}).get("registrations") or []}
    tests = sorted(registered(skipped))
    caught = sorted(set(tests) & failed)
    return {"would_skip_tests": tests, "false_skip_count": len(caught), "false_skips": caught,
            "sampled_failures": sorted(registered(sampled) & failed)}


# Executables whose key ignores what decides their bytes; a hash difference
# there is expected and says nothing about the key. Read from the base's
# extracted copy, so a change cannot list itself to hide a difference.
KEY_BLIND_EXECUTABLES = "tools/ci/key_blind_executables.json"


KEY_BLIND_SCHEMA = "pulp-key-blind/v1"
# The shape a result always carries, so an absent field never reads as "none".
UNKNOWN_UNREACHED = {"unreached_changed": None, "unreached_compared": 0, "unreached_unchecked_modules": None}


def unreached_changed(result_dir: Path, build_dir: Path, binding: dict[str, Any],
                      derived: dict[str, Any], scope: str) -> dict[str, Any]:
    """Would-skip executables whose bytes, or the bytes of anything in their
    spawn closure (the tools they run and the modules they load), differ
    from the picked base record's hash for the same artifact: a key that
    said "unchanged" about something that changed.

    `scope` is "all" when every would-skip was built (a keyed full run) and
    "sampled" when only the sample was. The record hashes the executables
    registered ctests run, which every would-skip is; a closure artifact it
    has no hash for is listed in `unreached_unchecked_modules`, and a would-skip
    without one is skipped. `unreached_changed` is None, never an empty
    list, when nothing can be compared: no record was picked (bytes from
    another toolchain always differ), the record's identity is unusable, or
    no artifact in scope has a hash. `unreached_compared` counts the
    artifacts compared, so "none changed" over one artifact reads as that."""
    unknown = UNKNOWN_UNREACHED
    if derived.get("status") != "derived" or derived.get("base_record_run_id") is None:
        return unknown
    record = next((c for c in binding["candidates"] if c["run_id"] == derived["base_record_run_id"]), None)
    if record is None:
        return unknown
    try:
        selection = json.loads((result_dir / "selection.json").read_text(encoding="utf-8"))
        manifest = json.loads((result_dir / "executable-keys.json").read_text(encoding="utf-8"))
        identity = json.loads((Path(record["record_path"]) / "identity.json").read_text(encoding="utf-8"))
        job = json.loads((Path(record["record_path"]) / "job.json").read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return unknown
    # The recorder writes an empty identity when it could not hash, and says
    # so: as `identity.usable`, or in older records as a problem line.
    usable = job["identity"].get("usable") if isinstance(job.get("identity"), dict) else None
    if usable is False or (usable is None and any(
            "executable identity unavailable" in str(p) for p in job.get("problems") or [])):
        return unknown
    blind_path = Path(binding["derivation_code_dir"]) / KEY_BLIND_EXECUTABLES
    try:
        listing = json.loads(blind_path.read_text(encoding="utf-8"))
        if listing.get("schema") != KEY_BLIND_SCHEMA:
            return unknown
        blind = set(listing.get("executables") or {})
    except FileNotFoundError:
        blind = set()
    except (OSError, json.JSONDecodeError, AttributeError):
        return unknown
    executables = manifest.get("executables") or {}
    roots = set(selection.get("would_skip") or [])
    if scope == "sampled":
        roots &= set(selection.get("sampled_executables") or [])
    recorded = identity.get("executables") or {}
    verdicts: dict[str, bool | None] = {}

    def differs(artifact: str) -> bool | None:
        """Whether the built artifact differs from its record hash; None
        when either side is missing."""
        if artifact not in verdicts:
            expected = (recorded.get(f"<build>/{artifact}") or {}).get("sha256")
            built = build_dir / artifact
            verdicts[artifact] = (None if not expected or not built.is_file()
                                  else hashlib.sha256(built.read_bytes()).hexdigest() != expected)
        return verdicts[artifact]

    changed, unchecked = [], set()
    for artifact in sorted(roots - blind):
        closure, stack = set(), list((executables.get(artifact) or {}).get("spawns") or [])
        while stack:
            member = stack.pop()
            if member not in closure:
                closure.add(member)
                stack.extend((executables.get(member) or {}).get("spawns") or [])
        hit = differs(artifact) is True
        for member in sorted(closure - blind):
            verdict = differs(member)
            if verdict is None:
                unchecked.add(member)
            hit = hit or verdict is True
        if hit:
            changed.append(artifact)
    compared = sum(1 for verdict in verdicts.values() if verdict is not None)
    return {"unreached_changed": changed if compared else None, "unreached_compared": compared,
            "unreached_unchecked_modules": sorted(unchecked)}


def lane_red_allowlist(
    base_sha: str, repo_root: Path = REPO_ROOT, today: str | None = None
) -> tuple[dict[str, str], str] | None:
    """The protected base's unexpired lane reds (name -> expiry, `YYYY-MM-DD`
    UTC) and the sha256 of the file's bytes. An entry past its expiry is
    dropped, so a fixed red cannot keep hiding a new failure under its name."""
    shown = subprocess.run(
        ["git", "-C", str(repo_root), "show", f"{base_sha}:{LANE_RED_ALLOWLIST}"],
        capture_output=True, shell=False)
    if shown.returncode != 0:
        return None
    today = today or time.strftime("%Y-%m-%d", time.gmtime())
    try:
        entries = {entry["name"]: entry["expires"] for entry in json.loads(shown.stdout)["tests"]}
    except (ValueError, KeyError, TypeError):
        return None
    current = {name: expires for name, expires in sorted(entries.items())
               if isinstance(expires, str) and len(expires) == 10 and expires >= today}
    return current, hashlib.sha256(shown.stdout).hexdigest()


def comparison_verdict(
    selected_result: int,
    full_result: int | None,
    sets: tuple[set[str], set[str], set[str]] | None = None,
) -> str:
    """`sets` is (selected registrations, selected-leg failures, full-suite
    failures). With them, two failing legs compare per test: `matched_fail`
    when the selected leg failed nothing the full suite passed and saw every
    full-suite failure inside its selection."""
    if full_result is None:
        return "not_compared"
    if selected_result == 0 and full_result == 0:
        return "matched_pass"
    if selected_result != 0 and full_result != 0:
        if sets is None:
            return "failure_overlap_unproven"
        selection, selected_failures, full_failures = sets
        if not selected_failures or not full_failures:
            return "failure_overlap_unproven"
        if not selected_failures <= full_failures:
            return "selected_only_failure"
        if not (full_failures & selection) <= selected_failures:
            return "missed_full_failure"
        return "matched_fail"
    return "mismatched_non_graduation"


def full_build_timing_fields(
    selected_build_seconds: float | None,
    incremental_full_build_seconds: float | None,
) -> dict[str, bool | float | None]:
    """Describe the sequential shadow build without implying an independent run."""

    if incremental_full_build_seconds is None:
        return {
            "full_build_is_incremental_after_selected": None,
            "full_build_incremental_duration_seconds": None,
            "full_build_estimated_total_duration_seconds": None,
        }
    if selected_build_seconds is None:
        raise SelectionExecutionError(
            "incremental full-build timing has no preceding selected-build timing"
        )
    return {
        "full_build_is_incremental_after_selected": True,
        "full_build_incremental_duration_seconds": incremental_full_build_seconds,
        "full_build_estimated_total_duration_seconds": (
            selected_build_seconds + incremental_full_build_seconds
        ),
    }


def fsync_directory(directory: Path) -> None:
    """Persist a newly published receipt's directory entry on POSIX."""

    descriptor = os.open(directory, os.O_RDONLY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def write_result_receipt(result_dir: Path, receipt: dict[str, Any]) -> Path:
    """Append one owner-private immutable result receipt without overwriting."""

    if not result_dir.is_absolute():
        raise SelectionExecutionError("result receipt directory must be absolute")
    result_dir.mkdir(parents=True, exist_ok=True)
    payload = (json.dumps(receipt, sort_keys=True, separators=(",", ":")) + "\n").encode()
    for sequence in range(100):
        path = result_dir / f"result-{time.time_ns()}-{os.getpid()}-{sequence}.json"
        try:
            descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o400)
        except FileExistsError:
            continue
        with os.fdopen(descriptor, "wb") as result_file:
            result_file.write(payload)
            result_file.flush()
            os.fsync(result_file.fileno())
        fsync_directory(result_dir)
        return path
    raise SelectionExecutionError("cannot allocate immutable result receipt")


def _fallback_receipt_identity(
    args: argparse.Namespace,
) -> tuple[list[str], list[str], dict[str, Any]]:
    """Recover planner identity only after it was bound to this checkout."""

    if getattr(args, "_changed_surface_receipt_identity_verified", False) is not True:
        raise SelectionExecutionError(
            "fallback requires a selection receipt bound to this checkout"
        )
    names, _payload, targets, _target_payload, receipt = decode_selection_receipt(
        args.selection_receipt_b64, args.selection_receipt_sha256
    )
    return names, targets, receipt


def write_fallback_result_receipt(
    args: argparse.Namespace,
    error: BaseException,
    *,
    result_dir: Path,
    selection_identity: tuple[list[str], list[str], dict[str, Any]],
    full_build_result: int | None,
    full_build_seconds: float | None,
    full_result: int,
    full_seconds: float,
) -> None:
    """Record that selected execution refused and full validation stayed authoritative."""

    reason = f"{type(error).__name__}: {error}"[:1024]
    selected_build_completed = (
        getattr(args, "_changed_surface_selected_build_completed", False) is True
    )
    selected_build_seconds = getattr(
        args, "_changed_surface_selected_build_seconds", None
    )
    selected_build_result = getattr(
        args, "_changed_surface_selected_build_result", None
    )
    if not selected_build_completed:
        selected_build_seconds = None
        selected_build_result = None
    if full_build_seconds is None:
        full_build_timing = full_build_timing_fields(None, None)
    elif selected_build_completed:
        full_build_timing = full_build_timing_fields(
            selected_build_seconds, full_build_seconds
        )
    else:
        full_build_timing = {
            "full_build_is_incremental_after_selected": False,
            "full_build_incremental_duration_seconds": full_build_seconds,
            "full_build_estimated_total_duration_seconds": full_build_seconds,
        }
    selected_names, selected_targets, selection = selection_identity
    receipt = {
        "schema_version": 2,
        "recorded_at_unix_ns": time.time_ns(),
        "repository": selection["repository"],
        "pull_request": selection["pull_request"],
        "target": selection["target"],
        "base_sha": selection["base_sha"],
        "head_sha": selection["head_sha"],
        "tree_sha": selection["tree_sha"],
        "execution_payload_sha256": args.selection_receipt_sha256,
        "policy_digest": selection["policy_digest"],
        "selection_receipt_digest": selection["selection_receipt_digest"],
        "validation_contract_digest": selection["validation_contract_digest"],
        "workflow_digest": selection["workflow_digest"],
        "selected_tests_digest": selection["selected_tests_digest"],
        "selected_logical_count": len(selected_names),
        "selected_registration_count": None,
        "full_registration_count": None,
        "prebuild_unbuilt_placeholder_count": None,
        "inventory_validation_deferred_until_full_build": None,
        "verification_duration_seconds": None,
        "selected_duration_seconds": None,
        "selected_returncode": None,
        "selected_build_targets_digest": selection.get(
            "selected_build_targets_digest"
        ),
        "selected_build_target_count": len(selected_targets),
        "selected_build_duration_seconds": selected_build_seconds,
        "selected_build_returncode": selected_build_result,
        "full_duration_seconds": full_seconds,
        "full_returncode": full_result,
        **full_build_timing,
        "full_build_returncode": full_build_result,
        "full_authoritative": True,
        "failure_coverage": "selected_execution_refused",
        "comparison_verdict": "fallback_full_non_graduation",
        "graduation_eligible": False,
        "selected_execution_disposition": "refused_fallback_full",
        "fallback_reason": reason,
    }
    write_result_receipt(result_dir, receipt)


def run_full_fallback(
    args: argparse.Namespace, build_dir: Path, error: BaseException
) -> int:
    """Run the untouched full authority after a shadow-only selection refusal."""

    result_dir_value = os.environ.get("SHIPYARD_CHANGED_SURFACE_RESULT_DIR")
    if not result_dir_value:
        raise SelectionExecutionError(
            "shadow fallback requires an immutable result receipt directory"
        )
    result_dir = Path(result_dir_value)
    if not result_dir.is_absolute():
        raise SelectionExecutionError("result receipt directory must be absolute")
    identity = _fallback_receipt_identity(args)
    selection_schema = identity[2]["schema_version"]
    full_build_result: int | None = None
    full_build_seconds: float | None = None
    if selection_schema == 2:
        full_build_started = time.monotonic()
        full_build_result = subprocess.run(build_argv(build_dir), shell=False).returncode
        full_build_seconds = time.monotonic() - full_build_started
        if full_build_result == 0:
            full_build_result = clear_build_sentinel(build_dir)
    full_result = full_build_result if full_build_result is not None else 0
    full_seconds = 0.0
    if full_build_result in (None, 0):
        reuse_out = lane_reuse_record.record_dir()
        junit = lane_reuse_record.junit_path(reuse_out, "full") if reuse_out else None
        started_epoch = int(time.time())
        full_started = time.monotonic()
        full_result = subprocess.run(execution_argv(build_dir, junit=junit), shell=False).returncode
        full_seconds = time.monotonic() - full_started
        if reuse_out is not None and junit is not None:
            attempts = lane_reuse_record.keep_last_test_log(build_dir, reuse_out, "full")
            lane_reuse_record.record(build_dir, REPO_ROOT, [("full", junit, attempts, True)], started_epoch)
    write_fallback_result_receipt(
        args,
        error,
        result_dir=result_dir,
        selection_identity=identity,
        full_build_result=full_build_result,
        full_build_seconds=full_build_seconds,
        full_result=full_result,
        full_seconds=full_seconds,
    )
    return full_result


def run_keyed_full(args: argparse.Namespace, build_dir: Path, receipt: dict[str, Any]) -> int:
    """A full plan with a bound executable-reuse: derive the selection, then
    run exactly the configured build and test stages, so the verdict is the
    full suite's. The derivation is shadow only and cannot change it."""

    result_dir_value = os.environ.get("SHIPYARD_CHANGED_SURFACE_RESULT_DIR")
    if not result_dir_value or not Path(result_dir_value).is_absolute():
        raise SelectionExecutionError("a keyed full run requires an absolute result receipt directory")
    result_dir = Path(result_dir_value)
    binding = receipt["executable_reuse"]
    derived = derive_executable_reuse(binding, receipt["head_sha"], build_dir, result_dir,
                                      base_sha=receipt["base_sha"])
    args._changed_surface_full_authority_started = True
    env = {**os.environ, **STAGE_ENV}
    with tempfile.TemporaryDirectory(prefix="pulp-changed-surface-") as directory:
        # The suite's report goes into the reuse record when Shipyard asked
        # for one, as the configured test stage's wrapper would put it.
        reuse_out = lane_reuse_record.record_dir()
        recorded = lane_reuse_record.junit_path(reuse_out, "full") if reuse_out is not None else None
        junit = recorded or Path(directory) / "full-junit.xml"
        build_started = time.monotonic()
        build_result = subprocess.run(build_argv(build_dir), shell=False, env=env).returncode
        if build_result == 0:
            build_result = clear_build_sentinel(build_dir)
        build_seconds = time.monotonic() - build_started
        test_result: int | None = None
        test_seconds: float | None = None
        if build_result == 0:
            test_started = time.monotonic()
            started_epoch = int(time.time())
            test_result = subprocess.run(stage_test_argv(build_dir, junit), shell=False, env=env).returncode
            test_seconds = time.monotonic() - test_started
            if recorded is not None:
                attempts = lane_reuse_record.keep_last_test_log(build_dir, reuse_out, "full")
                lane_reuse_record.record(build_dir, REPO_ROOT, [("full", recorded, attempts, True)],
                                         started_epoch)
        measured = false_skips(result_dir, junit) if derived["status"] == "derived" else None
        derived.update(unreached_changed(result_dir, build_dir, binding, derived, "all")
                       if build_result == 0 and derived["status"] == "derived" else UNKNOWN_UNREACHED)
        try:
            inventory_names = [t.get("name") for t in ctest_payload(build_dir)["tests"]]
        except SelectionExecutionError:
            inventory_names = None
    returncode = build_result if build_result != 0 else test_result
    write_result_receipt(result_dir, {
        "schema_version": 2,
        "recorded_at_unix_ns": time.time_ns(),
        **{key: receipt[key] for key in (
            "repository", "pull_request", "target", "base_sha", "head_sha", "tree_sha",
            "policy_digest", "selection_receipt_digest", "validation_contract_digest",
            "workflow_digest")},
        "execution_payload_sha256": args.selection_receipt_sha256,
        "selected_execution_disposition": "keyed_full_shadow",
        "full_authoritative": True,
        # The plan selected nothing; these match its empty selection.
        "selected_tests_digest": "",
        "selected_logical_count": 0,
        "selected_build_targets_digest": None,
        "selected_build_target_count": 0,
        # Every registration ran, so nothing here reads as a bounded selection.
        "selected_tests": inventory_names,
        "full_registration_count": len(inventory_names) if inventory_names is not None else None,
        "full_build_returncode": build_result,
        "full_build_duration_seconds": build_seconds,
        "full_returncode": test_result,
        "full_duration_seconds": test_seconds,
        "comparison_verdict": "keyed_full_shadow",
        "failure_coverage": "not_compared",
        "graduation_eligible": False,
        "executable_reuse": {"mode": "keyed_full_shadow", "bound": binding, "derived": derived,
                             **(measured or UNMEASURED_SKIPS)},
    })
    return returncode


def run_locked(args: argparse.Namespace, build_dir: Path) -> int:
    """Verify and execute one selection while the build tree is exclusive."""

    args._changed_surface_full_authority_started = False
    args._changed_surface_fallback_safe = False
    args._changed_surface_receipt_identity_verified = False
    args._changed_surface_selected_build_completed = False
    args._changed_surface_selected_build_seconds = None
    args._changed_surface_selected_build_result = None
    verification_started = time.monotonic()
    config_path = args.config.resolve(strict=True)
    (
        selected_names,
        selected_payload,
        selected_build_targets,
        _selected_build_target_payload,
        selection_receipt,
    ) = decode_selection_receipt(args.selection_receipt_b64, args.selection_receipt_sha256)
    validate_receipt_identity(selection_receipt, args.target)
    args._changed_surface_receipt_identity_verified = True
    policy = load_policy(config_path, args.target)
    source_root = inventory.source_root_for_build(build_dir).resolve(strict=True)
    if source_root != REPO_ROOT.resolve():
        raise SelectionExecutionError(
            f"build source root {source_root} does not match checkout {REPO_ROOT.resolve()}"
        )
    validate_build_configuration(build_dir, policy)
    # Only selection-layer failures may fall back to the ordinary full stage.
    # Checkout identity, build-source provenance, and live build configuration
    # remain hard prerequisites for accepting any execution result.
    if selection_receipt.get("disposition") == "full":
        return run_keyed_full(args, build_dir, selection_receipt)
    args._changed_surface_fallback_safe = True
    require_ctest_version()
    base = base_projection(selection_receipt["base_sha"], policy, build_dir)
    compare_full = os.environ.get("SHIPYARD_CHANGED_SURFACE_COMPARE_FULL") == "1"
    executable_reuse = None
    binding = selection_receipt.get("executable_reuse")
    if binding is not None and os.environ.get("SHIPYARD_CHANGED_SURFACE_RESULT_DIR"):
        executable_reuse = {"mode": "keyed_bounded_shadow", "bound": binding,
                            "derived": derive_executable_reuse(
            binding, selection_receipt["head_sha"], build_dir,
            Path(os.environ["SHIPYARD_CHANGED_SURFACE_RESULT_DIR"]), base_sha=selection_receipt["base_sha"])}
    with tempfile.TemporaryDirectory(prefix="pulp-changed-surface-") as directory:
        selected_file = write_private_selection(Path(directory), selected_payload)
        snapshot_identity = selected_file.stat()
        full_payload = ctest_payload(build_dir)
        full_tests = full_payload["tests"]
        selected_tests = ctest_json(build_dir, selected_file)
        # The reference for the check after the full build, which a
        # configure-only base cannot be for a built tree.
        prebuild_snapshot = inventory.project_registrations(
            full_payload, source_root, build_dir, shape="configure")
        (Path(directory) / "prebuild-registrations.json").write_text(
            json.dumps(prebuild_snapshot, sort_keys=True), encoding="utf-8")
        prebuild_unbuilt_placeholder_count = 0
        try:
            validate_selection(
                selected_names=selected_names,
                full_payload=full_payload,
                selected_tests=selected_tests,
                source_root=source_root,
                build_dir=build_dir,
                policy=policy,
                base=base,
                target=args.target,
                # Nothing is built yet; the full build is checked strictly.
                require_built=False,
            )
        except inventory.InventoryError as error:
            if (
                not compare_full
                or selection_receipt["schema_version"] != 2
                or "has no unambiguous command" not in str(error)
            ):
                raise
            _, placeholders = inventory.split_proven_unbuilt_placeholders(
                full_tests, build_dir
            )
            if not placeholders:
                raise
            prebuild_unbuilt_placeholder_count = len(placeholders)
            if selection_receipt["schema_version"] == 2:
                validate_build_target_projection(
                    build_dir=build_dir,
                    selected_tests=[],
                    selected_build_targets=selected_build_targets,
                )
        if (
            selection_receipt["schema_version"] == 2
            and not prebuild_unbuilt_placeholder_count
        ):
            validate_build_target_projection(
                build_dir=build_dir,
                selected_tests=selected_tests,
                selected_build_targets=selected_build_targets,
            )
        verification_seconds = time.monotonic() - verification_started
        selected_build_result: int | None = None
        selected_build_seconds: float | None = None
        if selection_receipt["schema_version"] == 2:
            selected_build_started = time.monotonic()
            selected_build_result = subprocess.run(
                build_argv(build_dir, selected_build_targets), shell=False
            ).returncode
            selected_build_seconds = time.monotonic() - selected_build_started
            args._changed_surface_selected_build_completed = True
            args._changed_surface_selected_build_seconds = selected_build_seconds
            args._changed_surface_selected_build_result = selected_build_result
        if selected_build_result in (None, 0) and prebuild_unbuilt_placeholder_count:
            deferred_verification_started = time.monotonic()
            full_payload = ctest_payload(build_dir)
            full_tests = full_payload["tests"]
            selected_tests = ctest_json(build_dir, selected_file)
            validate_after_selected_build(
                selected_names=selected_names,
                full_payload=full_payload,
                selected_tests=selected_tests,
                source_root=source_root,
                build_dir=build_dir,
                policy=policy,
                base=base,
                target=args.target,
                selected_build_targets=selected_build_targets,
            )
            verification_seconds += time.monotonic() - deferred_verification_started
        selected_seconds = 0.0
        reuse_out = lane_reuse_record.record_dir()
        legs_started = int(time.time())
        selected_attempts: Path | None = None
        full_attempts: Path | None = None
        if selected_build_result in (None, 0):
            selected_started = time.monotonic()
            selected_result = subprocess.run(
                execution_argv(build_dir, selected_file, Path(directory) / "selected-junit.xml"),
                shell=False,
            ).returncode
            selected_seconds = time.monotonic() - selected_started
            if reuse_out is not None:
                selected_attempts = lane_reuse_record.keep_last_test_log(build_dir, reuse_out, "pr-affected")
        else:
            selected_result = selected_build_result
        full_build_result: int | None = None
        full_build_seconds: float | None = None
        full_result: int | None = None
        full_seconds: float | None = None
        if compare_full:
            args._changed_surface_full_authority_started = True
            if selection_receipt["schema_version"] == 2:
                full_build_started = time.monotonic()
                full_build_result = subprocess.run(build_argv(build_dir), shell=False).returncode
                full_build_seconds = time.monotonic() - full_build_started
                if full_build_result == 0:
                    full_build_result = clear_build_sentinel(build_dir)
                if full_build_result == 0 and prebuild_unbuilt_placeholder_count:
                    # The selected leg runs first from its provenance-backed
                    # runnable subset.  The authoritative full build must then
                    # hydrate every deferred registration before the ordinary
                    # exact-inventory check and full test execution.
                    full_payload = ctest_payload(build_dir)
                    full_tests = full_payload["tests"]
                    selected_tests = ctest_json(build_dir, selected_file)
                    _, remaining_placeholders = (
                        inventory.split_proven_unbuilt_placeholders(
                            full_tests, build_dir
                        )
                    )
                    if remaining_placeholders:
                        raise SelectionExecutionError(
                            "full build left provenance-backed CTest placeholders unresolved"
                        )
                    validate_selection(
                        selected_names=selected_names,
                        full_payload=full_payload,
                        selected_tests=selected_tests,
                        source_root=source_root,
                        build_dir=build_dir,
                        policy=policy,
                        base=prebuild_snapshot,
                        target=args.target,
                        reference_label=PREBUILD_SNAPSHOT,
                    )
            full_seconds = 0.0
            if full_build_result in (None, 0):
                full_started = time.monotonic()
                full_result = subprocess.run(
                    execution_argv(build_dir, junit=Path(directory) / "full-junit.xml"),
                    shell=False,
                ).returncode
                full_seconds = time.monotonic() - full_started
                if reuse_out is not None:
                    full_attempts = lane_reuse_record.keep_last_test_log(build_dir, reuse_out, "full")
            else:
                full_result = full_build_result
        elif selection_receipt["schema_version"] == 2 and selected_result == 0:
            selected_result = clear_build_sentinel(build_dir)
        full_build_timing = full_build_timing_fields(
            selected_build_seconds, full_build_seconds
        )
        final_identity = selected_file.stat()
        if (
            (snapshot_identity.st_dev, snapshot_identity.st_ino)
            != (final_identity.st_dev, final_identity.st_ino)
            or selected_file.read_bytes() != selected_payload
        ):
            raise SelectionExecutionError("private selected-tests snapshot changed during execution")
        # Before the private directory holding the JUnit reports is removed.
        lane_reuse_record.record_legs(
            build_dir,
            source_root,
            [("pr-affected", Path(directory) / "selected-junit.xml", selected_attempts),
             ("full", Path(directory) / "full-junit.xml", full_attempts)],
            "success" if selected_build_result in (None, 0) and full_build_result in (None, 0) else "failure",
            legs_started,
        )
        result_dir = os.environ.get("SHIPYARD_CHANGED_SURFACE_RESULT_DIR")
        if result_dir:
            selected_failures = junit_failures(Path(directory) / "selected-junit.xml")
            full_failures = junit_failures(Path(directory) / "full-junit.xml")
            sets = (None if selected_failures is None or full_failures is None
                    else (set(selected_names), selected_failures, full_failures))
            verdict = comparison_verdict(selected_result, full_result, sets)
            allowlist = lane_red_allowlist(selection_receipt["base_sha"])
            outside = sorted(full_failures - set(selected_names)) if full_failures is not None else None
            absorbed = (len(set(outside or []) & set(allowlist[0]))
                        if allowlist is not None else None)
            # In-selection failures that are themselves lane reds: a matched_fail
            # resting on these proved less than one resting on new failures.
            absorbed_selected = (len((selected_failures or set()) & set(allowlist[0]))
                                 if allowlist is not None else None)
            if executable_reuse and executable_reuse["derived"]["status"] == "derived":
                executable_reuse.update(false_skips(Path(result_dir), Path(directory) / "full-junit.xml")
                                        or UNMEASURED_SKIPS)
                executable_reuse["derived"].update(unreached_changed(
                    Path(result_dir), build_dir, executable_reuse["bound"], executable_reuse["derived"],
                    "sampled"))
            elif executable_reuse:
                executable_reuse.update(UNMEASURED_SKIPS)
                executable_reuse["derived"].update(UNKNOWN_UNREACHED)
            eligible = compare_full and (
                verdict == "matched_pass"
                or (verdict == "matched_fail" and allowlist is not None
                    and set(outside or []) <= set(allowlist[0])))
            write_result_receipt(
                Path(result_dir),
                {
                    "schema_version": 2,
                    "recorded_at_unix_ns": time.time_ns(),
                    "repository": selection_receipt["repository"],
                    "pull_request": selection_receipt["pull_request"],
                    "target": selection_receipt["target"],
                    "base_sha": selection_receipt["base_sha"],
                    "head_sha": selection_receipt["head_sha"],
                    "tree_sha": selection_receipt["tree_sha"],
                    "execution_payload_sha256": args.selection_receipt_sha256,
                    **({"executable_reuse": executable_reuse} if executable_reuse else {}),
                    "policy_digest": selection_receipt["policy_digest"],
                    "selection_receipt_digest": selection_receipt[
                        "selection_receipt_digest"
                    ],
                    "validation_contract_digest": selection_receipt[
                        "validation_contract_digest"
                    ],
                    "workflow_digest": selection_receipt["workflow_digest"],
                    "selected_tests_digest": selection_receipt["selected_tests_digest"],
                    "selected_logical_count": len(selected_names),
                    "selected_registration_count": len(selected_tests),
                    "full_registration_count": len(full_tests),
                    "base_inventory_rows": base.get("row_count"),
                    # Rows the base listed without a program (an unbuilt
                    # target), so compared on name, arguments and properties.
                    "base_inventory_name_only_rows": inventory.name_only_rows(base),
                    "base_inventory_configure_seconds": base.get("configure_seconds"),
                    # The environment the base and this tree were compared
                    # under, so an equal registration set is shown to come
                    # from an equal environment.
                    "base_inventory_environment": base.get("provisioning"),
                    "base_inventory_linked_externals": base.get("linked_externals"),
                    "prebuild_unbuilt_placeholder_count": (
                        prebuild_unbuilt_placeholder_count
                    ),
                    "inventory_validation_deferred_until_full_build": bool(
                        prebuild_unbuilt_placeholder_count
                    ),
                    "verification_duration_seconds": verification_seconds,
                    "selected_duration_seconds": selected_seconds,
                    "selected_returncode": selected_result,
                    "selected_build_targets_digest": selection_receipt.get(
                        "selected_build_targets_digest"
                    ),
                    "selected_build_target_count": len(selected_build_targets),
                    "selected_build_duration_seconds": selected_build_seconds,
                    "selected_build_returncode": selected_build_result,
                    "full_duration_seconds": full_seconds,
                    "full_returncode": full_result,
                    **full_build_timing,
                    "full_build_returncode": full_build_result,
                    "full_authoritative": compare_full,
                    "failure_coverage": failure_coverage(selected_result, full_result),
                    "comparison_verdict": verdict,
                    "graduation_eligible": eligible,
                    # The named sets Shipyard recomputes a matched_fail from.
                    "selected_tests": list(selected_names),
                    "selected_failures": (sorted(selected_failures)
                                          if selected_failures is not None else None),
                    "full_failures": sorted(full_failures) if full_failures is not None else None,
                    "full_failures_outside_selection": outside,
                    "lane_red_allowlist": sorted(allowlist[0]) if allowlist else None,
                    "lane_red_allowlist_expires": allowlist[0] if allowlist else None,
                    # How much of a matched_fail rested on the allowlist.
                    "allowlisted_failure_count": absorbed,
                    "allowlisted_selected_failure_count": absorbed_selected,
                    "lane_red_allowlist_sha256": allowlist[1] if allowlist else None,
                },
            )
        return full_result if full_result is not None else selected_result


def run(args: argparse.Namespace) -> int:
    build_dir = args.build_dir.resolve(strict=True)
    with build_dir_lock.exclusive_build_dir(build_dir):
        try:
            return run_locked(args, build_dir)
        except (
            SelectionExecutionError,
            inventory.InventoryError,
            OSError,
            json.JSONDecodeError,
        ) as error:
            if getattr(args, "_changed_surface_full_authority_started", False) is True:
                raise FullAuthorityExecutionError(str(error)) from error
            if os.environ.get("SHIPYARD_CHANGED_SURFACE_COMPARE_FULL") != "1":
                raise
            if getattr(args, "_changed_surface_fallback_safe", False) is not True:
                raise
            args._changed_surface_full_authority_started = True
            try:
                return run_full_fallback(args, build_dir, error)
            except (OSError, SelectionExecutionError) as full_error:
                raise FullAuthorityExecutionError(str(full_error)) from full_error


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--selection-receipt-b64", required=True)
    parser.add_argument("--selection-receipt-sha256", required=True)
    parser.add_argument("--build-dir", default=REPO_ROOT / "build", type=Path)
    parser.add_argument("--config", default=DEFAULT_CONFIG, type=Path)
    parser.add_argument("--target", default="mac")
    return parser.parse_args(argv)


def main() -> int:
    try:
        return run(parse_args())
    except (
        SelectionExecutionError,
        inventory.InventoryError,
        FullAuthorityExecutionError,
        OSError,
        json.JSONDecodeError,
    ) as error:
        print(f"changed-surface execution refused: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
