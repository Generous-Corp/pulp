#!/usr/bin/env python3
"""Recognize the release bot's exact generated version-bump transaction.

This is a fail-closed CI optimization, not a correctness gate.  Exit zero only
when the candidate commit is byte-for-byte reproducible from the authenticated
protected base by the base branch's version-at-land writer.  Every missing,
stale, mixed, or otherwise ambiguous input exits one so callers retain the
ordinary full validation path.

The workflow must execute this file from the protected base, never from the PR
checkout.  The script intentionally keeps GitHub authentication out of PR code:
only this trusted program invokes ``gh api``.
"""

from __future__ import annotations

import argparse
import importlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from typing import Any


BUMP_BRANCH = "release/version-bump"
BUMP_SUBJECT = "chore: bump versions"
BOT_NAME = "pulp-release-bot"
BOT_EMAIL = "25807+danielraffel@users.noreply.github.com"
BOT_LOGIN = "danielraffel"
SHA_RE = re.compile(r"^[0-9a-f]{40}$")
TRUSTED_WRITER_ROOT = "tools/scripts"
TRUSTED_WRITER_PATHS = (
    "tools/scripts/version_at_land.py",
    "tools/scripts/version_bump_apply.py",
    "tools/scripts/version_bump_heuristics.py",
    "tools/scripts/version_bump_surfaces.py",
    "tools/scripts/gate_common.py",
    "tools/scripts/versioning.json",
    # `_write_plan` executes this derived-file generator. Bind its executable
    # to the protected base below and pin its complete local import closure.
    "tools/scripts/pulp_tooling_disposition.py",
    "tools/scripts/cli_sync_check.py",
    "tools/scripts/cli_command_inventory.py",
)


class NotGeneratedBump(RuntimeError):
    """A normal full-validation disposition, never a workflow failure."""


def _run(
    *args: str,
    cwd: Path,
    check: bool = True,
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        list(args), cwd=cwd, text=True, capture_output=True, check=check
    )


def _git(repo: Path, *args: str, check: bool = True) -> subprocess.CompletedProcess[str]:
    return _run("git", *args, cwd=repo, check=check)


def _git_text(repo: Path, *args: str) -> str:
    return _git(repo, *args).stdout.strip()


def _load_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise NotGeneratedBump(f"unreadable JSON input {path}: {exc}") from exc


def _gh_json(repo_name: str, endpoint: str, *, paginate: bool = False) -> Any:
    args = ["gh", "api"]
    if paginate:
        args.extend(["--paginate", "--slurp"])
    args.append(f"repos/{repo_name}/{endpoint}")
    try:
        proc = subprocess.run(args, text=True, capture_output=True, check=True)
        return json.loads(proc.stdout)
    except (OSError, subprocess.CalledProcessError, json.JSONDecodeError) as exc:
        raise NotGeneratedBump(f"GitHub API lookup failed for {endpoint}") from exc


def _flatten_pages(payload: Any) -> list[Any]:
    if not isinstance(payload, list):
        raise NotGeneratedBump("paginated GitHub response was not an array")
    flattened: list[Any] = []
    for page in payload:
        if not isinstance(page, list):
            raise NotGeneratedBump("paginated GitHub response contained a non-array page")
        flattened.extend(page)
    return flattened


def _parents(repo: Path, commit: str) -> list[str]:
    # Read the commit object directly. `git show --format=%P` suppresses parents
    # for a commit recorded as a shallow boundary, even though the immutable
    # object still carries them; CI deliberately uses depth-one fetches here.
    raw = _git(repo, "cat-file", "-p", commit).stdout
    fields = [line.removeprefix("parent ") for line in raw.splitlines()
              if line.startswith("parent ")]
    if not all(SHA_RE.fullmatch(value) for value in fields):
        raise NotGeneratedBump(f"could not resolve parents for {commit}")
    return fields


def _fetch_commit(repo: Path, commit: str) -> None:
    if not SHA_RE.fullmatch(commit):
        raise NotGeneratedBump(f"invalid commit id: {commit!r}")
    if _git(repo, "cat-file", "-e", f"{commit}^{{commit}}", check=False).returncode == 0:
        return
    if _git(
        repo, "fetch", "--no-tags", "--depth=1", "origin", commit, check=False
    ).returncode != 0:
        raise NotGeneratedBump(f"could not fetch immutable commit {commit}")
    if _git(repo, "cat-file", "-e", f"{commit}^{{commit}}", check=False).returncode != 0:
        raise NotGeneratedBump(f"fetched object is not a commit: {commit}")


