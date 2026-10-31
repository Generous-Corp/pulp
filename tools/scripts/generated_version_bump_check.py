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
# The release bot's SSH signing key(s), read from the PROTECTED BASE only. An
# author name, email or GitHub login is free text any signer can claim; the key
# that produced the signature is the identity.
RELEASE_BOT_SIGNING_PIN = "tools/scripts/release_bot_signing.json"
SSH_FINGERPRINT_RE = re.compile(r"^SHA256:[A-Za-z0-9+/]{43}$")
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


def _ssh_key_fingerprint(public_key_blob: bytes) -> str:
    """OpenSSH's ``SHA256:`` fingerprint of a wire-format public key blob."""
    digest = hashlib.sha256(public_key_blob).digest()
    return "SHA256:" + base64.b64encode(digest).decode("ascii").rstrip("=")


def _ssh_signature_key_fingerprint(armored: str) -> str:
    """Fingerprint of the public key embedded in an armored ``SSHSIG``.

    GitHub's ``verification.verified`` says the signature is valid for SOME key
    registered to the account; the embedded key says WHICH one. Parsing is
    strict and fails closed on any malformed field.
    """
    lines = [line.strip() for line in armored.strip().splitlines() if line.strip()]
    if (len(lines) < 3 or lines[0] != "-----BEGIN SSH SIGNATURE-----"
            or lines[-1] != "-----END SSH SIGNATURE-----"):
        raise NotGeneratedBump("release-bot signature is not an armored SSH signature")
    try:
        raw = base64.b64decode("".join(lines[1:-1]), validate=True)
    except ValueError as exc:
        raise NotGeneratedBump("release-bot SSH signature is not valid base64") from exc
    if raw[:6] != b"SSHSIG" or len(raw) < 10:
        raise NotGeneratedBump("release-bot SSH signature has no SSHSIG preamble")
    offset = 10  # magic + uint32 version

    def field() -> bytes:
        nonlocal offset
        if offset + 4 > len(raw):
            raise NotGeneratedBump("release-bot SSH signature is truncated")
        size = int.from_bytes(raw[offset:offset + 4], "big")
        offset += 4
        if offset + size > len(raw):
            raise NotGeneratedBump("release-bot SSH signature is truncated")
        value = raw[offset:offset + size]
        offset += size
        return value

    if int.from_bytes(raw[6:10], "big") != 1:
        raise NotGeneratedBump("release-bot SSH signature has an unknown version")
    public_key = field()
    if field() != b"git":
        raise NotGeneratedBump("release-bot SSH signature is not in the git namespace")
    if not public_key:
        raise NotGeneratedBump("release-bot SSH signature carries no public key")
    return _ssh_key_fingerprint(public_key)


def _pinned_signing_fingerprints(repo: Path, base: str) -> set[str]:
    """The release-bot key fingerprints the protected base declares.

    Read with ``git show <base>:``, never from the working tree or the
    candidate, so a candidate cannot name its own signer. Each entry's
    fingerprint must be the one its recorded public key yields. A missing,
    unreadable or empty pin refuses the fast path.
    """
    shown = _git(repo, "show", f"{base}:{RELEASE_BOT_SIGNING_PIN}", check=False)
    if shown.returncode != 0:
        raise NotGeneratedBump("the protected base pins no release-bot signing key")
    try:
        entries = json.loads(shown.stdout).get("ssh_signing_keys")
    except (json.JSONDecodeError, AttributeError) as exc:
        raise NotGeneratedBump("the protected base's release-bot key pin is unreadable") from exc
    if not isinstance(entries, list) or not entries:
        raise NotGeneratedBump("the protected base pins no release-bot signing key")
    pinned: set[str] = set()
    for entry in entries:
        fingerprint = entry.get("fingerprint") if isinstance(entry, dict) else None
        public_key = entry.get("public_key") if isinstance(entry, dict) else None
        if not isinstance(fingerprint, str) or not SSH_FINGERPRINT_RE.fullmatch(fingerprint):
            raise NotGeneratedBump("the protected base's release-bot key pin is malformed")
        parts = public_key.split() if isinstance(public_key, str) else []
        try:
            blob = base64.b64decode(parts[1], validate=True) if len(parts) >= 2 else b""
        except ValueError:
            blob = b""
        if not blob or _ssh_key_fingerprint(blob) != fingerprint:
            raise NotGeneratedBump(
                "the protected base's release-bot key pin disagrees with its public key"
            )
        pinned.add(fingerprint)
    return pinned


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
    signature = str(verification.get("signature") or "")
    if not signature.startswith("-----BEGIN SSH SIGNATURE-----"):
        raise NotGeneratedBump("release-bot commit is not SSH-signed")
    if _ssh_signature_key_fingerprint(signature) not in _pinned_signing_fingerprints(
        repo, candidate_base
    ):
        raise NotGeneratedBump(
            "commit is not signed by the release-bot key the protected base pins"
        )
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


