#!/usr/bin/env python3
"""Counter-derivation contract tests.

The rule these pin is asymmetric and easy to get backwards: a counter must
advance when its own material moved, and must NOT advance when it did not.
Over-bumping is not the safe direction — it fails the opposite evolution rule,
`... changed without a manifest change`.
"""
from __future__ import annotations

import io
import pathlib
import subprocess
import sys
import tempfile
from unittest import mock

import agent_capability_rederive as rederive


ROOT = pathlib.Path(__file__).resolve().parents[2]

BASE_MANIFEST = {"manifest_revision": 28, "capabilities": [{"key": "a"}]}
BASE_SURFACE = {"inventory_version": 45, "headers": [{"include": "x.hpp"}]}


def _manifest(revision: int, capabilities: list[dict[str, str]] | None = None) -> dict:
    return {
        "manifest_revision": revision,
        "capabilities": capabilities if capabilities is not None else [{"key": "a"}],
    }


def _surface(version: int, headers: list[dict[str, str]] | None = None) -> dict:
    return {
        "inventory_version": version,
        "headers": headers if headers is not None else [{"include": "x.hpp"}],
    }


def exercise_no_change() -> int:
    """Identical material must hold both counters still."""
    revision, inventory = rederive.derive(
        _manifest(28), _surface(45), BASE_MANIFEST, BASE_SURFACE
    )
    assert (revision, inventory) == (28, 45), (revision, inventory)

    # A tree that already carries a HIGHER counter with identical material is
    # still derived back down: the counter it holds is not evidence, the
    # material is. This is the stale-reservation case the tool exists for.
    revision, inventory = rederive.derive(
        _manifest(29), _surface(46), BASE_MANIFEST, BASE_SURFACE
    )
    assert (revision, inventory) == (28, 45), (revision, inventory)
    return 2


def exercise_manifest_only() -> int:
    """Manifest material moved; the surface did not."""
    revision, inventory = rederive.derive(
        _manifest(28, [{"key": "a"}, {"key": "b"}]),
        _surface(45),
        BASE_MANIFEST,
        BASE_SURFACE,
    )
    assert revision == 29, revision
    assert inventory == 45, f"surface must not move when its material is identical: {inventory}"
    return 1


def exercise_surface_only() -> int:
    """Surface material moved; the manifest did not."""
    revision, inventory = rederive.derive(
        _manifest(28),
        _surface(45, [{"include": "x.hpp"}, {"include": "y.hpp"}]),
        BASE_MANIFEST,
        BASE_SURFACE,
    )
    assert inventory == 46, inventory
    assert revision == 28, f"manifest must not move when its material is identical: {revision}"
    return 1


def exercise_both() -> int:
    revision, inventory = rederive.derive(
        _manifest(28, [{"key": "a"}, {"key": "b"}]),
        _surface(45, [{"include": "x.hpp"}, {"include": "y.hpp"}]),
        BASE_MANIFEST,
        BASE_SURFACE,
    )
    assert (revision, inventory) == (29, 46), (revision, inventory)
    return 1


def exercise_counter_is_not_material() -> int:
    """The counter itself must be excluded from the comparison.

    Without the pop, a tree holding 29 against a base of 28 would look like a
    material change and bump forever on every re-run.
    """
    first = rederive.derive(_manifest(28), _surface(45), BASE_MANIFEST, BASE_SURFACE)
    second = rederive.derive(
        _manifest(first[0]), _surface(first[1]), BASE_MANIFEST, BASE_SURFACE
    )
    assert first == second == (28, 45), (first, second)
    return 1


def exercise_rejects_underivable_base() -> int:
    """A base we cannot read must raise, never guess a number."""
    checks = 0
    for base_manifest, base_surface, needle in (
        (None, BASE_SURFACE, "initial bootstrap"),
        (BASE_MANIFEST, None, "initial bootstrap"),
        ({"manifest_revision": "28"}, BASE_SURFACE, "manifest_revision is not an integer"),
        (BASE_MANIFEST, {"inventory_version": None}, "inventory_version is not an integer"),
        # bool is an int subclass in Python; True must not read as revision 1.
        ({"manifest_revision": True}, BASE_SURFACE, "manifest_revision is not an integer"),
    ):
        try:
            rederive.derive(_manifest(28), _surface(45), base_manifest, base_surface)
        except rederive.DeriveError as error:
            assert needle in str(error), f"expected {needle!r} in {error!r}"
            checks += 1
        else:  # pragma: no cover - a miss is a test failure, not a branch
            raise AssertionError(f"underivable base unexpectedly accepted: {needle}")
    return checks