def _pull_shape(
    event_name: str,
    event: dict[str, Any],
    associated_pulls: list[Any],
    *,
    repo_name: str,
    candidate_base: str,
    candidate: str,
) -> None:
    if event_name == "pull_request":
        pull = event.get("pull_request")
        pulls = [pull]
    elif event_name == "merge_group":
        pulls = associated_pulls
    else:
        raise NotGeneratedBump(f"unsupported event: {event_name}")

    if len(pulls) != 1 or not isinstance(pulls[0], dict):
        raise NotGeneratedBump("candidate is not owned by exactly one pull request")
    pull = pulls[0]
    head = pull.get("head") or {}
    base_ref = pull.get("base") or {}
    head_repo = head.get("repo") or {}
    if pull.get("state") != "open":
        raise NotGeneratedBump("version-bump pull request is not open")
    if pull.get("title") != BUMP_SUBJECT:
        raise NotGeneratedBump("pull-request title is not the canonical bump subject")
    if head.get("ref") != BUMP_BRANCH or head.get("sha") != candidate:
        raise NotGeneratedBump("pull-request head ref/SHA is not the fixed bump transaction")
    if head_repo.get("full_name") != repo_name:
        raise NotGeneratedBump("version-bump branch is not in the protected repository")
    if base_ref.get("sha") != candidate_base or base_ref.get("ref") != "main":
        raise NotGeneratedBump("pull-request base is not the immutable protected main SHA")
    if pull.get("commits") != 1:
        raise NotGeneratedBump("version-bump pull request does not contain exactly one commit")


def _commit_provenance(
    repo: Path,
    metadata: dict[str, Any],
    *,
    candidate_base: str,
    candidate: str,
) -> None:
    if metadata.get("sha") != candidate:
        raise NotGeneratedBump("commit API response does not bind the candidate SHA")
    commit = metadata.get("commit") or {}
    verification = commit.get("verification") or {}
    author = commit.get("author") or {}
    committer = commit.get("committer") or {}
    api_author = metadata.get("author") or {}
    api_committer = metadata.get("committer") or {}
    if verification.get("verified") is not True or verification.get("reason") != "valid":
        raise NotGeneratedBump("release-bot signature is absent or not valid")
    if not str(verification.get("signature") or "").startswith("-----BEGIN SSH SIGNATURE-----"):
        raise NotGeneratedBump("release-bot commit is not SSH-signed")
    if author.get("name") != BOT_NAME or committer.get("name") != BOT_NAME:
        raise NotGeneratedBump("commit author/committer name is not the release bot")
    if author.get("email") != BOT_EMAIL or committer.get("email") != BOT_EMAIL:
        raise NotGeneratedBump("commit author/committer email is not the release bot")
    if api_author.get("login") != BOT_LOGIN or api_committer.get("login") != BOT_LOGIN:
        raise NotGeneratedBump("verified signature is not attributed to the release-bot account")
    if _parents(repo, candidate) != [candidate_base]:
        raise NotGeneratedBump("candidate is not a one-commit fast-forward from protected main")


