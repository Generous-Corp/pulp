#!/usr/bin/env python3
"""Build Pulp's Vellum SDK consumers against Vellum main's published SDK.

Two subcommands back the `vellum-sdk-consumer` workflow:

resolve
    Find the newest successful "Locked macOS GPU proof" push run on Vellum
    main, download its `vellum-gpu-macos-arm64-evidence` artifact, check the
    SDK archive against the artifact's SHA256SUMS, unpack it, and write a
    receipt naming the run id, head sha, artifact id and archive sha256. With
    --previous-receipt it reports `unchanged` when Vellum main's head equals
    the head that receipt recorded, so a scheduled run can stop early. Any
    lookup, download or digest failure exits non-zero; nothing is guessed.

negative-control
    Prove the lane can see a broken consumer. It takes the compile command of
    one translation unit from the configured build (`ninja -t commands`),
    turns it into a syntax-only compile, and runs it twice: against the SDK as
    published, where it must succeed, and against a copy of the SDK's mirror
    headers with one header renamed, where it must fail naming that header.
    A failure for any other reason is INCONCLUSIVE, never a pass.

Exit codes: 0 success (resolve) or CONFIRMED (negative-control); 1 the lane
did not detect the planted break (NOT CONFIRMED); 2 inconclusive or an error.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.error
import urllib.parse
import urllib.request
import zipfile
from dataclasses import dataclass
from pathlib import Path

VELLUM_REPO = "Generous-Corp/vellum"
GPU_WORKFLOW_NAME = "Locked macOS GPU proof"
EVIDENCE_ARTIFACT = "vellum-gpu-macos-arm64-evidence"
ARCHIVE_PREFIX = "vellum-sdk-"
ARCHIVE_SUFFIX = "-darwin-arm64.tar.gz"
RECEIPT_SCHEMA = "pulp.vellum-sdk-consumer.receipt.v1"
RECEIPT_ARTIFACT = "vellum-sdk-consumer-receipt"

CONFIRMED, NOT_CONFIRMED, INCONCLUSIVE = 0, 1, 2


class ResolveError(RuntimeError):
    pass


# --------------------------------------------------------------------------
# resolve


class _DropAuthOnRedirect(urllib.request.HTTPRedirectHandler):
    """Artifact downloads redirect to signed blob storage, which rejects a
    forwarded GitHub Authorization header; never send the token off-host."""

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        new = super().redirect_request(req, fp, code, msg, headers, newurl)
        if new is not None and urllib.parse.urlsplit(newurl).hostname != urllib.parse.urlsplit(req.full_url).hostname:
            new.remove_header("Authorization")
        return new


_OPENER = urllib.request.build_opener(_DropAuthOnRedirect)


def _open_url(request: urllib.request.Request, timeout: int = 300):
    """Open a URL through _OPENER (the auth-dropping redirect handler).

    Goes through urlopen, not _OPENER.open, so this stays a byte-level network
    read and not a text-mode file open for the encoding lint."""
    urllib.request.install_opener(_OPENER)
    return urllib.request.urlopen(request, timeout=timeout)


def _api(path: str, token: str, *, raw: bool = False):
    if not token:
        raise ResolveError("no GitHub token: set GH_TOKEN (artifact downloads need an authenticated token)")
    request = urllib.request.Request(
        f"https://api.github.com/{path}",
        headers={
            "Authorization": f"Bearer {token}",
            "Accept": "application/vnd.github+json",
            "X-GitHub-Api-Version": "2022-11-28",
        },
    )
    try:
        with _open_url(request, timeout=300) as response:
            body = response.read()
    except urllib.error.HTTPError as error:
        raise ResolveError(f"GET {path}: HTTP {error.code} {error.reason}") from error
    except urllib.error.URLError as error:
        raise ResolveError(f"GET {path}: {error.reason}") from error
    return body if raw else json.loads(body)


def pick_run(runs: list[dict]) -> dict:
    """The newest successful push run of the GPU proof on main."""
    candidates = [
        run for run in runs
        if run.get("name") == GPU_WORKFLOW_NAME
        and run.get("event") == "push"
        and run.get("head_branch") == "main"
        and run.get("conclusion") == "success"
    ]
    if not candidates:
        raise ResolveError(f"no successful '{GPU_WORKFLOW_NAME}' push run on {VELLUM_REPO} main")
    return max(candidates, key=lambda run: (run.get("created_at") or "", run.get("id") or 0))


def pick_artifact(artifacts: list[dict]) -> dict:
    matches = [a for a in artifacts if a.get("name") == EVIDENCE_ARTIFACT]
    if len(matches) != 1:
        raise ResolveError(f"expected one '{EVIDENCE_ARTIFACT}' artifact, found {len(matches)}")
    if matches[0].get("expired"):
        raise ResolveError(f"artifact {matches[0].get('id')} has expired")
    return matches[0]


def verify_archive(directory: Path) -> tuple[Path, str]:
    """The SDK archive in `directory`, checked against SHA256SUMS there."""
    archives = sorted(p for p in directory.iterdir()
                      if p.name.startswith(ARCHIVE_PREFIX) and p.name.endswith(ARCHIVE_SUFFIX))
    if len(archives) != 1:
        raise ResolveError(f"expected one {ARCHIVE_PREFIX}*{ARCHIVE_SUFFIX} in the artifact, found {len(archives)}")
    archive = archives[0]
    sums = directory / "SHA256SUMS"
    if not sums.is_file():
        raise ResolveError("the artifact has no SHA256SUMS")
    expected = None
    for line in sums.read_text(encoding="utf-8").splitlines():
        parts = line.split()
        if len(parts) == 2 and parts[1].lstrip("*") == archive.name:
            expected = parts[0].lower()
    if expected is None:
        raise ResolveError(f"SHA256SUMS does not list {archive.name}")
    actual = hashlib.sha256(archive.read_bytes()).hexdigest()
    if actual != expected:
        raise ResolveError(f"{archive.name}: sha256 {actual} does not match SHA256SUMS {expected}")
    return archive, actual


def _safe_extract(archive: Path, destination: Path) -> None:
    root = destination.resolve()
    with tarfile.open(archive) as tar:
        for member in tar.getmembers():
            target = (destination / member.name).resolve()
            if root != target and root not in target.parents:
                raise ResolveError(f"archive member escapes the destination: {member.name}")
            if member.issym() or member.islnk():
                link = (target.parent / member.linkname).resolve()
                if root != link and root not in link.parents:
                    raise ResolveError(f"archive link escapes the destination: {member.name}")
        tar.extractall(destination)


def previous_head(repository: str, workflow: str, token: str) -> str | None:
    """The Vellum head the last successful run of `workflow` recorded, if any.

    Only a run that finished green counts, so a red run is retried next time.
    Anything missing (no earlier run, an expired receipt) returns None and the
    lane runs in full.
    """
    runs = _api(f"repos/{repository}/actions/workflows/{workflow}/runs?status=success&per_page=10", token)
    for run in runs.get("workflow_runs", []):
        artifacts = _api(f"repos/{repository}/actions/runs/{run['id']}/artifacts", token).get("artifacts", [])
        receipt = [a for a in artifacts if a.get("name") == RECEIPT_ARTIFACT and not a.get("expired")]
        if not receipt:
            continue
        payload = _api(f"repos/{repository}/actions/artifacts/{receipt[0]['id']}/zip", token, raw=True)
        with zipfile.ZipFile(io.BytesIO(payload)) as bundle:
            if "receipt.json" not in bundle.namelist():
                continue
            return json.loads(bundle.read("receipt.json")).get("vellum_head_sha")
    return None


def resolve(args: argparse.Namespace) -> int:
    token = os.environ.get("GH_TOKEN", "")
    out = Path(args.out_dir)
    out.mkdir(parents=True, exist_ok=True)
    try:
        if args.run_id:
            run = _api(f"repos/{VELLUM_REPO}/actions/runs/{args.run_id}", token)
            if run.get("conclusion") != "success" or run.get("name") != GPU_WORKFLOW_NAME:
                raise ResolveError(f"run {args.run_id} is not a successful '{GPU_WORKFLOW_NAME}' run")
        else:
            listing = _api(f"repos/{VELLUM_REPO}/actions/runs?branch=main&event=push&status=success&per_page=50",
                           token)
            run = pick_run(listing.get("workflow_runs", []))
        head = run["head_sha"]
        previous = None
        if args.previous_receipt and Path(args.previous_receipt).is_file():
            previous = json.loads(Path(args.previous_receipt).read_text(encoding="utf-8")).get("vellum_head_sha")
        elif args.skip_if_unchanged:
            repository, workflow = args.skip_if_unchanged.split(":", 1)
            previous = previous_head(repository, workflow, token)
        if previous == head and not args.run_id:
            print(f"vellum-sdk-consumer: unchanged; Vellum main {head} was already checked")
            _write_outputs(args.github_output, {"unchanged": "true", "vellum_head_sha": head})
            return 0
        artifact = pick_artifact(_api(f"repos/{VELLUM_REPO}/actions/runs/{run['id']}/artifacts", token)
                                 .get("artifacts", []))
        payload = _api(f"repos/{VELLUM_REPO}/actions/artifacts/{artifact['id']}/zip", token, raw=True)
        evidence = out / "evidence"
        if evidence.exists():
            shutil.rmtree(evidence)
        evidence.mkdir()
        zip_path = out / "evidence.zip"
        zip_path.write_bytes(payload)
        with zipfile.ZipFile(zip_path) as bundle:
            for name in bundle.namelist():
                target = (evidence / name).resolve()
                if evidence.resolve() not in target.parents:
                    raise ResolveError(f"artifact member escapes the destination: {name}")
            bundle.extractall(evidence)
        archive, digest = verify_archive(evidence)
        unpacked = out / "unpacked"
        if unpacked.exists():
            shutil.rmtree(unpacked)
        unpacked.mkdir()
        _safe_extract(archive, unpacked)
        prefix = unpacked / "sdk"
        if not (prefix / "lib/cmake/Vellum/VellumConfig.cmake").is_file():
            raise ResolveError("the unpacked SDK has no lib/cmake/Vellum/VellumConfig.cmake")
    except ResolveError as error:
        print(f"vellum-sdk-consumer: resolve failed: {error}", file=sys.stderr)
        return INCONCLUSIVE
    receipt = {
        "schema": RECEIPT_SCHEMA,
        "vellum_repository": VELLUM_REPO,
        "vellum_run_id": run["id"],
        "vellum_run_url": run.get("html_url"),
        "vellum_head_sha": head,
        "artifact_id": artifact["id"],
        "archive": archive.name,
        "archive_sha256": digest,
        "prefix": str(prefix),
    }
    (out / "receipt.json").write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps(receipt, indent=2, sort_keys=True))
    _write_outputs(args.github_output, {"unchanged": "false", "vellum_head_sha": head, "prefix": str(prefix)})
    return 0


def _write_outputs(path: str | None, values: dict[str, str]) -> None:
    if path:
        with open(path, "a", encoding="utf-8") as handle:
            for key, value in values.items():
                handle.write(f"{key}={value}\n")


# --------------------------------------------------------------------------
# negative-control


@dataclass
class Compile:
    argv: list[str]


def syntax_only(command: str, source: str) -> Compile:
    """A ninja compile line rewritten as a syntax-only compile of `source`.

    Drops a leading `cmake -E env ...` and compiler launcher (ccache), the
    dependency-file and output flags, and replaces `-c` with -fsyntax-only.
    """
    argv = shlex.split(command)
    if len(argv) >= 3 and argv[0].endswith("cmake") and argv[1:3] == ["-E", "env"]:
        argv = argv[3:]
        while argv and "=" in argv[0] and not argv[0].startswith("-"):
            argv = argv[1:]
    if argv and Path(argv[0]).name in {"ccache", "sccache"}:
        argv = argv[1:]
    out: list[str] = []
    skip = False
    for i, arg in enumerate(argv):
        if skip:
            skip = False
            continue
        if arg in {"-o", "-MF", "-MT", "-MQ"}:
            skip = True
            continue
        if arg in {"-MD", "-MMD"}:
            continue
        if arg == "-c":
            out.append("-fsyntax-only")
            continue
        out.append(arg)
    if not any(a.endswith(source) for a in out):
        raise ValueError(f"the compile command does not compile {source}")
    if "-fsyntax-only" not in out:
        raise ValueError("the compile command has no -c to replace")
    return Compile(out)


def substitute(compile_: Compile, old: str, new: str) -> Compile:
    replaced = []
    hits = 0
    for arg in compile_.argv:
        if arg == old:
            replaced.append(new)
            hits += 1
        elif arg.startswith(("-I", "-isystem")) and arg.endswith(old) and arg not in ("-I", "-isystem"):
            replaced.append(arg[: len(arg) - len(old)] + new)
            hits += 1
        else:
            replaced.append(arg)
    if hits == 0:
        raise ValueError(f"the compile command never includes {old}")
    return Compile(replaced)


def judge(clean_rc: int, broken_rc: int, broken_stderr: str, header: str) -> tuple[int, str]:
    """Verdict on the clean and planted-break compiles."""
    if clean_rc != 0:
        return INCONCLUSIVE, "the unmodified SDK does not compile; the control cannot judge anything"
    if broken_rc == 0:
        return NOT_CONFIRMED, f"the compile still succeeds with {header} renamed; the lane cannot see this break"
    leaf = "/".join(Path(header).parts[1:]) or header
    if f"{leaf}' file not found" not in broken_stderr:
        return INCONCLUSIVE, f"the planted-break compile failed, but not on a missing {leaf}"
    return CONFIRMED, f"renaming {header} fails the compile with '{leaf}' file not found"


def negative_control(args: argparse.Namespace) -> int:
    build = Path(args.build_dir)
    mirror_include = Path(args.prefix) / "mirror" / "include"
    header = args.header
    if not (mirror_include / header).is_file():
        print(f"negative-control: INCONCLUSIVE: {mirror_include / header} does not exist", file=sys.stderr)
        return INCONCLUSIVE
    commands = subprocess.run(["ninja", "-C", str(build), "-t", "commands", args.target],
                              capture_output=True, text=True, encoding="utf-8")
    lines = [line for line in commands.stdout.splitlines() if args.source in line and " -c " in f" {line} "]
    if commands.returncode != 0 or len(lines) != 1:
        print(f"negative-control: INCONCLUSIVE: expected one compile of {args.source} in {args.target}, "
              f"found {len(lines)}", file=sys.stderr)
        return INCONCLUSIVE
    try:
        clean = syntax_only(lines[0], args.source)
        with tempfile.TemporaryDirectory(prefix="vellum-sdk-negative-") as scratch:
            broken_include = Path(scratch) / "include"
            shutil.copytree(mirror_include, broken_include, symlinks=True)
            renamed = broken_include / header
            renamed.rename(renamed.with_name(renamed.name + ".renamed-by-negative-control"))
            broken = substitute(clean, str(mirror_include), str(broken_include))
            clean_run = subprocess.run(clean.argv, cwd=build, capture_output=True, text=True, encoding="utf-8")
            broken_run = subprocess.run(broken.argv, cwd=build, capture_output=True, text=True, encoding="utf-8")
    except ValueError as error:
        print(f"negative-control: INCONCLUSIVE: {error}", file=sys.stderr)
        return INCONCLUSIVE
    verdict, reason = judge(clean_run.returncode, broken_run.returncode, broken_run.stderr, header)
    name = {CONFIRMED: "CONFIRMED", NOT_CONFIRMED: "NOT CONFIRMED", INCONCLUSIVE: "INCONCLUSIVE"}[verdict]
    print(f"negative-control: {name}: {reason}")
    if verdict != CONFIRMED:
        print("--- clean compile stderr ---\n" + clean_run.stderr[-4000:], file=sys.stderr)
        print("--- planted-break compile stderr ---\n" + broken_run.stderr[-4000:], file=sys.stderr)
    else:
        first = next((l for l in broken_run.stderr.splitlines() if "file not found" in l), "")
        print(f"negative-control: compiler said: {first.strip()}")
    return verdict


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    r = sub.add_parser("resolve", help="download and verify Vellum main's SDK archive")
    r.add_argument("--out-dir", required=True)
    r.add_argument("--run-id", help="a specific Vellum GPU proof run instead of the newest on main")
    r.add_argument("--previous-receipt", help="receipt of the last successful run; report unchanged if its head matches")
    r.add_argument("--skip-if-unchanged", metavar="OWNER/REPO:WORKFLOW_FILE",
                   help="report unchanged when the last successful run of this workflow checked the same head")
    r.add_argument("--github-output", default=os.environ.get("GITHUB_OUTPUT"))
    n = sub.add_parser("negative-control", help="prove a renamed SDK header fails one consumer compile")
    n.add_argument("--build-dir", required=True)
    n.add_argument("--prefix", required=True, help="the unpacked SDK's sdk/ directory")
    n.add_argument("--target", default="pulp-slice-vellum-ui")
    n.add_argument("--source", default="test/editor_host_slice/vellum_ui/knob_panel.cpp")
    n.add_argument("--header", default="vellum/canvas/canvas.hpp")
    args = parser.parse_args(argv)
    return resolve(args) if args.command == "resolve" else negative_control(args)


if __name__ == "__main__":
    sys.exit(main())