def exercise_rewrite_round_trip() -> int:
    """Rewriting must move exactly the two constants and stay idempotent."""
    path = ROOT / rederive.SCRIPT
    original = path.read_text(encoding="utf-8")
    try:
        moved = rederive.rewrite_constants(ROOT, 1234, 5678)
        assert moved is True
        text = path.read_text(encoding="utf-8")
        assert "MANIFEST_REVISION = 1234" in text
        assert "SURFACE_INVENTORY_VERSION = 5678" in text
        # Same values again must report "nothing moved" rather than rewrite.
        assert rederive.rewrite_constants(ROOT, 1234, 5678) is False
        # Everything except those two lines must be byte-identical.
        before = [
            line for line in original.splitlines()
            if not line.startswith(("MANIFEST_REVISION =", "SURFACE_INVENTORY_VERSION ="))
        ]
        after = [
            line for line in text.splitlines()
            if not line.startswith(("MANIFEST_REVISION =", "SURFACE_INVENTORY_VERSION ="))
        ]
        assert before == after, "rewrite touched lines other than the two constants"
        return 4
    finally:
        path.write_text(original, encoding="utf-8")


def exercise_refuses_mid_merge() -> int:
    """An uncommitted merge must refuse, not derive from the merge base.

    This is the case the tool is most often reached for and the one where the
    resolver quietly answers a different question: until the merge commits, the
    incoming tip is not an ancestor of HEAD, so resolution steps back to the
    merge base and the derived counter is stale but plausible.
    """
    git_dir = subprocess.run(
        ["git", "rev-parse", "--git-dir"],
        cwd=ROOT, capture_output=True, text=True,
    ).stdout.strip()
    if not git_dir:  # pragma: no cover - not a work tree
        return 0
    path = pathlib.Path(git_dir)
    if not path.is_absolute():
        path = ROOT / path
    merge_head = path / "MERGE_HEAD"
    if merge_head.exists():  # pragma: no cover - never clobber a real merge
        raise AssertionError(
            "a merge is genuinely in progress; refusing to run this test"
        )
    try:
        merge_head.write_text("0" * 39 + "1\n", encoding="utf-8")
        completed = subprocess.run(
            [sys.executable, str(ROOT / "tools/scripts/agent_capability_rederive.py"),
             "--print"],
            cwd=ROOT, capture_output=True, text=True,
        )
        assert completed.returncode != 0, completed.stdout
        assert "a merge is in progress" in completed.stderr, completed.stderr
        # The message must say what to DO, or it is just a different stall.
        assert "Commit the merge first" in completed.stderr, completed.stderr
    finally:
        merge_head.unlink(missing_ok=True)
    assert not merge_head.exists(), "test left a stray MERGE_HEAD behind"
    return 3


