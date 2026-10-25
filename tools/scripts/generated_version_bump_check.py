#!/usr/bin/env python3
"""Recognize the release bot's exact generated version-bump transaction.

This is a fail-closed CI optimization, not a correctness gate.  Exit zero only
when the candidate commit is byte-for-byte reproducible from the authenticated
protected base by the base branch's version-at-land writer.  Every missing,
stale, mixed, or otherwise ambiguous input exits one so callers retain the
ordinary full validation path.

release-please's release PRs (`release-please--branches--main*`) take the
same path through a second recognizer, `_verify_release_pr`: the PR must be
release-please's own (branch, title, `autorelease: pending` label, commits),
release-please's own commit may change only its surface's version strings (each changed JSON
leaf or text line must be exactly the old version becoming the new one), and
an SDK release PR's derived-file commit must be byte-reproducible by the
protected base's `release_derived_refresh.py`. In a merge group the PR's
changes must overlay paths main has not touched since the PR's base, so the
group tree is exactly main plus the proven edits.

The workflow must execute this file from the protected base, never from the PR
checkout.  The script intentionally keeps GitHub authentication out of PR code:
only this trusted program invokes ``gh api``.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
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
    "tools/scripts/gpu_handoff_provenance.py",
    # `_write_plan` executes this derived-file generator. Bind its executable
    # to the protected base below and pin its complete local import closure.
    "tools/scripts/pulp_tooling_disposition.py",
    "tools/scripts/gpu_handoff_provenance.py",
    "tools/scripts/cli_sync_check.py",
    "tools/scripts/cli_command_inventory.py",
)

# release-please's release PRs. The branch names and title are release-please's
# defaults for the two configurations in .github/release-please/; the refresh
# commit is the one release-please.yml pushes on the SDK branch.
RELEASE_PR_BRANCHES = {
    "release-please--branches--main": "sdk",
    "release-please--branches--main--components--plugin": "plugin",
}
RELEASE_PR_TITLE = re.compile(
    r"^chore\(main\): release (?P<plugin>plugin )?(?P<version>[0-9]+\.[0-9]+\.[0-9]+)$", re.ASCII
)
# Canonical semantic version: ASCII digits, no leading zeros.
SEMVER = re.compile(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)", re.ASCII)
GENERIC_MARKER = "x-release-please-version"
# The release bot's SSH signing key, pinned (read from the protected base).
SIGNER_CONFIG = "tools/scripts/release_bot_signer.json"
RELEASE_PR_LABEL = "autorelease: pending"
REFRESH_SUBJECT = "chore: refresh release-derived files"
REFRESH_MARKER = "Release-Derived-Refresh:"
# Owned by the plugin release PR's release-please `extra-files`.
PLUGIN_DISPOSITION = "docs/status/pulp-tooling-disposition.json"
# What the SDK refresh commit may touch; its bytes are then reproduced exactly.
REFRESH_PATH_PREFIXES = (
    "CHANGELOG.md",
    "docs/status/gpu-vellum-handoff.yaml",
    "docs/validation/gpu-handoff-provenance/receipt.json",
    "docs/migrations/",
)


# Derived files the bump renders with a tool this verifier does not run: the
# changelog comes from `shipyard changelog regenerate` over the tag graph. The
# build and tests never read it, so its bytes are taken from the candidate
# instead of reproduced, and the rest of the tree must still match exactly.
UNREPRODUCED_DERIVED = ("CHANGELOG.md",)


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


def _prove_pr_merge_ref(
    repo: Path,
    *,
    merge_head: str,
    event_base: Any,
    candidate: Any,
) -> None:
    """Bind a checked-out ``refs/pull/N/merge`` commit to the event's own SHAs.

    A freshly opened pull request's payload often carries a null or stale
    ``merge_commit_sha`` and no ``after`` field, so ``GITHUB_SHA`` (the merge
    ref GitHub synthesized for this run) cannot be matched by value. Accept it
    only when the local, immutable commit object proves it IS the merge of the
    event's base and head: exactly ``[base.sha, head.sha]`` as parents, in that
    order, and the working checkout's HEAD. Its tree must equal the head's
    tree; ``_commit_provenance`` separately requires the head's sole parent to
    be ``base.sha``, so the only faithful merge is the head tree itself and a
    merge commit carrying any extra byte is refused here.
    """
    for value in (merge_head, event_base, candidate):
        if not isinstance(value, str) or not SHA_RE.fullmatch(value):
            raise NotGeneratedBump("pull-request merge ref proof has a malformed commit id")
    checked_out = _git(
        repo, "rev-parse", "--verify", "--quiet", "HEAD^{commit}", check=False
    ).stdout.strip()
    if checked_out != merge_head:
        raise NotGeneratedBump("workflow head is not the checked-out pull-request merge ref")
    if _parents(repo, merge_head) != [event_base, candidate]:
        raise NotGeneratedBump(
            "pull-request merge ref parents are not exactly [base.sha, head.sha]"
        )
    _fetch_commit(repo, candidate)
    merge_tree = _git_text(repo, "rev-parse", f"{merge_head}^{{tree}}")
    head_tree = _git_text(repo, "rev-parse", f"{candidate}^{{tree}}")
    if merge_tree != head_tree:
        raise NotGeneratedBump("pull-request merge ref tree differs from the head tree")


def _event_commits(
    repo: Path,
    event_name: str,
    event: dict[str, Any],
    *,
    explicit_base: str,
    explicit_head: str,
    accept_pr_merge_ref: bool = False,
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
                if not accept_pr_merge_ref:
                    raise NotGeneratedBump(
                        "workflow head is unrelated to the pull request event"
                    )
                _prove_pr_merge_ref(
                    repo,
                    merge_head=explicit_head,
                    event_base=event_base,
                    candidate=candidate,
                )
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


def _complete_history(repo: Path, commits: list[str]) -> None:
    """Give the regenerators the full commit graph behind every trusted base.

    The GPU handoff regenerator pins each ledger row to the last commit that
    touched its path, which is a question about history. The classify job
    checks out one commit deep, so in that checkout every path's last owner is
    the shallow graft boundary and the regenerator refuses, leaving the ledger
    pair at the base's bytes. The bot regenerates from a complete clone, so
    any bump that re-pinned the pair could never be reproduced here and fell
    back to full validation every time.

    Deepen in place, blobless: commits and trees are all a per-path `git log`
    reads, and a missing blob is fetched on demand. Only immutable ids are
    fetched, never a branch name. A repository still shallow afterwards means
    reproduction would be measuring a truncated graph, so it refuses.
    """
    for commit in commits:
        if not SHA_RE.fullmatch(commit):
            raise NotGeneratedBump(f"invalid commit id: {commit!r}")
    if _git_text(repo, "rev-parse", "--is-shallow-repository") != "true":
        return
    detail = ""
    for _attempt in range(2):
        fetched = _git(
            repo,
            "fetch",
            "--no-tags",
            "--unshallow",
            "--filter=blob:none",
            "origin",
            *dict.fromkeys(commits),
            check=False,
        )
        if _git_text(repo, "rev-parse", "--is-shallow-repository") != "true":
            return
        lines = (fetched.stderr or fetched.stdout).strip().splitlines()
        detail = lines[-1] if lines else f"git fetch exited {fetched.returncode}"
    raise NotGeneratedBump(
        "could not deepen the shallow checkout to complete history, so the "
        f"history-derived GPU handoff ledger cannot be regenerated: {detail}"
    )


def _bind_trusted_regenerators(module: Any, worktree: Path) -> None:
    """Make every derived-file subprocess execute from the trusted base tree."""
    scripts = (worktree / TRUSTED_WRITER_ROOT).resolve()
    bound: list[tuple[str, list[str]]] = []
    for output, command in module._DERIVED_REGENERATORS:
        if tuple(output) and set(output) <= set(UNREPRODUCED_DERIVED):
            continue
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


def _adopt_unreproduced(
    repo: Path, worktree: Path, *, base: str, observed: str, edited: list[str]
) -> list[str]:
    """Copy the observed bytes of each unreproduced derived file it changed."""
    adopted = list(edited)
    for path in UNREPRODUCED_DERIVED:
        changed = _git(repo, "diff", "--quiet", base, observed, "--", path, check=False)
        if changed.returncode == 0:
            continue
        if changed.returncode != 1:
            raise NotGeneratedBump(f"could not compare {path} against the base")
        blob = subprocess.run(
            ["git", "cat-file", "blob", f"{observed}:{path}"],
            cwd=repo,
            capture_output=True,
            check=False,
        )
        if blob.returncode != 0:
            raise NotGeneratedBump(f"candidate {path} is not a readable blob")
        (worktree / path).write_bytes(blob.stdout)
        if path not in adopted:
            adopted.append(path)
    return adopted


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
            edited = _adopt_unreproduced(
                repo, worktree, base=base, observed=candidate, edited=edited
            )
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
                if cumulative_base == base:
                    # The group is the bump alone on its own base, so the
                    # exact cumulative projection IS the projection just
                    # proven. Regenerating it again in a second worktree would
                    # not be: the trusted scripts are bound to the base
                    # worktree, and a regenerator that anchors its root to its
                    # own location (gpu_handoff_provenance.py does) writes the
                    # first worktree and leaves the second stale.
                    if _git_text(repo, "rev-parse", f"{cumulative_head}^{{tree}}") != (
                        expected_tree
                    ):
                        raise NotGeneratedBump(
                            "merge-group tree differs from the exact cumulative projection"
                        )
                    _path_projection(
                        repo, base=base, observed=cumulative_head, edited=edited
                    )
                    return assignments
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
                    cumulative_edited = _adopt_unreproduced(
                        repo,
                        cumulative_worktree,
                        base=cumulative_base,
                        observed=cumulative_head,
                        edited=cumulative_edited,
                    )
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


# ── release-please release PRs ──────────────────────────────────────────────


def _subject(repo: Path, commit: str) -> str:
    return _git_text(repo, "show", "-s", "--format=%s", commit)


def _single_parent(repo: Path, commit: str) -> str:
    parents = _parents(repo, commit)
    if len(parents) != 1:
        raise NotGeneratedBump(f"release PR commit {commit[:12]} is not single-parent")
    _fetch_commit(repo, parents[0])
    return parents[0]


def _release_chain(repo: Path, head: str) -> tuple[str, str | None, str]:
    """(release-please commit, refresh commit or None, PR base) behind `head`."""
    _fetch_commit(repo, head)
    refresh = None
    release = head
    if _subject(repo, head) == REFRESH_SUBJECT:
        refresh = head
        release = _single_parent(repo, head)
    base = _single_parent(repo, release)
    return release, refresh, base


def _blob(repo: Path, rev: str, path: str) -> str | None:
    shown = _git(repo, "rev-parse", "--verify", "--quiet", f"{rev}:{path}", check=False)
    return shown.stdout.strip() or None


def _changed(repo: Path, old: str, new: str) -> dict[str, str]:
    fields = _git(repo, "diff", "--name-status", "--no-renames", "-z", old, new).stdout.split("\0")
    if fields and fields[-1] == "":
        fields.pop()
    if len(fields) % 2:
        raise NotGeneratedBump("release PR path status stream is malformed")
    return {fields[i + 1]: fields[i] for i in range(0, len(fields), 2)}


def _semver(version: Any) -> tuple[int, int, int]:
    match = SEMVER.fullmatch(version) if isinstance(version, str) else None
    if match is None:
        raise NotGeneratedBump(f"{version!r} is not a canonical x.y.z version")
    return tuple(int(part) for part in match.groups())  # type: ignore[return-value]


def _one_step(old: str, new: str, *, breaking: bool) -> None:
    """`new` is exactly one semantic-version step from `old`; a major step
    only when a counted commit records a breaking change."""
    major, minor, patch = _semver(old)
    step = _semver(new)
    if step not in {(major, minor, patch + 1), (major, minor + 1, 0), (major + 1, 0, 0)}:
        raise NotGeneratedBump(f"{old} -> {new} is not one semantic-version step")
    if step == (major + 1, 0, 0) and not breaking:
        raise NotGeneratedBump(f"{old} -> {new} is a major step with no breaking change recorded")


_BREAKING_HEADER = re.compile(r"^[A-Za-z]+(\([^)]*\))?!: ", re.ASCII)
_BREAKING_FOOTER = re.compile(r"^BREAKING[ -]CHANGE: ", re.ASCII | re.MULTILINE)


def _tag_for(rp_config: dict, version: str) -> str:
    """The tag release-please names for `version` under this configuration."""
    package = (rp_config.get("packages") or {}).get(".") or {}

    def option(key: str, default: Any) -> Any:
        return package.get(key, rp_config.get(key, default))

    prefix = ""
    if option("include-component-in-tag", True) and option("component", ""):
        prefix = option("component", "") + option("tag-separator", "-")
    return prefix + ("v" if option("include-v-in-tag", True) else "") + version


def _breaking_since(repo: Path, tag: str, base: str, exclude_paths: list[str]) -> bool:
    """Whether a commit release-please counted since `tag` records a break.

    The commits are every commit in `tag..base` whose files are not all under
    `exclude-paths` (release-please's own rule). A break is a `<type>!:`
    header on a commit, or a `BREAKING CHANGE:` footer. A merge commit counts
    only by footer: release-please reads a merged PR title from its body
    split on `<type>(<scope>): `, which a `!:` title does not match."""
    found = _git(repo, "rev-parse", "--verify", "--quiet", f"refs/tags/{tag}^{{commit}}",
                 check=False).stdout.strip()
    if not found:
        _git(repo, "fetch", "--no-tags", "origin", f"+refs/tags/{tag}:refs/tags/{tag}",
             check=False)
        found = _git(repo, "rev-parse", "--verify", "--quiet", f"refs/tags/{tag}^{{commit}}",
                     check=False).stdout.strip()
    if not SHA_RE.fullmatch(found):
        raise NotGeneratedBump(f"cannot read the previous release tag {tag}")
    _complete_history(repo, [found, base])
    if _git(repo, "merge-base", "--is-ancestor", found, base, check=False).returncode != 0:
        raise NotGeneratedBump(f"{tag} is not an ancestor of the release PR's base")
    excluded = [path.strip("/") + "/" for path in exclude_paths]
    log = _git(repo, "log", "--format=%H%x00%P%x00%B%x01", f"{found}..{base}").stdout
    for entry in log.split("\x01"):
        fields = entry.strip("\n").split("\x00")
        if len(fields) != 3:
            continue
        sha, parents, message = fields
        merge = len(parents.split()) > 1
        marked = bool(_BREAKING_FOOTER.search(message)) or (
            not merge and bool(_BREAKING_HEADER.match(message)))
        if not marked:
            continue
        files = _git(repo, "diff", "--name-only", f"{sha}^1", sha).stdout.split()
        if any(not any(path.startswith(prefix) for prefix in excluded) for path in files):
            return True
    return False


def _strict_json(text: str, path: str) -> Any:
    def pairs(items: list[tuple[str, Any]]) -> dict[str, Any]:
        keys = [key for key, _value in items]
        if len(keys) != len(set(keys)):
            raise NotGeneratedBump(f"{path} has a duplicate JSON key")
        return dict(items)
    try:
        return json.loads(text, object_pairs_hook=pairs)
    except json.JSONDecodeError as exc:
        raise NotGeneratedBump(f"{path} is not JSON") from exc


_JSONPATH_STEP = re.compile(
    r"\.(?P<key>[A-Za-z_][A-Za-z0-9_]*)"
    r"|\[(?P<index>[0-9]+)\]"
    r'|\["(?P<qkey>[^"]*)"\]'
    r"|\[\?\(@\.(?P<fkey>[A-Za-z_][A-Za-z0-9_]*)=='(?P<fval>[^']*)'\)\]",
    re.ASCII,
)


def _resolve_jsonpath(document: Any, jsonpath: str) -> list[tuple[Any, ...]]:
    """Concrete locations for the JSONPath subset the release-please configs use."""
    if not jsonpath.startswith("$"):
        raise NotGeneratedBump(f"unsupported jsonpath {jsonpath!r}")
    locations: list[tuple[Any, ...]] = [()]
    rest = jsonpath[1:]
    while rest:
        step = _JSONPATH_STEP.match(rest)
        if step is None:
            raise NotGeneratedBump(f"unsupported jsonpath {jsonpath!r}")
        rest = rest[step.end():]
        found: list[tuple[Any, ...]] = []
        for location in locations:
            node = _json_at(document, location)
            key = step.group("key") if step.group("key") is not None else step.group("qkey")
            if key is not None and isinstance(node, dict) and key in node:
                found.append(location + (key,))
            elif step.group("index") is not None and isinstance(node, list) \
                    and int(step.group("index")) < len(node):
                found.append(location + (int(step.group("index")),))
            elif step.group("fkey") is not None and isinstance(node, list):
                found.extend(location + (i,) for i, item in enumerate(node)
                             if isinstance(item, dict)
                             and item.get(step.group("fkey")) == step.group("fval"))
        locations = found
    if not locations:
        raise NotGeneratedBump(f"jsonpath {jsonpath!r} matches nothing")
    return locations


def _json_at(document: Any, location: tuple[Any, ...]) -> Any:
    for part in location:
        document = document[part]
    return document


def _json_set(document: Any, location: tuple[Any, ...], value: Any) -> None:
    for part in location[:-1]:
        document = document[part]
    document[location[-1]] = value


def _changed_lines(before: str, after: str, path: str) -> list[tuple[str, str]]:
    lines_before, lines_after = before.split("\n"), after.split("\n")
    if len(lines_before) != len(lines_after):
        raise NotGeneratedBump(f"release PR changed the line count of {path}")
    return [(b, a) for b, a in zip(lines_before, lines_after) if b != a]


def _prove_json_file(before: str, after: str, path: str, jsonpaths: list[str],
                     old: str, new: str) -> None:
    """Bytes differ only in the `"old"` string at each declared location."""
    base_doc, candidate_doc = _strict_json(before, path), _strict_json(after, path)
    locations = sorted({loc for jp in jsonpaths for loc in _resolve_jsonpath(base_doc, jp)},
                       key=repr)
    expected = json.loads(before)
    for location in locations:
        if _json_at(base_doc, location) != old:
            raise NotGeneratedBump(f"{path} declared location does not hold {old}")
        _json_set(expected, location, new)
    if candidate_doc != expected:
        raise NotGeneratedBump(f"{path} changed beyond its declared version fields")
    changed = _changed_lines(before, after, path)
    if len(changed) != len(locations):
        raise NotGeneratedBump(f"{path} bytes changed outside its version fields")
    for line_before, line_after in changed:
        if line_before.count(f'"{old}"') != 1 or \
                line_before.replace(f'"{old}"', f'"{new}"') != line_after:
            raise NotGeneratedBump(f"{path} bytes changed outside its version token")


def _prove_generic_file(before: str, after: str, path: str, old: str, new: str) -> None:
    """Only marker lines change, each moving `old` to `new` as one whole token."""
    token = re.compile(rf"(?<![0-9A-Za-z.]){re.escape(old)}(?![0-9A-Za-z.])", re.ASCII)
    changed = _changed_lines(before, after, path)
    if not changed:
        raise NotGeneratedBump(f"{path} changed without moving a version")
    for line_before, line_after in changed:
        if GENERIC_MARKER not in line_before or len(token.findall(line_before)) != 1 \
                or token.sub(new, line_before) != line_after:
            raise NotGeneratedBump(f"{path} changed beyond its marked version token")


def _prove_version_edits(
    repo: Path, worktree: Path, surfaces: Any, config: Any, *,
    surface_name: str, base: str, release: str, title_version: str,
) -> tuple[str, str]:
    """Release-please's commit moves only the declared version tokens, one step."""
    surface = next((s for s in config.surfaces if s.name == surface_name), None)
    if surface is None:
        raise NotGeneratedBump(f"protected base has no {surface_name} surface")
    raw = _strict_json(
        (worktree / TRUSTED_WRITER_ROOT / "versioning.json").read_text(encoding="utf-8"),
        "versioning.json",
    )
    mapping = raw["surfaces"][surface_name].get("release_please") or {}
    if not mapping.get("manifest") or not mapping.get("config"):
        raise NotGeneratedBump("protected base names no release-please config for the surface")
    rp_config = _strict_json((worktree / mapping["config"]).read_text(encoding="utf-8"),
                             mapping["config"])
    package = (rp_config.get("packages") or {}).get(".")
    if not isinstance(package, dict) or len(rp_config["packages"]) != 1:
        raise NotGeneratedBump("release-please config is not one root package")
    # Every location release-please may write, from the protected base.
    json_paths: dict[str, list[str]] = {mapping["manifest"]: ['$["."]']}
    generic: set[str] = set()
    for extra in package.get("extra-files") or []:
        if extra.get("type") == "json":
            json_paths.setdefault(extra["path"], []).append(extra["jsonpath"])
        elif extra.get("type") == "generic":
            generic.add(extra["path"])
        else:
            raise NotGeneratedBump(f"unsupported extra-files type {extra.get('type')!r}")

    old_versions = {surfaces.read_version(worktree, vf) for vf in surface.version_files}
    if len(old_versions) != 1:
        raise NotGeneratedBump(f"version files drift within surface {surface_name}")
    old = old_versions.pop()
    new = title_version
    breaking = False
    if _semver(new)[0] == _semver(old)[0] + 1:
        # A major step needs a breaking change among the commits release-please
        # counted for this surface since its last release, never the PR body.
        breaking = _breaking_since(repo, _tag_for(rp_config, old), base,
                                   package.get("exclude-paths") or [])
    _one_step(old, new, breaking=breaking)

    changed = _changed(repo, base, release)
    if not changed or any(status != "M" for status in changed.values()):
        raise NotGeneratedBump("release-please commit adds, deletes or renames a path")
    if not set(changed) <= set(json_paths) | generic \
            or not {vf.path for vf in surface.version_files} <= set(changed):
        raise NotGeneratedBump("release-please commit touches paths outside its declared files")
    for path in changed:
        before = (worktree / path).read_text(encoding="utf-8")
        after = _git(repo, "show", f"{release}:{path}").stdout
        if path in generic:
            _prove_generic_file(before, after, path, old, new)
        else:
            _prove_json_file(before, after, path, json_paths[path], old, new)
        (worktree / path).write_text(after, encoding="utf-8")
    # Each version file must now read as the new version: a changed marker
    # line elsewhere cannot stand in for the one the surface is read from.
    for version_file in surface.version_files:
        if surfaces.read_version(worktree, version_file) != new:
            raise NotGeneratedBump(f"{version_file.path} does not read as {new} after the edits")
    return old, new


def _signing_key_fingerprint(repo: Path, commit: str) -> str | None:
    """SHA256 fingerprint of the public key embedded in a commit's SSH signature."""
    raw = _git(repo, "cat-file", "commit", commit).stdout
    lines = raw.split("\n")
    start = next((i for i, line in enumerate(lines) if line.startswith("gpgsig ")), None)
    if start is None:
        return None
    armored = [lines[start][len("gpgsig "):]]
    for line in lines[start + 1:]:
        if not line.startswith(" "):
            break
        armored.append(line[1:])
    if armored[0] != "-----BEGIN SSH SIGNATURE-----" or armored[-1] != "-----END SSH SIGNATURE-----":
        return None
    try:
        blob = base64.b64decode("".join(armored[1:-1]), validate=True)
    except ValueError:
        return None
    # SSHSIG: magic, uint32 version, string public key, ...
    if blob[:6] != b"SSHSIG" or len(blob) < 14:
        return None
    length = int.from_bytes(blob[10:14], "big")
    key = blob[14:14 + length]
    if len(key) != length or not key:
        return None
    return "SHA256:" + base64.b64encode(hashlib.sha256(key).digest()).decode().rstrip("=")


def _pinned_signer(repo: Path, base: str) -> str:
    shown = _git(repo, "show", f"{base}:{SIGNER_CONFIG}", check=False)
    if shown.returncode != 0:
        raise NotGeneratedBump(f"protected base has no {SIGNER_CONFIG}")
    fingerprint = _strict_json(shown.stdout, SIGNER_CONFIG).get("ssh_signing_key_fingerprint")
    if not isinstance(fingerprint, str) or not fingerprint.startswith("SHA256:"):
        raise NotGeneratedBump(f"{SIGNER_CONFIG} pins no SHA256 signing-key fingerprint")
    return fingerprint


def _bot_signed(meta: Any, what: str, *, repo: Path | None = None, commit: str = "",
                signer: str = "") -> None:
    """The commit is the release bot's: name, email, account, a signature
    GitHub verified, and that signature made with the pinned bot key."""
    if not isinstance(meta, dict):
        raise NotGeneratedBump(f"{what} metadata is not an object")
    if repo is not None and _signing_key_fingerprint(repo, commit) != signer:
        raise NotGeneratedBump(f"{what} is not signed with the pinned release-bot key")
    commit = meta.get("commit") or {}
    verification = commit.get("verification") or {}
    if verification.get("verified") is not True or verification.get("reason") != "valid" \
            or not str(verification.get("signature") or "").startswith(
                "-----BEGIN SSH SIGNATURE-----"):
        raise NotGeneratedBump(f"{what} is not SSH-signed with a valid signature")
    for who in ("author", "committer"):
        person = commit.get(who) or {}
        if person.get("name") != BOT_NAME or person.get("email") != BOT_EMAIL \
                or (meta.get(who) or {}).get("login") != BOT_LOGIN:
            raise NotGeneratedBump(f"{what} {who} is not the release bot")


def _release_pull(
    repo_name: str, event_name: str, event: dict[str, Any], candidate: str
) -> dict[str, Any]:
    if event_name == "pull_request":
        return event.get("pull_request") or {}
    summaries = _flatten_pages(
        _gh_json(repo_name, f"commits/{candidate}/pulls?per_page=100", paginate=True)
    )
    if len(summaries) != 1 or not isinstance(summaries[0], dict):
        raise NotGeneratedBump("candidate is not owned by exactly one pull request")
    detail = _gh_json(repo_name, f"pulls/{summaries[0].get('number')}")
    if not isinstance(detail, dict):
        raise NotGeneratedBump("detailed pull response is not an object")
    return detail


def is_release_pr_event(event_name: str, event: dict[str, Any]) -> bool:
    """A pull_request event for a release-please branch. A merge group names no
    branch; `verify` recognizes a release-please candidate by its commits."""
    return event_name == "pull_request" and (
        ((event.get("pull_request") or {}).get("head") or {}).get("ref") in RELEASE_PR_BRANCHES)


def _verify_release_pr(args: argparse.Namespace, repo: Path, event: dict[str, Any]) -> dict[str, Any]:
    repo_name = (event.get("repository") or {}).get("full_name")
    if not isinstance(repo_name, str) or not repo_name:
        raise NotGeneratedBump("event omitted repository.full_name")
    group_head = None
    if args.event_name == "pull_request":
        pull_event = event.get("pull_request") or {}
        candidate = (pull_event.get("head") or {}).get("sha")
        event_base = (pull_event.get("base") or {}).get("sha")
        if args.head != candidate and args.head not in {
            pull_event.get("merge_commit_sha"), event.get("after")
        }:
            if not getattr(args, "accept_pr_merge_ref", False):
                raise NotGeneratedBump("workflow head is unrelated to the pull request event")
            _prove_pr_merge_ref(repo, merge_head=args.head, event_base=event_base,
                                candidate=candidate)
    elif args.event_name == "merge_group":
        group = event.get("merge_group") or {}
        event_base, group_head = group.get("base_sha"), args.head
        if group.get("head_sha") != group_head:
            raise NotGeneratedBump("workflow head does not match the merge-group event")
        _fetch_commit(repo, group_head)
        parents = _parents(repo, group_head)
        if len(parents) != 2 or parents[0] != event_base:
            raise NotGeneratedBump("merge group is mixed, nested, or has unknown topology")
        candidate = parents[1]
    else:
        raise NotGeneratedBump(f"unsupported event: {args.event_name}")
    if args.base != event_base or not all(
        isinstance(v, str) and SHA_RE.fullmatch(v) for v in (event_base, candidate)
    ):
        raise NotGeneratedBump("explicit commits do not match immutable event commits")

    release, refresh, base = _release_chain(repo, candidate)
    pull = _release_pull(repo_name, args.event_name, event, candidate)
    head = pull.get("head") or {}
    surface_name = RELEASE_PR_BRANCHES.get(head.get("ref"))
    if surface_name is None:
        raise NotGeneratedBump("pull request is not a release-please release PR")
    title = RELEASE_PR_TITLE.fullmatch(pull.get("title") or "")
    if pull.get("state") != "open" or title is None:
        raise NotGeneratedBump("release PR is not open or its title is not release-please's")
    if bool(title.group("plugin")) != (surface_name == "plugin"):
        raise NotGeneratedBump("release PR title names the other surface")
    if _subject(repo, release) != pull.get("title"):
        raise NotGeneratedBump("release commit subject is not the release PR title")
    labels = {label.get("name") for label in pull.get("labels") or [] if isinstance(label, dict)}
    if RELEASE_PR_LABEL not in labels:
        raise NotGeneratedBump(f"release PR does not carry the {RELEASE_PR_LABEL!r} label")
    if head.get("sha") != candidate or (head.get("repo") or {}).get("full_name") != repo_name:
        raise NotGeneratedBump("release PR head is not this candidate in this repository")
    if (pull.get("base") or {}).get("ref") != "main":
        raise NotGeneratedBump("release PR does not target main")
    if pull.get("commits") != (2 if refresh else 1):
        raise NotGeneratedBump("release PR holds commits other than release-please's and the refresh")
    if refresh and surface_name != "sdk":
        raise NotGeneratedBump("only the SDK release PR carries a derived-file refresh")
    if args.event_name == "pull_request" and base != event_base:
        raise NotGeneratedBump("release PR is behind main; the merge group proves it instead")

    # Identity: release-please.yml re-commits release-please's API commit as
    # the release bot, SSH-signed (same tree and message), and signs the
    # refresh the same way, so both commits carry the bot's signature.
    signer = _pinned_signer(repo, base)
    _bot_signed(_gh_json(repo_name, f"commits/{release}"), "release commit",
                repo=repo, commit=release, signer=signer)
    if refresh:
        _bot_signed(_gh_json(repo_name, f"commits/{refresh}"), "derived-file refresh",
                    repo=repo, commit=refresh, signer=signer)
        message = _git(repo, "show", "-s", "--format=%B", refresh).stdout
        if f"{REFRESH_MARKER} #{pull.get('number')} {release}" not in message:
            raise NotGeneratedBump("refresh marker does not bind this PR and release commit")
    # The verifier and regenerators run from `base`, so it must be protected main.
    status = (_gh_json(repo_name, f"compare/{base}...main") or {}).get("status")
    if status not in ("ahead", "identical"):
        raise NotGeneratedBump("release PR base is not on protected main")

    _complete_history(repo, [base])
    runner_temp = Path(os.environ.get("RUNNER_TEMP") or tempfile.gettempdir())
    runner_temp.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="pulp-release-pr-proof-", dir=runner_temp) as holder:
        worktree = Path(holder) / "base"
        _git(repo, "worktree", "add", "--detach", str(worktree), base)
        try:
            scripts = worktree / TRUSTED_WRITER_ROOT
            # Importing from the worktree must not write __pycache__ into the
            # tree whose bytes are compared below.
            sys.dont_write_bytecode = True
            sys.path.insert(0, str(scripts))
            surfaces = importlib.import_module("version_bump_surfaces")
            config = surfaces.load_config(scripts / "versioning.json")
            old, new = _prove_version_edits(
                repo, worktree, surfaces, config,
                surface_name=surface_name, base=base, release=release,
                title_version=title.group("version"),
            )
            if surface_name == "sdk":
                # Reproduce the refresh (or prove there was nothing to refresh)
                # with the protected base's own regenerators.
                if refresh and not all(
                    path.startswith(REFRESH_PATH_PREFIXES) for path in _changed(repo, release, refresh)
                ):
                    raise NotGeneratedBump("derived-file refresh touches a non-derived path")
                if not (scripts / "release_derived_refresh.py").is_file():
                    raise NotGeneratedBump("protected base has no release_derived_refresh.py")
                version_at_land = importlib.import_module("version_at_land")
                _bind_trusted_regenerators(version_at_land, worktree)
                refresher = importlib.import_module("release_derived_refresh")
                refresher.refresh(worktree, scripts / "versioning.json")
                _adopt_unreproduced(repo, worktree, base=base, observed=candidate, edited=[])
            _git(worktree, "add", "-A")
            expected_tree = _git_text(worktree, "write-tree")
            if expected_tree != _git_text(repo, "rev-parse", f"{candidate}^{{tree}}"):
                raise NotGeneratedBump("release PR tree differs from the proven projection")
        finally:
            _git(repo, "worktree", "remove", "--force", str(worktree), check=False)

    if group_head is not None:
        # The group must be main plus exactly the PR's edits, on paths main has
        # not touched since the PR's base.
        edited = _changed(repo, base, candidate)
        _fetch_commit(repo, event_base)
        for path in edited:
            if _blob(repo, event_base, path) != _blob(repo, base, path):
                raise NotGeneratedBump(f"main changed {path} after the release PR's base")
        group_changed = _changed(repo, event_base, group_head)
        if set(group_changed) != set(edited) or any(
            _blob(repo, group_head, path) != _blob(repo, candidate, path) for path in edited
        ):
            raise NotGeneratedBump("merge-group tree is not main plus the release PR's edits")
    return {
        "generated_version_bump": True,
        "base": args.base,
        "candidate": candidate,
        "surfaces": [surface_name],
        "release_please": {"from": old, "to": new, "refresh": bool(refresh)},
    }