def _derive_assignments(
    repo: Path,
    surfaces: Any,
    version_at_land: Any,
    config: Any,
    worktree: Path,
    *,
    base: str,
    candidate: str,
) -> list[Any]:
    """The per-surface version increments the candidate commit claims.

    Fails closed on an unreadable version file, a surface whose files disagree
    with each other, a step that is not one strict semantic-version increment,
    or a candidate that moves no configured surface.
    """
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
    return assignments


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
            assignments = _derive_assignments(
                repo, surfaces, version_at_land, config, worktree,
                base=base, candidate=candidate,
            )

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


def _diff_paths(repo: Path, older: str, newer: str) -> list[str]:
    """Every path that differs between two commits, renames split into A + D."""
    proc = _git(
        repo, "diff", "--name-status", "--no-renames", "-z", older, newer,
        check=False,
    )
    if proc.returncode != 0:
        raise NotGeneratedBump(f"could not compute the diff {older[:12]}..{newer[:12]}")
    fields = proc.stdout.split("\0")
    if fields and fields[-1] == "":
        fields.pop()
    if len(fields) % 2 != 0:
        raise NotGeneratedBump("diff path status stream is malformed")
    paths: list[str] = []
    for index in range(0, len(fields), 2):
        status, path = fields[index], fields[index + 1]
        if status not in {"A", "M", "D"} or not path:
            raise NotGeneratedBump("diff contains a type change or unknown status")
        paths.append(path)
    return paths


# Code and build files the writer allowlist admits. On a path match alone a
# forged or buggy bump could carry arbitrary CMake or Python, so allowlist-diff
# mode admits each of them only when the change is confined to its version
# value(s): every other byte must match the protected base. Each pattern's
# first group is the fixed text, its second the value that may move.
CONTENT_BOUNDED: dict[str, tuple[re.Pattern[str], str]] = {
    "CMakeLists.txt": (
        re.compile(r"(project\s*\(\s*[A-Za-z_]\w*\s+VERSION\s+)(\d+\.\d+\.\d+)"),
        "version",
    ),
    "tools/scripts/agent_capability_manifest.py": (
        re.compile(r"^((?:MANIFEST_REVISION|SURFACE_INVENTORY_VERSION) = )(\d+)$",
                   re.MULTILINE),
        "counter",
    ),
}
# A changed path that looks like code or build input and has no bound above
# keeps the full path, so widening the writer allowlist cannot silently widen
# what skips the native build.
CODE_SUFFIXES = (
    ".bash", ".bat", ".c", ".cc", ".cjs", ".cmake", ".cpp", ".cxx", ".h", ".hpp",
    ".in", ".js", ".m", ".mjs", ".mm", ".ps1", ".py", ".rs", ".sh", ".swift",
    ".toml", ".ts", ".yml", ".zsh",
)


def _blob_text(repo: Path, commit: str, path: str) -> str | None:
    shown = _git(repo, "show", f"{commit}:{path}", check=False)
    return shown.stdout if shown.returncode == 0 else None


def _check_bounded_content(repo: Path, older: str, newer: str, paths: list[str]) -> None:
    """Refuse a code/build change that is more than its version value(s)."""
    for path in paths:
        bound = CONTENT_BOUNDED.get(path)
        if bound is None:
            name = path.rsplit("/", 1)[-1]
            if name == "CMakeLists.txt" or name.endswith(CODE_SUFFIXES):
                raise NotGeneratedBump(f"code path {path} has no version-only content bound")
            continue
        pattern, kind = bound
        before = _blob_text(repo, older, path)
        after = _blob_text(repo, newer, path)
        if before is None or after is None:
            raise NotGeneratedBump(f"{path} is added or removed, not a version edit")
        before_matches = pattern.findall(before)
        after_matches = pattern.findall(after)
        fixed_before = [prefix for prefix, _ in before_matches]
        if (not before_matches or len(set(fixed_before)) != len(fixed_before)
                or fixed_before != [prefix for prefix, _ in after_matches]):
            raise NotGeneratedBump(f"{path} version line(s) are missing or reshaped")
        if pattern.sub(r"\g<1>@", before) != pattern.sub(r"\g<1>@", after):
            raise NotGeneratedBump(f"{path} changes more than its version value(s)")
        if kind == "counter":
            for (_, old_value), (_, new_value) in zip(before_matches, after_matches):
                if int(new_value) - int(old_value) not in (0, 1):
                    raise NotGeneratedBump(f"{path} counter moves by more than one")