def _event_commits(
    repo: Path,
    event_name: str,
    event: dict[str, Any],
    *,
    explicit_base: str,
    explicit_head: str,
) -> tuple[str, str, str, str | None]:
    repository = event.get("repository") or {}
    repo_name = repository.get("full_name")
    if not isinstance(repo_name, str) or not repo_name:
        raise NotGeneratedBump("event omitted repository.full_name")

    if event_name == "pull_request":
        pull = event.get("pull_request") or {}
        event_base = ((pull.get("base") or {}).get("sha"))
        candidate = ((pull.get("head") or {}).get("sha"))
        if explicit_head != candidate:
            # pull_request workflows normally check out a synthetic merge commit,
            # but GITHUB_SHA is not authority for the source branch transaction.
            merge_sha = event.get("after")
            if explicit_head not in {
                event.get("pull_request", {}).get("merge_commit_sha"),
                merge_sha,
            }:
                raise NotGeneratedBump("workflow head is unrelated to the pull request event")
        candidate_base = event_base
        validation_head = None
    elif event_name == "merge_group":
        group = event.get("merge_group") or {}
        event_base = group.get("base_sha")
        if group.get("head_sha") != explicit_head:
            raise NotGeneratedBump("workflow head does not match the merge-group event")
        _fetch_commit(repo, explicit_head)
        group_parents = _parents(repo, explicit_head)
        if len(group_parents) != 2 or group_parents[0] != event_base:
            raise NotGeneratedBump("merge group is mixed, nested, or has unknown topology")
        candidate = group_parents[1]
        _fetch_commit(repo, candidate)
        candidate_parents = _parents(repo, candidate)
        if len(candidate_parents) != 1:
            raise NotGeneratedBump("generated candidate is not a single-parent commit")
        candidate_base = candidate_parents[0]
        _fetch_commit(repo, candidate_base)
        if candidate_base != event_base:
            _fetch_commit(repo, event_base)
            cumulative_parents = _parents(repo, event_base)
            if len(cumulative_parents) != 2 or cumulative_parents[0] != candidate_base:
                raise NotGeneratedBump(
                    "cumulative base is not one exact prior group on candidate protected main"
                )
        validation_head = explicit_head
    else:
        raise NotGeneratedBump(f"unsupported event: {event_name}")

    if explicit_base != event_base or not isinstance(candidate, str):
        raise NotGeneratedBump("explicit commits do not match immutable event commits")
    if not SHA_RE.fullmatch(explicit_base) or not SHA_RE.fullmatch(candidate):
        raise NotGeneratedBump("event contains a malformed commit id")
    return repo_name, candidate, candidate_base, validation_head


def _message_plan(repo: Path, candidate: str, base: str, assignments: list[Any]) -> None:
    message = _git(repo, "show", "-s", "--format=%B", candidate).stdout.rstrip("\n")
    summary = "; ".join(
        f"{item.surface} {item.current}->{item.assigned} ({item.level})"
        for item in assignments
    )
    pattern = re.compile(
        rf"^{re.escape(BUMP_SUBJECT)}\n\n"
        rf"Assigned from Version-Bump intent on [0-9a-f]{{12}}\.\.{base[:12]}\.\n"
        rf"{re.escape(summary)}\n\n"
        rf"Version-Bump-Applied: {base}$"
    )
    if pattern.fullmatch(message) is None:
        raise NotGeneratedBump("commit message/assignment marker does not match the exact plan")


def _candidate_version(repo: Path, module: Any, candidate: str, version_file: Any) -> str | None:
    shown = _git(repo, "show", f"{candidate}:{version_file.path}", check=False)
    if shown.returncode != 0:
        return None
    return module._extract_version_from_text(shown.stdout, version_file)


def _path_projection(
    repo: Path,
    *,
    base: str,
    observed: str,
    edited: list[str],
) -> None:
    statuses = _git(
        repo, "diff", "--name-status", "--no-renames", "-z", base, observed
    ).stdout
    fields = statuses.split("\0")
    if fields and fields[-1] == "":
        fields.pop()
    if len(fields) % 2 != 0:
        raise NotGeneratedBump("candidate path status stream is malformed")
    actual_paths: list[str] = []
    for index in range(0, len(fields), 2):
        status, path = fields[index], fields[index + 1]
        if status != "M" or not path:
            raise NotGeneratedBump("candidate contains a rename/add/delete/type change")
        actual_paths.append(path)
    if set(actual_paths) != set(edited) or len(actual_paths) != len(set(actual_paths)):
        raise NotGeneratedBump("candidate path set differs from trusted generated edits")


def _assert_writer_unchanged(repo: Path, older_base: str, cumulative_base: str) -> None:
    for path in TRUSTED_WRITER_PATHS:
        older = _git_text(repo, "rev-parse", f"{older_base}:{path}")
        cumulative = _git_text(repo, "rev-parse", f"{cumulative_base}:{path}")
        if older != cumulative:
            raise NotGeneratedBump(
                f"trusted version writer changed ahead of the generated candidate: {path}"
            )


def _bind_trusted_regenerators(module: Any, worktree: Path) -> None:
    """Make every derived-file subprocess execute from the trusted base tree."""
    scripts = (worktree / TRUSTED_WRITER_ROOT).resolve()
    bound: list[tuple[str, list[str]]] = []
    for output, command in module._DERIVED_REGENERATORS:
        if len(command) < 2 or command[0] != "python3":
            raise NotGeneratedBump("trusted derived regenerator has an unknown command shape")
        relative = Path(command[1])
        if relative.as_posix() not in TRUSTED_WRITER_PATHS:
            raise NotGeneratedBump("trusted derived regenerator is not pinned")
        trusted_script = (worktree / relative).resolve()
        try:
            trusted_script.relative_to(scripts)
        except ValueError as exc:
            raise NotGeneratedBump(
                "trusted derived regenerator escapes tools/scripts"
            ) from exc
        if not trusted_script.is_file():
            raise NotGeneratedBump("trusted derived regenerator script is missing")
        bound.append((output, [sys.executable, str(trusted_script), *command[2:]]))
    module._DERIVED_REGENERATORS = bound