def exercise_reset_is_write_path_only() -> int:
    """Resetting the generated artifacts must not happen on a read-only run.

    `--print` and `--no-write` exist to answer a question without touching the
    tree; a reset that fired there would silently discard a merge resolution
    someone was still inspecting.
    """
    checks = 0
    # The reset covers exactly the three files --write regenerates. If a fourth
    # generated artifact is ever added, this fails rather than silently leaving
    # it merged-but-unreset — which is the defect this whole path exists to fix.
    names = {str(a) for a in rederive.GENERATED_ARTIFACTS}
    assert names == {
        "docs/status/agent-capabilities.json",
        "docs/status/agent-capability-surface.json",
        "tools/agent-capabilities/contract-history.json",
    }, names
    checks += 1

    # On a CLEAN tree a reset is a no-op, so comparing git status before and
    # after proves nothing — the first version of this test passed against a
    # deliberately broken tool for exactly that reason. Give the reset something
    # to destroy: a generated artifact that differs from the base, standing in
    # for the merge resolution a read-only run must not discard.
    artifact = ROOT / "docs/status/agent-capability-surface.json"
    original = artifact.read_text(encoding="utf-8")
    manifest_script = ROOT / "tools/scripts/agent_capability_manifest.py"
    original_manifest_script = manifest_script.read_text(encoding="utf-8")
    sentinel = original.replace("\n", "\n", 1) + "\n"  # trailing byte, still valid text
    try:
        for mode in ("--print", "--no-write"):
            artifact.write_text(sentinel, encoding="utf-8")
            subprocess.run(
                [sys.executable, str(ROOT / "tools/scripts/agent_capability_rederive.py"), mode],
                cwd=ROOT, capture_output=True, text=True,
            )
            assert artifact.read_text(encoding="utf-8") == sentinel, (
                f"{mode} reset a generated artifact; a read-only run must not "
                "discard an in-progress merge resolution"
            )
            checks += 1
    finally:
        artifact.write_text(original, encoding="utf-8")
        # --no-write rewrites the constants by design. Restore the exact bytes
        # present when this test began; `git checkout` would erase unrelated
        # uncommitted validator work in the same authorized file.
        manifest_script.write_text(original_manifest_script, encoding="utf-8")
    return checks


def exercise_prints_comparison_receipt() -> int:
    # HEAD always contains itself, so the receipt does not depend on how far
    # this checkout is behind origin/main.
    completed = subprocess.run(
        [sys.executable, str(ROOT / "tools/scripts/agent_capability_rederive.py"),
         "--print"],
        cwd=ROOT, capture_output=True, text=True,
        env={**rederive.history.os.environ, "PULP_AGENT_CAPABILITY_BASE_REF": "HEAD"},
    )
    assert completed.returncode == 0, completed.stderr
    assert "comparison receipt" in completed.stdout, completed.stdout
    assert '"status":"available"' in completed.stdout, completed.stdout
    assert '"comparison_anchor":' in completed.stdout, completed.stdout
    return 3


def exercise_proven_commit_absent_vs_malformed_authority() -> int:
    """A proven commit may lack a path or contain bad JSON; neither is history loss."""
    with tempfile.TemporaryDirectory() as directory:
        root = pathlib.Path(directory)

        def git(*arguments: str) -> None:
            subprocess.run(
                ["git", *arguments], cwd=root, check=True,
                capture_output=True, text=True,
            )

        git("init", "-q", "-b", "main")
        git("config", "user.email", "t@t")
        git("config", "user.name", "t")
        (root / "anchor.txt").write_text("proven commit\n")
        git("add", ".")
        git("commit", "-qm", "base without authority")
        with mock.patch.dict(
            rederive.history.os.environ,
            {"PULP_AGENT_CAPABILITY_BASE_REF": "HEAD"},
        ):
            try:
                rederive._resolve_base(root)
            except rederive.DeriveError as error:
                absent = str(error)
            else:
                raise AssertionError("absent authority unexpectedly derived")
        assert "authority is invalid" in absent, absent
        assert "required artifact is absent" in absent, absent
        assert '"status":"path_absent"' in absent, absent

        manifest_path = root / rederive.history.SNAPSHOT
        surface_path = root / rederive.surface.SURFACE_SNAPSHOT
        manifest_path.parent.mkdir(parents=True, exist_ok=True)
        surface_path.parent.mkdir(parents=True, exist_ok=True)
        manifest_path.write_text("{ malformed\n")
        surface_path.write_text("{}\n")
        git("add", ".")
        git("commit", "-qm", "malformed authority")
        with mock.patch.dict(
            rederive.history.os.environ,
            {"PULP_AGENT_CAPABILITY_BASE_REF": "HEAD"},
        ):
            try:
                rederive._resolve_base(root)
            except rederive.DeriveError as error:
                malformed = str(error)
            else:
                raise AssertionError("malformed authority unexpectedly derived")
        assert "authority is invalid JSON" in malformed, malformed
        assert '"status":"available"' in malformed, malformed
    return 5