def verify(args: argparse.Namespace) -> dict[str, Any]:
    repo = args.repo.resolve()
    event = _load_json(args.event_path)
    if not isinstance(event, dict):
        raise NotGeneratedBump("event payload is not an object")
    if is_release_pr_event(args.event_name, event):
        return _verify_release_pr(args, repo, event)
    if args.event_name == "merge_group":
        # A merge group does not name its pull request; a release-please
        # candidate is recognized by its own commits before the bump path runs.
        group_parents = []
        if (event.get("merge_group") or {}).get("head_sha") == args.head:
            _fetch_commit(repo, args.head)
            group_parents = _parents(repo, args.head)
        if len(group_parents) == 2:
            _fetch_commit(repo, group_parents[1])
            subject = _subject(repo, group_parents[1])
            if subject == REFRESH_SUBJECT or RELEASE_PR_TITLE.fullmatch(subject):
                return _verify_release_pr(args, repo, event)
    repo_name, candidate, candidate_base, cumulative_head = _event_commits(
        repo,
        args.event_name,
        event,
        explicit_base=args.base,
        explicit_head=args.head,
        accept_pr_merge_ref=bool(getattr(args, "accept_pr_merge_ref", False)),
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
    # Every cheap provenance check has passed; only now pay for history.
    _complete_history(
        repo,
        [candidate_base, args.base] if cumulative_head is not None else [candidate_base],
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
    parser.add_argument(
        "--accept-pr-merge-ref",
        action="store_true",
        help=(
            "on pull_request events, accept a GITHUB_SHA that is the checked-out "
            "merge of exactly [base.sha, head.sha]; the workflow passes this only "
            "when the PULP_BUMP_FASTPATH_PR_MERGE_REF repository variable is 1"
        ),
    )
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)
    try:
        result = verify(args)
    except (NotGeneratedBump, OSError, subprocess.SubprocessError, ValueError,
            KeyError, TypeError, AttributeError) as exc:
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