def _reproduce_tree(
    repo: Path,
    *,
    base: str,
    candidate: str,
    cumulative_base: str | None = None,
    cumulative_head: str | None = None,
) -> list[Any]:
    runner_temp = Path(os.environ.get("RUNNER_TEMP") or tempfile.gettempdir())
    runner_temp.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="pulp-version-bump-proof-", dir=runner_temp) as holder:
        worktree = Path(holder) / "base"
        added = False
        try:
            _git(repo, "worktree", "add", "--detach", str(worktree), base)
            added = True
            scripts = worktree / "tools" / "scripts"
            sys.path.insert(0, str(scripts))
            version_at_land = importlib.import_module("version_at_land")
            surfaces = importlib.import_module("version_bump_surfaces")
            _bind_trusted_regenerators(version_at_land, worktree)
            config = surfaces.load_config(scripts / "versioning.json")
            assignments: list[Any] = []
            for surface in config.surfaces:
                base_versions = [
                    surfaces.read_version(worktree, vf) for vf in surface.version_files
                ]
                candidate_versions = [
                    _candidate_version(repo, surfaces, candidate, vf)
                    for vf in surface.version_files
                ]
                if any(value is None for value in base_versions + candidate_versions):
                    raise NotGeneratedBump(f"unreadable version file on surface {surface.name}")
                if len(set(base_versions)) != 1 or len(set(candidate_versions)) != 1:
                    raise NotGeneratedBump(f"version files drift within surface {surface.name}")
                current = str(base_versions[0])
                assigned = str(candidate_versions[0])
                if current == assigned:
                    continue
                levels = [
                    level
                    for level in ("patch", "minor", "major")
                    if version_at_land.bump_version(current, level) == assigned
                ]
                if len(levels) != 1:
                    raise NotGeneratedBump(
                        f"surface {surface.name} is not one strict semantic-version increment"
                    )
                assignments.append(
                    version_at_land.Assignment(
                        surface.name, levels[0], current, assigned
                    )
                )
            if not assignments:
                raise NotGeneratedBump("candidate changes no configured version surface")

            edited = version_at_land._write_plan(worktree, config, assignments)
            if not edited:
                raise NotGeneratedBump("trusted version writer reproduced no edits")
            _git(worktree, "add", "--", *dict.fromkeys(edited))
            expected_tree = _git_text(worktree, "write-tree")
            actual_tree = _git_text(repo, "rev-parse", f"{candidate}^{{tree}}")
            if expected_tree != actual_tree:
                raise NotGeneratedBump("candidate tree differs from trusted byte regeneration")
            _path_projection(repo, base=base, observed=candidate, edited=edited)
            _message_plan(repo, candidate, base, assignments)

            if cumulative_head is not None:
                if cumulative_base is None:
                    raise NotGeneratedBump("cumulative head omitted its immutable base")
                _assert_writer_unchanged(repo, base, cumulative_base)
                cumulative_worktree = Path(holder) / "cumulative"
                cumulative_added = False
                try:
                    _git(
                        repo,
                        "worktree",
                        "add",
                        "--detach",
                        str(cumulative_worktree),
                        cumulative_base,
                    )
                    cumulative_added = True
                    by_name = {surface.name: surface for surface in config.surfaces}
                    for assignment in assignments:
                        surface = by_name.get(assignment.surface)
                        if surface is None:
                            raise NotGeneratedBump(
                                f"cumulative base omitted surface {assignment.surface}"
                            )
                        versions = [
                            surfaces.read_version(cumulative_worktree, vf)
                            for vf in surface.version_files
                        ]
                        if any(value is None for value in versions) or set(versions) != {
                            assignment.current
                        }:
                            raise NotGeneratedBump(
                                f"cumulative base changed surface {assignment.surface}"
                            )
                    cumulative_edited = version_at_land._write_plan(
                        cumulative_worktree, config, assignments
                    )
                    if not cumulative_edited:
                        raise NotGeneratedBump("cumulative projection reproduced no edits")
                    _git(
                        cumulative_worktree,
                        "add",
                        "--",
                        *dict.fromkeys(cumulative_edited),
                    )
                    expected_cumulative_tree = _git_text(
                        cumulative_worktree, "write-tree"
                    )
                    actual_cumulative_tree = _git_text(
                        repo, "rev-parse", f"{cumulative_head}^{{tree}}"
                    )
                    if expected_cumulative_tree != actual_cumulative_tree:
                        raise NotGeneratedBump(
                            "merge-group tree differs from the exact cumulative projection"
                        )
                    _path_projection(
                        repo,
                        base=cumulative_base,
                        observed=cumulative_head,
                        edited=cumulative_edited,
                    )
                finally:
                    if cumulative_added:
                        _git(
                            repo,
                            "worktree",
                            "remove",
                            "--force",
                            str(cumulative_worktree),
                            check=False,
                        )
            return assignments
        finally:
            if added:
                _git(repo, "worktree", "remove", "--force", str(worktree), check=False)


