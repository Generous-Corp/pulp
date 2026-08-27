#!/usr/bin/env python3
"""Issue and verify exact-tree CI receipts for protected merge groups.

The receipt is evidence about one completed pull-request validation.  It is
never itself a merge-group verdict: ``verify`` derives a new, subject-bound
decision after proving that the merge-group commit has the identical tree and
parents as the pull-request checkout recorded by the receipt.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import shutil
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request
import zipfile
from pathlib import Path
from typing import Any, Iterable


SCHEMA = "pulp.protected-validation-receipt/v1"
DECISION_SCHEMA = "pulp.protected-merge-reuse-decision/v1"
DOMAIN = b"pulp-protected-validation-receipt-v1\0"
POLICY_PATHS = (
    ".github/workflows/build.yml",
    ".agents/contract.toml",
    "tools/scripts/classify_changes.py",
    "tools/scripts/protected_merge_receipt.py",
)


class ReceiptError(ValueError):
    """Receipt evidence is absent, ambiguous, or does not match exactly."""


def canonical_json(value: Any) -> bytes:
    return json.dumps(
        value, ensure_ascii=False, sort_keys=True, separators=(",", ":")
    ).encode("utf-8")


def digest(value: Any) -> str:
    return hashlib.sha256(DOMAIN + canonical_json(value)).hexdigest()


def _git(repo: Path, *args: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(repo), *args],
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode:
        raise ReceiptError(result.stderr.strip() or f"git {' '.join(args)} failed")
    return result.stdout.strip()


def commit_identity(repo: Path, sha: str) -> dict[str, Any]:
    resolved = _git(repo, "rev-parse", f"{sha}^{{commit}}")
    tree = _git(repo, "show", "-s", "--format=%T", resolved)
    parents = _git(repo, "show", "-s", "--format=%P", resolved).split()
    return {"sha": resolved, "tree": tree, "parents": parents}


def policy_identity(repo: Path, revision: str) -> dict[str, Any]:
    records = []
    for path in POLICY_PATHS:
        try:
            blob = _git(repo, "rev-parse", f"{revision}:{path}")
        except ReceiptError as error:
            raise ReceiptError(f"policy path unavailable: {path}") from error
        records.append({"path": path, "blob": blob})
    return {"paths": records, "digest": digest(records)}


def _sha256_file(path: Path) -> str:
    hasher = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            hasher.update(chunk)
    return hasher.hexdigest()


def artifact_identity(build_dir: Path, ctest_json: Path | None = None) -> dict[str, Any]:
    if ctest_json is None:
        result = subprocess.run(
            ["ctest", "--test-dir", str(build_dir), "--show-only=json-v1"],
            check=False,
            capture_output=True,
            text=True,
        )
        if result.returncode:
            raise ReceiptError("CTest inventory unavailable after successful validation")
        try:
            inventory = json.loads(result.stdout)
        except json.JSONDecodeError as error:
            raise ReceiptError("CTest inventory is not valid JSON") from error
    else:
        inventory = json.loads(ctest_json.read_text(encoding="utf-8"))

    paths: set[Path] = set()
    commands: list[dict[str, Any]] = []
    for test in inventory.get("tests", []):
        command = test.get("command")
        if not isinstance(command, list) or not command:
            raise ReceiptError("CTest inventory contains an unbound command")
        executable = Path(command[0])
        if not executable.is_absolute():
            if len(executable.parts) == 1:
                resolved_command = shutil.which(command[0])
                if resolved_command is None:
                    raise ReceiptError(f"tested command unavailable: {command[0]}")
                executable = Path(resolved_command)
            else:
                executable = build_dir / executable
        try:
            executable = executable.resolve(strict=True)
        except OSError as error:
            raise ReceiptError(f"tested artifact unavailable: {command[0]}") from error
        if not executable.is_file():
            raise ReceiptError(f"tested artifact is not a file: {executable}")
        paths.add(executable)
        commands.append({"name": test.get("name"), "command": command})
    if not paths:
        raise ReceiptError("CTest inventory contains no tested artifacts")
    records = [
        {"path": os.path.relpath(path, build_dir), "sha256": _sha256_file(path)}
        for path in sorted(paths)
    ]
    identity = {
        "files": records,
        "ctest_inventory_digest": digest(commands),
    }
    identity["digest"] = digest(identity)
    return identity


def _version(command: list[str]) -> str:
    result = subprocess.run(command, check=False, capture_output=True, text=True)
    if result.returncode:
        raise ReceiptError(f"toolchain probe failed: {' '.join(command)}")
    return (result.stdout or result.stderr).strip()


def _cmake_cache_contract(build_dir: Path) -> dict[str, str]:
    cache_path = build_dir / "CMakeCache.txt"
    try:
        lines = cache_path.read_text(encoding="utf-8", errors="strict").splitlines()
    except OSError as error:
        raise ReceiptError("CMake toolchain cache is unavailable") from error
    wanted = {
        "CMAKE_BUILD_TYPE", "CMAKE_CXX_COMPILER", "CMAKE_CXX_COMPILER_ID",
        "CMAKE_CXX_COMPILER_VERSION", "CMAKE_GENERATOR", "CMAKE_OSX_ARCHITECTURES",
        "CMAKE_SYSTEM_NAME", "CMAKE_SYSTEM_PROCESSOR",
    }
    values: dict[str, str] = {}
    for line in lines:
        if not line or line.startswith(("#", "//")) or "=" not in line or ":" not in line:
            continue
        key_and_type, value = line.split("=", 1)
        key = key_and_type.split(":", 1)[0]
        if key in wanted:
            values[key] = value
    compiler_text = values.get("CMAKE_CXX_COMPILER")
    if not compiler_text:
        raise ReceiptError("CMake C++ compiler identity is unavailable")
    compiler = Path(compiler_text).resolve(strict=True)
    values["CMAKE_CXX_COMPILER_REALPATH"] = str(compiler)
    values["CMAKE_CXX_COMPILER_SHA256"] = _sha256_file(compiler)
    values["CMAKE_CXX_COMPILER_VERSION_OUTPUT"] = _version([str(compiler), "--version"])
    return values


def toolchain_identity(target: str, build_dir: Path) -> dict[str, Any]:
    values = {
        "target": target,
        "runner_os": os.environ.get("RUNNER_OS", platform.system()),
        "runner_arch": os.environ.get("RUNNER_ARCH", platform.machine()),
        "runner_image": os.environ.get("ImageOS", "self-hosted"),
        "runner_image_version": os.environ.get("ImageVersion", "self-hosted"),
        "python": platform.python_version(),
        "cmake_version": _version(["cmake", "--version"]),
        "ctest_version": _version(["ctest", "--version"]),
        "cmake_cache": _cmake_cache_contract(build_dir),
    }
    return {"values": values, "digest": digest(values)}


def issue(args: argparse.Namespace) -> dict[str, Any]:
    repo = args.repo.resolve()
    checkout = commit_identity(repo, args.checkout_sha)
    expected_parents = [
        _git(repo, "rev-parse", f"{args.base_sha}^{{commit}}"),
        _git(repo, "rev-parse", f"{args.head_sha}^{{commit}}"),
    ]
    if checkout["parents"] != expected_parents:
        raise ReceiptError("validated checkout is not the exact base+head merge")
    policy = policy_identity(repo, checkout["sha"])
    artifact = artifact_identity(args.build_dir.resolve(), args.ctest_json)
    body = {
        "schema": SCHEMA,
        "repository": args.repository,
        "workflow": args.workflow,
        "workflow_sha": args.workflow_sha,
        "run_id": args.run_id,
        "run_attempt": args.run_attempt,
        "target": args.target,
        "base_sha": expected_parents[0],
        "head_sha": expected_parents[1],
        "validated_checkout": checkout,
        "policy": policy,
        "toolchain": toolchain_identity(args.target, args.build_dir.resolve()),
        "artifact": artifact,
        "validation": {"conclusion": "success", "ctest_exit": 0},
    }
    body["receipt_digest"] = digest(body)
    return body


def _require_exact_keys(value: dict[str, Any], expected: Iterable[str], label: str) -> None:
    expected_set = set(expected)
    if set(value) != expected_set:
        raise ReceiptError(f"{label} fields do not match schema")


def verify_receipt(receipt: dict[str, Any], args: argparse.Namespace) -> dict[str, Any]:
    _require_exact_keys(
        receipt,
        (
            "schema", "repository", "workflow", "workflow_sha", "run_id",
            "run_attempt", "target", "base_sha", "head_sha",
            "validated_checkout", "policy", "toolchain", "artifact",
            "validation", "receipt_digest",
        ),
        "receipt",
    )
    supplied_digest = receipt["receipt_digest"]
    unsigned = dict(receipt)
    del unsigned["receipt_digest"]
    if receipt["schema"] != SCHEMA or supplied_digest != digest(unsigned):
        raise ReceiptError("receipt digest or schema is invalid")
    if receipt["repository"] != args.repository or receipt["workflow"] != args.workflow:
        raise ReceiptError("repository or workflow identity changed")
    if receipt["target"] != args.target:
        raise ReceiptError("target identity changed")
    if receipt["validation"] != {"conclusion": "success", "ctest_exit": 0}:
        raise ReceiptError("receipt does not record successful validation")
    if not str(receipt["run_id"]).isdigit() or not str(receipt["run_attempt"]).isdigit():
        raise ReceiptError("workflow run identity is malformed")

    repo = args.repo.resolve()
    group = commit_identity(repo, args.group_sha)
    if len(group["parents"]) != 2:
        raise ReceiptError("merge group is not a two-parent candidate")
    if group["parents"] != [receipt["base_sha"], receipt["head_sha"]]:
        raise ReceiptError("merge-group head or base changed")
    validated = receipt["validated_checkout"]
    if set(validated) != {"sha", "tree", "parents"}:
        raise ReceiptError("validated checkout identity is malformed")
    if validated.get("parents") != group["parents"] or validated.get("tree") != group["tree"]:
        raise ReceiptError("merge-group tree is not identical to validated PR tree")
    current_policy = policy_identity(repo, group["sha"])
    protected_policy = policy_identity(repo, group["parents"][0])
    if current_policy != protected_policy:
        raise ReceiptError("validation policy differs from protected base")
    if receipt["policy"] != current_policy:
        raise ReceiptError("validation policy changed")
    policy = receipt["policy"]
    if set(policy) != {"paths", "digest"} or policy.get("digest") != digest(policy.get("paths")):
        raise ReceiptError("validation policy identity is malformed")
    toolchain = receipt["toolchain"]
    if (
        set(toolchain) != {"values", "digest"}
        or not isinstance(toolchain.get("values"), dict)
        or toolchain.get("digest") != digest(toolchain.get("values"))
        or toolchain["values"].get("target") != args.target
    ):
        raise ReceiptError("toolchain identity is malformed")
    artifact = receipt["artifact"]
    if set(artifact) != {"files", "ctest_inventory_digest", "digest"}:
        raise ReceiptError("tested artifact identity is malformed")
    unsigned_artifact = dict(artifact)
    artifact_digest = unsigned_artifact.pop("digest", None)
    if not isinstance(artifact_digest, str) or artifact_digest != digest(unsigned_artifact):
        raise ReceiptError("tested artifact identity is malformed")

    decision = {
        "schema": DECISION_SCHEMA,
        "repository": args.repository,
        "target": args.target,
        "merge_group_sha": group["sha"],
        "merge_group_tree": group["tree"],
        "source_receipt_digest": supplied_digest,
        "source_run_id": receipt["run_id"],
        "source_run_attempt": receipt["run_attempt"],
        "policy_digest": current_policy["digest"],
        "toolchain_digest": toolchain["digest"],
        "artifact_digest": artifact["digest"],
        "verdict": "reuse",
    }
    decision["decision_digest"] = digest(decision)
    return decision


class _CredentialSafeRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, msg, headers, new_url):
        redirected = super().redirect_request(request, fp, code, msg, headers, new_url)
        if redirected is None:
            return None
        if urllib.parse.urlsplit(request.full_url).netloc != urllib.parse.urlsplit(new_url).netloc:
            redirected.remove_header("Authorization")
        return redirected


def _api_json(opener, url: str, token: str) -> dict[str, Any]:
    request = urllib.request.Request(
        url,
        headers={
            "Accept": "application/vnd.github+json",
            "Authorization": f"Bearer {token}",
            "X-GitHub-Api-Version": "2022-11-28",
        },
    )
    with opener.open(request, timeout=30) as response:
        return json.load(response)


def download(args: argparse.Namespace) -> dict[str, Any]:
    opener = urllib.request.build_opener(_CredentialSafeRedirect())
    encoded_name = urllib.parse.quote(args.artifact_name, safe="")
    root = args.api_url.rstrip("/")
    listing = _api_json(
        opener,
        f"{root}/repos/{args.repository}/actions/artifacts?name={encoded_name}&per_page=100",
        args.token,
    )
    artifacts = [
        item for item in listing.get("artifacts", [])
        if item.get("name") == args.artifact_name and not item.get("expired", True)
    ]
    if len(artifacts) != 1:
        raise ReceiptError("expected exactly one unexpired exact-name artifact")
    artifact = artifacts[0]
    run_id = str(artifact.get("workflow_run", {}).get("id", ""))
    run = _api_json(
        opener,
        f"{root}/repos/{args.repository}/actions/runs/{run_id}",
        args.token,
    )
    pulls = run.get("pull_requests")
    if (
        run.get("name") != args.workflow
        or run.get("event") != "pull_request"
        or run.get("conclusion") != "success"
        or run.get("head_sha") != args.head_sha
        or not isinstance(pulls, list)
        or len(pulls) != 1
        or pulls[0].get("head", {}).get("sha") != args.head_sha
        or pulls[0].get("base", {}).get("sha") != args.base_sha
    ):
        raise ReceiptError("artifact workflow run is not the exact successful pull request")
    archive_request = urllib.request.Request(
        artifact["archive_download_url"],
        headers={
            "Accept": "application/vnd.github+json",
            "Authorization": f"Bearer {args.token}",
            "X-GitHub-Api-Version": "2022-11-28",
        },
    )
    with opener.open(archive_request, timeout=60) as response:
        archive = response.read(64 * 1024 * 1024 + 1)
    if len(archive) > 64 * 1024 * 1024:
        raise ReceiptError("receipt artifact exceeds size bound")
    metadata_digest = artifact.get("digest", "")
    if metadata_digest != f"sha256:{hashlib.sha256(archive).hexdigest()}":
        raise ReceiptError("artifact archive digest does not match GitHub metadata")
    import io
    with zipfile.ZipFile(io.BytesIO(archive)) as bundle:
        if bundle.namelist() != ["receipt.json"]:
            raise ReceiptError("receipt artifact has unexpected archive layout")
        raw = bundle.read("receipt.json")
    if len(raw) > 1024 * 1024:
        raise ReceiptError("receipt exceeds size bound")
    parsed = json.loads(raw)
    if str(parsed.get("run_id")) != run_id:
        raise ReceiptError("receipt run identity differs from artifact authority")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(canonical_json(parsed) + b"\n")
    return {"run_id": run_id, "artifact_id": str(artifact["id"])}


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--repo", type=Path, default=Path.cwd())
    common.add_argument("--repository", required=True)
    common.add_argument("--workflow", default="Build and Test")
    common.add_argument("--target", choices=("macos", "linux"), required=True)

    create = subparsers.add_parser("issue", parents=[common])
    create.add_argument("--base-sha", required=True)
    create.add_argument("--head-sha", required=True)
    create.add_argument("--checkout-sha", required=True)
    create.add_argument("--workflow-sha", required=True)
    create.add_argument("--run-id", required=True)
    create.add_argument("--run-attempt", required=True)
    create.add_argument("--build-dir", type=Path, required=True)
    create.add_argument("--ctest-json", type=Path)
    create.add_argument("--output", type=Path, required=True)

    verify = subparsers.add_parser("verify", parents=[common])
    verify.add_argument("--group-sha", required=True)
    verify.add_argument("--receipt", type=Path, required=True)
    verify.add_argument("--output", type=Path, required=True)

    fetch = subparsers.add_parser("download", parents=[common])
    fetch.add_argument("--api-url", default="https://api.github.com")
    fetch.add_argument("--token", required=True)
    fetch.add_argument("--artifact-name", required=True)
    fetch.add_argument("--base-sha", required=True)
    fetch.add_argument("--head-sha", required=True)
    fetch.add_argument("--output", type=Path, required=True)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.command == "issue":
            result = issue(args)
        elif args.command == "download":
            result = download(args)
            print(canonical_json(result).decode("utf-8"))
            return 0
        else:
            raw = args.receipt.read_bytes()
            result = verify_receipt(json.loads(raw), args)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(canonical_json(result) + b"\n")
    except (OSError, ReceiptError, json.JSONDecodeError) as error:
        print(f"protected receipt: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