def exercise_github_event_failures_preserve_resolved_head() -> int:
    with tempfile.TemporaryDirectory() as directory:
        root = pathlib.Path(directory)
        subprocess.run(["git", "init", "-q", "-b", "main"], cwd=root, check=True)
        subprocess.run(["git", "config", "user.email", "t@t"], cwd=root, check=True)
        subprocess.run(["git", "config", "user.name", "t"], cwd=root, check=True)
        (root / "seed.txt").write_text("seed\n")
        subprocess.run(["git", "add", "."], cwd=root, check=True)
        subprocess.run(["git", "commit", "-qm", "seed"], cwd=root, check=True)
        head = subprocess.run(
            ["git", "rev-parse", "HEAD"], cwd=root, check=True,
            capture_output=True, text=True,
        ).stdout.strip()
        event = root / "event.json"
        env = {"GITHUB_ACTIONS": "true", "GITHUB_EVENT_PATH": str(event)}
        candidate = "f" * 40
        event.write_text('{"pull_request":{"base":{"sha":"' + candidate + '"}}}')
        with mock.patch.dict(rederive.history.os.environ, env, clear=False):
            rederive.history.os.environ.pop("PULP_AGENT_CAPABILITY_BASE_REF", None)
            fetch_failed = rederive.history._resolve_protected_comparison(
                root, "missing-local"
            )
        assert fetch_failed.requested_base == candidate
        assert fetch_failed.resolved_head == head
        assert fetch_failed.resolved_base_tip is None
        assert fetch_failed.comparison_anchor is None
        assert fetch_failed.status != "available"
        assert len(fetch_failed.stderr) <= 512

        for text in ("{ malformed", '{"pull_request":null}'):
            event.write_text(text)
            with mock.patch.dict(rederive.history.os.environ, env, clear=False):
                rederive.history.os.environ.pop(
                    "PULP_AGENT_CAPABILITY_BASE_REF", None
                )
                malformed = rederive.history._resolve_protected_comparison(
                    root, "HEAD"
                )
            assert malformed.source == "github_event"
            assert malformed.resolved_head == head
            assert malformed.resolved_base_tip is None
            assert malformed.comparison_anchor is None
            assert malformed.status == "history_unavailable"
    return 16


