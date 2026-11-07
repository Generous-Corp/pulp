"""A real merge queue's shape in a throwaway git repository, for the tests of
the merge-group steps in build.yml (the HOLD check and the queue breaker).

GitHub builds a batch as a chain of speculative groups: the first entry's group
is merge(main, entry), each later one is merge(previous group, entry), and every
group commit is a `Merge pull request #N from ...` commit. A group's
`merge_group.base_sha` is its first parent, so for every entry but the first it
is the previous speculative group, not main. A group that passes lands every
entry ahead of it, so a check that scans from `base_sha` sees only the last one.

`MergeQueueRepo` builds that chain for real, serves `git/commits/<sha>` reads
from it in the shape the GitHub REST API answers, and runs the
"Resolve the merge group's main base" step straight out of build.yml in a
depth-1 checkout, so a test drives the same base the workflow hands the checks.
Only the standard library is used.
"""
from __future__ import annotations

import os
import re
import subprocess
import tempfile
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional

ROOT = Path(__file__).resolve().parents[1].parent
BUILD_WORKFLOW = ROOT / ".github" / "workflows" / "build.yml"
RESOLVER = ROOT / "tools" / "scripts" / "resolve_classify_base.py"
RESOLVE_STEP = "- name: Resolve the merge group's main base"


def step_script(name: str) -> str:
    """The `run: |` body of the build.yml step headed `name`, dedented."""
    workflow = BUILD_WORKFLOW.read_text(encoding="utf-8")
    step = workflow[workflow.index(name):]
    body = step[step.index("run: |\n") + len("run: |\n"):]
    lines = []
    for line in body.splitlines():
        if line.strip() and not line.startswith("          "):
            break
        lines.append(line[10:])
    return "\n".join(lines) + "\n"


class MergeQueueRepo:
    """main (carrying the protected resolver) plus one branch per entry."""

    def __init__(self, entries: List[int]) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.remote = self.root / "remote.git"
        self.src = self.root / "source"
        subprocess.run(["git", "init", "-q", "--bare", str(self.remote)], check=True)
        subprocess.run(["git", "clone", "-q", str(self.remote), str(self.src)],
                       check=True, capture_output=True)
        for key, value in (("user.email", "ci@example.invalid"), ("user.name", "ci")):
            self.git("config", key, value)
        self.git("symbolic-ref", "HEAD", "refs/heads/main")
        resolver = self.src / "tools" / "scripts" / "resolve_classify_base.py"
        resolver.parent.mkdir(parents=True)
        resolver.write_bytes(RESOLVER.read_bytes())
        self.git("add", "tools/scripts/resolve_classify_base.py")
        self.git("commit", "-qm", "seed")
        self.main = self.git("rev-parse", "HEAD")
        self.git("push", "-q", "origin", "main")
        # The head each entry lands.
        self.heads: Dict[int, str] = {}
        for number in entries:
            self.git("checkout", "-qb", f"entry-{number}", self.main)
            (self.src / f"entry-{number}.txt").write_text(f"{number}\n", encoding="utf-8")
            self.git("add", f"entry-{number}.txt")
            self.git("commit", "-qm", f"entry {number}")
            self.heads[number] = self.git("rev-parse", "HEAD")
        # The speculative group of each entry, built in queue order.
        self.groups: Dict[int, str] = {}
        base = self.main
        for number in entries:
            self.git("checkout", "-q", "--detach", base)
            self.git("merge", "-q", "--no-ff", "-m",
                     f"Merge pull request #{number} from o/entry-{number}\n\nentry {number}",
                     f"entry-{number}")
            group = self.git("rev-parse", "HEAD")
            self.git("push", "-q", "origin",
                     f"{group}:refs/heads/gh-readonly-queue/main/pr-{number}-{base}")
            self.groups[number] = group
            base = group

    def close(self) -> None:
        self._tmp.cleanup()

    def git(self, *args: str, cwd: Optional[Path] = None) -> str:
        proc = subprocess.run(["git", *args], cwd=cwd or self.src, capture_output=True,
                              text=True, encoding="utf-8", check=True)
        return proc.stdout.strip()

    def base_sha(self, number: int) -> str:
        """`merge_group.base_sha` of `number`'s group: its first parent."""
        return self.git("rev-parse", f"{self.groups[number]}^1")

    def checkout(self, sha: str) -> Path:
        """actions/checkout at fetch-depth 1: one commit, no history."""
        work = Path(tempfile.mkdtemp(prefix=f"checkout-{sha[:8]}-", dir=self.root))
        self.git("init", "-q", cwd=work)
        self.git("remote", "add", "origin", self.remote.as_uri(), cwd=work)
        self.git("fetch", "-q", "--no-tags", "--depth=1", "origin", sha, cwd=work)
        self.git("checkout", "-q", "--detach", sha, cwd=work)
        return work

    def resolve_main_base(self, number: int, base_ref: str = "refs/heads/main") -> str:
        """Run build.yml's resolve step for `number`'s group; its `base` output."""
        work = self.checkout(self.groups[number])
        output = work / ".github-output"
        environ = {k: v for k, v in os.environ.items() if not k.startswith("GITHUB_")}
        environ.update(GITHUB_OUTPUT=str(output), GITHUB_EVENT_NAME="merge_group",
                       MERGE_GROUP_BASE_SHA=self.base_sha(number),
                       MERGE_GROUP_BASE_REF=base_ref)
        proc = subprocess.run(["bash", "-c", step_script(RESOLVE_STEP)], cwd=work,
                              env=environ, capture_output=True, text=True,
                              encoding="utf-8", timeout=120, check=False)
        if proc.returncode != 0:
            raise AssertionError(f"resolve step failed: {proc.stdout}{proc.stderr}")
        found = re.findall(r"^base=(.*)$", output.read_text(encoding="utf-8"), re.MULTILINE)
        if len(found) != 1:
            raise AssertionError(f"resolve step wrote {found!r}: {proc.stdout}{proc.stderr}")
        return found[0]

    def commit_json(self, sha: str) -> Dict[str, Any]:
        """`GET repos/{repo}/git/commits/{sha}`, read from the real repository."""
        raw = self.git("cat-file", "-p", sha)
        header, _, message = raw.partition("\n\n")
        parents = re.findall(r"^parent ([0-9a-f]{40})$", header, re.MULTILINE)
        return {"sha": sha, "parents": [{"sha": p} for p in parents], "message": message}

    def fetch(self, repo: str, rest: Callable[..., Any]) -> Callable[..., Any]:
        """A GitHub fetch whose commit reads come from this repository and
        whose every other read goes to `rest`."""
        prefix = f"repos/{repo}/git/commits/"

        def fetch(method: str, path: str, payload: Optional[Dict[str, Any]] = None) -> Any:
            if path.startswith(prefix):
                return self.commit_json(path[len(prefix):])
            return rest(method, path, payload)
        return fetch