def verify(args: argparse.Namespace) -> dict[str, Any]:
    repo = args.repo.resolve()
    event = _load_json(args.event_path)
    if not isinstance(event, dict):
        raise NotGeneratedBump("event payload is not an object")
    repo_name, candidate, candidate_base, cumulative_head = _event_commits(
        repo,
        args.event_name,
        event,
        explicit_base=args.base,
        explicit_head=args.head,
    )
    _fetch_commit(repo, args.base)
    _fetch_commit(repo, candidate)

    metadata = (
        _load_json(args.commit_json)
        if args.commit_json
        else _gh_json(repo_name, f"commits/{candidate}")
    )
    if not isinstance(metadata, dict):
        raise NotGeneratedBump("commit metadata response is not an object")
    associated = []
    if args.event_name == "merge_group":
        pages = (
            _load_json(args.pulls_json)
            if args.pulls_json
            else _gh_json(repo_name, f"commits/{candidate}/pulls?per_page=100", paginate=True)
        )
        summaries = _flatten_pages(pages)
        if len(summaries) != 1 or not isinstance(summaries[0], dict):
            raise NotGeneratedBump(
                "candidate is not owned by exactly one pull request"
            )
        pull_number = summaries[0].get("number")
        if not isinstance(pull_number, int) or pull_number <= 0:
            raise NotGeneratedBump("associated pull response omitted its number")
        detail = (
            _load_json(args.pull_json)
            if args.pull_json
            else _gh_json(repo_name, f"pulls/{pull_number}")
        )
        if not isinstance(detail, dict) or detail.get("number") != pull_number:
            raise NotGeneratedBump("detailed pull response did not bind its summary")
        associated = [detail]

    _pull_shape(
        args.event_name,
        event,
        associated,
        repo_name=repo_name,
        candidate_base=candidate_base,
        candidate=candidate,
    )
    _commit_provenance(
        repo, metadata, candidate_base=candidate_base, candidate=candidate
    )
    assignments = _reproduce_tree(
        repo,
        base=candidate_base,
        candidate=candidate,
        cumulative_base=args.base if cumulative_head is not None else None,
        cumulative_head=cumulative_head,
    )
    return {
        "generated_version_bump": True,
        "base": args.base,
        "candidate": candidate,
        "surfaces": [item.surface for item in assignments],
    }


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path.cwd())
    parser.add_argument("--event-name", required=True)
    parser.add_argument("--event-path", type=Path, required=True)
    parser.add_argument("--base", required=True)
    parser.add_argument("--head", required=True)
    parser.add_argument("--commit-json", type=Path)
    parser.add_argument("--pulls-json", type=Path)
    parser.add_argument("--pull-json", type=Path)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)
    try:
        result = verify(args)
    except (NotGeneratedBump, OSError, subprocess.SubprocessError, ValueError) as exc:
        result = {"generated_version_bump": False, "reason": str(exc)}
        if args.json:
            print(json.dumps(result, sort_keys=True))
        else:
            print(f"generated-version-bump: full validation ({exc})", file=sys.stderr)
        return 1
    if args.json:
        print(json.dumps(result, sort_keys=True))
    else:
        print(
            "generated-version-bump: exact trusted transaction "
            f"({','.join(result['surfaces'])})",
            file=sys.stderr,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