def exercise_github_event_repairs_exact_shallow_merge() -> int:
    with tempfile.TemporaryDirectory() as directory:
        root = pathlib.Path(directory)
        origin = root / "origin.git"
        source = root / "source"
        pr_checkout = root / "pr-checkout"
        dispatch_checkout = root / "dispatch-checkout"
        subprocess.run(["git", "init", "-q", "--bare", origin], check=True)
        subprocess.run(["git", "init", "-q", "-b", "main", source], check=True)
        subprocess.run(["git", "config", "user.email", "t@t"], cwd=source, check=True)
        subprocess.run(["git", "config", "user.name", "t"], cwd=source, check=True)
        (source / "seed.txt").write_text("seed\n")
        subprocess.run(["git", "add", "."], cwd=source, check=True)
        subprocess.run(["git", "commit", "-qm", "base"], cwd=source, check=True)
        common_base = subprocess.run(
            ["git", "rev-parse", "HEAD"], cwd=source, check=True,
            capture_output=True, text=True,
        ).stdout.strip()
        subprocess.run(["git", "checkout", "-qb", "feature"], cwd=source, check=True)
        (source / "feature.txt").write_text("feature\n")
        subprocess.run(["git", "add", "."], cwd=source, check=True)
        subprocess.run(["git", "commit", "-qm", "feature"], cwd=source, check=True)
        feature_head = subprocess.run(
            ["git", "rev-parse", "HEAD"], cwd=source, check=True,
            capture_output=True, text=True,
        ).stdout.strip()
        subprocess.run(["git", "checkout", "-q", "main"], cwd=source, check=True)
        (source / "main.txt").write_text("main\n")
        subprocess.run(["git", "add", "."], cwd=source, check=True)
        subprocess.run(["git", "commit", "-qm", "protected main"], cwd=source, check=True)
        protected_base = subprocess.run(
            ["git", "rev-parse", "HEAD"], cwd=source, check=True,
            capture_output=True, text=True,
        ).stdout.strip()
        subprocess.run(["git", "checkout", "-qb", "synthetic"], cwd=source, check=True)
        subprocess.run(
            ["git", "merge", "-q", "--no-ff", "feature", "-m", "synthetic"],
            cwd=source, check=True,
        )
        synthetic_head = subprocess.run(
            ["git", "rev-parse", "HEAD"], cwd=source, check=True,
            capture_output=True, text=True,
        ).stdout.strip()
        subprocess.run(
            ["git", "remote", "add", "origin", str(origin)],
            cwd=source,
            check=True,
        )
        subprocess.run(
            ["git", "push", "-q", "origin", "main", "feature", "synthetic"],
            cwd=source, check=True,
        )
        subprocess.run(
            [
                "git", "clone", "-q", "--no-local", "--depth=1", "--no-tags",
                "--branch", "synthetic", str(origin), str(pr_checkout),
            ],
            check=True,
        )
        event = pr_checkout / "event.json"
        event.write_text(
            '{"pull_request":{"base":{"sha":"' + protected_base + '"}}}'
        )
        subprocess.run(
            [
                "git", "fetch", "-q", "--no-tags", "--depth=1", "origin",
                protected_base,
            ],
            cwd=pr_checkout,
            check=True,
        )
        unrepaired = rederive.history.resolve_git_comparison(
            pr_checkout, protected_base, source="github_event"
        )
        assert unrepaired.status == "history_unavailable", unrepaired
        assert unrepaired.comparison_anchor is None, unrepaired
        env = {
            "GITHUB_ACTIONS": "true",
            "GITHUB_EVENT_NAME": "pull_request",
            "GITHUB_EVENT_PATH": str(event),
        }
        with mock.patch.dict(rederive.history.os.environ, env, clear=False):
            rederive.history.os.environ.pop("PULP_AGENT_CAPABILITY_BASE_REF", None)
            comparison = rederive.history._resolve_protected_comparison(
                pr_checkout, "missing-local"
            )
        assert comparison.status == "available", comparison
        assert comparison.source == "github_event", comparison
        assert comparison.resolved_base_tip == protected_base, comparison
        assert comparison.resolved_head == synthetic_head, comparison
        assert comparison.comparison_anchor == protected_base, comparison
        assert comparison.comparison_mode == "exact_base", comparison

        subprocess.run(
            [
                "git", "clone", "-q", "--no-local", "--depth=1", "--no-tags",
                "--branch", "feature", str(origin), str(dispatch_checkout),
            ],
            check=True,
        )
        subprocess.run(
            [
                "git", "fetch", "-q", "--no-tags", "--depth=1", "origin",
                "+" + protected_base + ":refs/remotes/origin/main",
            ],
            cwd=dispatch_checkout,
            check=True,
        )
        unrepaired = rederive.history.resolve_git_comparison(
            dispatch_checkout, "origin/main", source="local_ref"
        )
        assert unrepaired.status == "history_unavailable", unrepaired
        assert unrepaired.comparison_anchor is None, unrepaired
        dispatch_event = dispatch_checkout / "event.json"
        dispatch_event.write_text("{}")
        dispatch_env = {
            "GITHUB_ACTIONS": "true",
            "GITHUB_EVENT_NAME": "workflow_dispatch",
            "GITHUB_EVENT_PATH": str(dispatch_event),
        }
        with mock.patch.dict(
            rederive.history.os.environ, dispatch_env, clear=False
        ):
            rederive.history.os.environ.pop("PULP_AGENT_CAPABILITY_BASE_REF", None)
            comparison = rederive.history._resolve_protected_comparison(
                dispatch_checkout, "origin/main"
            )
        assert comparison.status == "available", comparison
        assert comparison.source == "local_ref", comparison
        assert comparison.requested_base == "origin/main", comparison
        assert comparison.resolved_base_tip == protected_base, comparison
        assert comparison.resolved_head == feature_head, comparison
        assert comparison.comparison_anchor == protected_base, comparison
        assert comparison.comparison_anchor != common_base, comparison
        assert comparison.comparison_mode == "exact_base", comparison

        dispatch_event.write_text(
            '{"pull_request":{"base":{"sha":"' + protected_base + '"}}}'
        )
        event_env = {
            "GITHUB_ACTIONS": "true",
            "GITHUB_EVENT_NAME": "pull_request",
            "GITHUB_EVENT_PATH": str(dispatch_event),
        }
        with mock.patch.dict(rederive.history.os.environ, event_env, clear=False):
            rederive.history.os.environ.pop("PULP_AGENT_CAPABILITY_BASE_REF", None)
            event_comparison = rederive.history._resolve_protected_comparison(
                dispatch_checkout, "missing-local"
            )
        assert event_comparison.status == "available", event_comparison
        assert event_comparison.source == "github_event", event_comparison
        assert event_comparison.comparison_anchor == protected_base, event_comparison
        assert event_comparison.comparison_anchor != common_base, event_comparison
        assert event_comparison.comparison_mode == "exact_base", event_comparison

        explicit_env = {"PULP_AGENT_CAPABILITY_BASE_REF": protected_base}
        with mock.patch.dict(
            rederive.history.os.environ, explicit_env, clear=False
        ):
            explicit_comparison = rederive.history._resolve_protected_comparison(
                dispatch_checkout, protected_base
            )
        assert explicit_comparison.status == "available", explicit_comparison
        assert explicit_comparison.source == "environment_base_ref", explicit_comparison
        assert explicit_comparison.comparison_anchor == protected_base, explicit_comparison
        assert explicit_comparison.comparison_anchor != common_base, explicit_comparison
        assert explicit_comparison.comparison_mode == "exact_base", explicit_comparison
    return 28