def _check_allowlisted_diff(
    repo: Path,
    *,
    base: str,
    candidate: str,
    event_base: str,
    event_head: str,
) -> list[Any]:
    """Accept a bot bump whose whole diff sits inside the writer's allowlist.

    The looser sibling of ``_reproduce_tree``: the identity, signature, branch,
    title and topology checks have already passed, so this decides only whether
    the CONTENT is a version bump. It is for a bump the exact projection cannot
    reproduce byte for byte (a migration note added or removed, or a merge group
    that already carries an earlier queue entry), where the protected base's
    own writer allowlist still bounds every path it may touch.

    Requires, all from the protected base: the ``Version-Bump-Applied:`` marker
    on the canonical subject; every path in the candidate's diff, and in the
    event's own diff (the merge group against its base), inside
    ``writer_allowed_globs``; and each configured surface at one consistent
    version that moved by one strict increment. A group that batches the bump
    with another change has a diff outside the allowlist and is refused. A
    code or build file inside the allowlist must change only its version
    value(s) (``CONTENT_BOUNDED``); one with no bound is refused.
    """
    message = _git(repo, "show", "-s", "--format=%B", candidate).stdout
    if message.splitlines()[:1] != [BUMP_SUBJECT]:
        raise NotGeneratedBump("commit subject is not the canonical bump subject")

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
            try:
                has_marker = version_at_land.has_applied_marker
                outside = version_at_land.paths_outside_writer_allowlist
            except AttributeError as exc:
                raise NotGeneratedBump(
                    "the protected base predates the shared writer allowlist"
                ) from exc
            if not has_marker(message):
                raise NotGeneratedBump("commit carries no Version-Bump-Applied marker")
            config = surfaces.load_config(scripts / "versioning.json")
            if not tuple(config.writer_allowed_globs):
                raise NotGeneratedBump("the protected base declares no writer allowlist")
            if event_base != base:
                _assert_writer_unchanged(repo, base, event_base)
            observed: list[str] = []
            diffs: list[tuple[str, str, list[str]]] = []
            for older, newer in dict.fromkeys(((base, candidate), (event_base, event_head))):
                paths = _diff_paths(repo, older, newer)
                if not paths:
                    raise NotGeneratedBump("bump diff is empty")
                observed.extend(paths)
                diffs.append((older, newer, paths))
            stray = outside(config, observed)
            if stray:
                raise NotGeneratedBump(
                    "diff leaves the writer allowlist: " + ", ".join(stray[:5])
                )
            for older, newer, paths in diffs:
                _check_bounded_content(repo, older, newer, paths)
            return _derive_assignments(
                repo, surfaces, version_at_land, config, worktree,
                base=base, candidate=candidate,
            )
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
    if getattr(args, "allowlist_diff", False):
        assignments = _check_allowlisted_diff(
            repo,
            base=candidate_base,
            candidate=candidate,
            event_base=args.base,
            event_head=cumulative_head or candidate,
        )
        return {
            "generated_version_bump": True,
            "mode": "allowlist-diff",
            "base": args.base,
            "candidate": candidate,
            "surfaces": [item.surface for item in assignments],
        }
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
    parser.add_argument(
        "--allowlist-diff",
        action="store_true",
        help=(
            "accept a bump whose whole diff stays inside the protected base's "
            "writer_allowed_globs instead of reproducing its tree byte for byte; "
            "every identity, signature and topology check still applies"
        ),
    )
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
            "generated-version-bump: "
            f"{'allowlisted' if result.get('mode') == 'allowlist-diff' else 'exact'}"
            " trusted transaction "
            f"({','.join(result['surfaces'])})",
            file=sys.stderr,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