def exercise_git_unavailable_is_not_source_archive() -> int:
    history_document = rederive.history.json.loads(
        (ROOT / rederive.history.HISTORY_FILE).read_text()
    )
    manifest_document = rederive.history.json.loads(
        (ROOT / rederive.history.SNAPSHOT).read_text()
    )
    surface_document = rederive.history.json.loads(
        (ROOT / rederive.surface.SURFACE_SNAPSHOT).read_text()
    )
    with tempfile.TemporaryDirectory() as directory:
        repo = pathlib.Path(directory, "worktree")
        archive = pathlib.Path(directory, "archive")
        repo.mkdir()
        archive.mkdir()
        subprocess.run(["git", "init", "-q", "-b", "main"], cwd=repo, check=True)
        subprocess.run(["git", "config", "user.email", "t@t"], cwd=repo, check=True)
        subprocess.run(["git", "config", "user.name", "t"], cwd=repo, check=True)
        (repo / "seed.txt").write_text("seed\n")
        subprocess.run(["git", "add", "."], cwd=repo, check=True)
        subprocess.run(["git", "commit", "-qm", "seed"], cwd=repo, check=True)
        git_path = rederive.history.os.environ.get("PATH", "")
        with mock.patch.dict(
            rederive.history.os.environ, {"PATH": ""}, clear=True
        ):
            comparison = rederive.history._resolve_protected_comparison(repo, "HEAD")
            problems = rederive.history.protected_base_problems(
                repo, history_document, manifest_document, surface_document,
                comparison,
            )
        assert comparison.status == "command_failed", comparison
        assert problems and "history is unavailable" in problems[0], problems
        assert '"status":"command_failed"' in problems[0], problems
        with mock.patch.dict(
            rederive.history.os.environ, {"PATH": git_path}, clear=True
        ):
            available = rederive.history._resolve_protected_comparison(repo, "HEAD")
        assert available.status == "available", available
        with mock.patch.object(
            rederive.history, "_git_output", return_value=None
        ):
            supplied_problems = rederive.history.protected_base_problems(
                repo, history_document, manifest_document, surface_document,
                available,
            )
        assert not any(
            "history is unavailable" in problem
            for problem in supplied_problems
        ), supplied_problems
        archive_problems = rederive.history.protected_base_problems(
            archive, history_document, manifest_document, surface_document, None
        )
        assert archive_problems == [], archive_problems
    return 6


class _BaseFixture:
    """A repository whose protected base moved after a branch forked from it.

    `main` fingerprints core/a.hpp and carries one history entry. The branch
    forks, changes only notes.txt, and then `main` changes core/a.hpp, bumps
    the inventory, and appends a second history entry: the shape of a branch
    one counter behind main that carries none of main's newer entries.
    """

    HISTORY = "tools/agent-capabilities/contract-history.json"
    MANIFEST = "tools/scripts/agent_capability_manifest.py"

    def __init__(self, root: pathlib.Path) -> None:
        self.root = root
        self.git("init", "-q", "-b", "main")
        self.git("config", "user.email", "t@t")
        self.git("config", "user.name", "t")
        self._write_base(inventory=1, header="v1\n", entries=['{"n": 1}'])
        self.git("add", ".")
        self.git("commit", "-qm", "base")
        self.git("checkout", "-qb", "branch")
        (root / "notes.txt").write_text("branch work\n", encoding="utf-8")
        self.git("add", ".")
        self.git("commit", "-qm", "branch work")
        self.git("checkout", "-q", "main")
        self._write_base(inventory=2, header="v2\n", entries=['{"n": 1}', '{"n": 2}'])
        self.git("add", ".")
        self.git("commit", "-qm", "main moves the base")
        self.tip = self.git("rev-parse", "HEAD")
        self.git("checkout", "-q", "branch")

    def git(self, *arguments: str) -> str:
        return subprocess.run(
            ["git", *arguments], cwd=self.root, check=True,
            capture_output=True, text=True, encoding="utf-8",
        ).stdout.strip()

    def _write_base(self, inventory: int, header: str, entries: list[str]) -> None:
        def put(relative: str, text: str) -> None:
            path = self.root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")

        put("core/a.hpp", header)
        put(str(rederive.history.SNAPSHOT), '{"manifest_revision": 1}\n')
        put(
            str(rederive.surface.SURFACE_SNAPSHOT),
            '{"inventory_version": %d, "headers": [{"source": "core/a.hpp"}]}\n'
            % inventory,
        )
        put(self.HISTORY, '{"entries": [' + ", ".join(entries) + "]}\n")
        put(
            self.MANIFEST,
            "MANIFEST_REVISION = 1\nSURFACE_INVENTORY_VERSION = %d\n" % inventory,
        )

    def tracked_bytes(self) -> dict[str, bytes]:
        return {
            relative: (self.root / relative).read_bytes()
            for relative in (
                self.HISTORY, self.MANIFEST, str(rederive.history.SNAPSHOT),
                str(rederive.surface.SURFACE_SNAPSHOT),
            )
        }

    def resolve(self, root: pathlib.Path | None = None) -> tuple:
        with mock.patch.dict(
            rederive.history.os.environ,
            {"PULP_AGENT_CAPABILITY_BASE_REF": "main"},
        ):
            return rederive._resolve_base(root or self.root)

    def tree_json(self, relative: pathlib.Path) -> dict:
        return rederive.history.json.loads((self.root / relative).read_text(encoding="utf-8"))

    def run(self, *arguments: str) -> tuple[int, str]:
        """rederive's CLI against this repository, base pinned to `main`."""
        script = self.root / "tools/scripts/agent_capability_rederive.py"
        stderr = io.StringIO()
        with mock.patch.object(rederive, "__file__", str(script)), mock.patch.dict(
            rederive.history.os.environ,
            {"PULP_AGENT_CAPABILITY_BASE_REF": "main"},
        ), mock.patch.object(rederive.sys, "stderr", stderr):
            code = rederive.main(list(arguments))
        return code, stderr.getvalue()


def exercise_refuses_branch_behind_base() -> int:
    """A branch one counter behind main must refuse and write nothing."""
    with tempfile.TemporaryDirectory() as directory:
        fixture = _BaseFixture(pathlib.Path(directory))
        before = fixture.tracked_bytes()
        for arguments in ((), ("--print",)):
            code, stderr = fixture.run(*arguments)
            assert code == 1, (arguments, stderr)
            assert "does not contain the protected base " + fixture.tip in stderr, stderr
            assert "Merge origin/main first" in stderr, stderr
            assert "core/a.hpp" in stderr, stderr
        assert fixture.tracked_bytes() == before, "a refused rederive changed files"
        assert fixture.git("status", "--porcelain") == "", fixture.git("status")
    return 5


def exercise_allows_branch_with_base_merged() -> int:
    """Once main is merged, the guard passes and history keeps main's bytes."""
    with tempfile.TemporaryDirectory() as directory:
        fixture = _BaseFixture(pathlib.Path(directory))
        fixture.git("merge", "-q", "--no-edit", "main")
        base_history = fixture.git("show", "main:" + fixture.HISTORY) + "\n"
        base_manifest, base_surface, comparison = fixture.resolve()
        assert comparison.resolved_base_tip == fixture.tip, comparison
        assert base_surface["inventory_version"] == 2, base_surface
        # The merged tree holds main's material, so nothing moved: no counter
        # bump and so no history entry to append.
        derived = rederive.derive(
            fixture.tree_json(rederive.history.SNAPSHOT),
            fixture.tree_json(rederive.surface.SURFACE_SNAPSHOT),
            base_manifest, base_surface,
        )
        assert derived == (1, 2), derived
        merged_history = (fixture.root / fixture.HISTORY).read_text(encoding="utf-8")
        assert merged_history == base_history, merged_history
        assert merged_history.count('"n"') == 2, merged_history
    return 4


def exercise_allows_merge_in_progress_with_base() -> int:
    """An uncommitted merge whose incoming commit IS the base may derive."""
    with tempfile.TemporaryDirectory() as directory:
        fixture = _BaseFixture(pathlib.Path(directory))
        fixture.git("merge", "-q", "--no-commit", "--no-ff", "main")
        assert (fixture.root / ".git/MERGE_HEAD").read_text(encoding="utf-8").split()[0] == fixture.tip
        _, base_surface, comparison = fixture.resolve()
        assert comparison.resolved_base_tip == fixture.tip, comparison
        assert base_surface["inventory_version"] == 2, base_surface
    return 2


def exercise_refuses_unresolvable_base() -> int:
    """A base that cannot be resolved or proven refuses; it never skips."""
    with tempfile.TemporaryDirectory() as directory:
        fixture = _BaseFixture(pathlib.Path(directory))
        fixture.git("merge", "-q", "--no-edit", "main")
        with mock.patch.dict(
            rederive.history.os.environ,
            {"PULP_AGENT_CAPABILITY_BASE_REF": "no-such-ref"},
        ):
            try:
                rederive._resolve_base(fixture.root)
            except rederive.DeriveError as error:
                unresolved = str(error)
            else:
                raise AssertionError("an unresolvable base was not refused")
        assert "unavailable" in unresolved or "did not resolve" in unresolved, unresolved

        # A shallow clone of the merged branch has the base commit but not the
        # history that connects it to HEAD, so ancestry cannot be shown.
        shallow = pathlib.Path(directory, "shallow")
        subprocess.run(
            ["git", "clone", "-q", "--no-local", "--depth=1", "--branch", "branch",
             str(fixture.root), str(shallow)],
            check=True, capture_output=True,
        )
        subprocess.run(
            ["git", "fetch", "-q", "--depth=1", "origin", "main:main"],
            cwd=shallow, check=True, capture_output=True,
        )
        try:
            fixture.resolve(shallow)
        except rederive.DeriveError as error:
            shallow_error = str(error)
        else:
            raise AssertionError("a shallow clone derived without proving ancestry")
        assert fixture.tip in shallow_error, shallow_error
        assert "shallow clone" in shallow_error, shallow_error
    return 3


def main() -> int:
    checks = 0
    checks += exercise_refuses_branch_behind_base()
    checks += exercise_allows_branch_with_base_merged()
    checks += exercise_allows_merge_in_progress_with_base()
    checks += exercise_refuses_unresolvable_base()
    checks += exercise_reset_is_write_path_only()
    checks += exercise_prints_comparison_receipt()
    checks += exercise_proven_commit_absent_vs_malformed_authority()
    checks += exercise_github_event_failures_preserve_resolved_head()
    checks += exercise_github_event_repairs_exact_shallow_merge()
    checks += exercise_git_unavailable_is_not_source_archive()
    checks += exercise_refuses_mid_merge()
    checks += exercise_no_change()
    checks += exercise_manifest_only()
    checks += exercise_surface_only()
    checks += exercise_both()
    checks += exercise_counter_is_not_material()
    checks += exercise_rejects_underivable_base()
    checks += exercise_rewrite_round_trip()
    print(f"agent-capability-rederive: {checks} derivation contract checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
