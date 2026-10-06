#!/usr/bin/env python3
"""Generate the changed-surface families for Python tool scripts and skill docs.

The changed-surface selector (`[targets.mac.changed_surface_selection]` in
`.shipyard/config.toml`) can bound a pull request's tests only when every
changed path belongs to a reviewed family that names the literal tests able
to observe that path. Python tool scripts already have that map: the
script-driven ctests record their entry script and every input they import or
name in `test/ctest_script_inputs.json`. This tool turns that map into one
family per group of scripts sharing the same readers, so a change to
`tools/scripts/foo.py` selects exactly the ctests that read it, and builds the
CMake targets whose products those ctests run.

A script is mapped only when nothing outside that map can reach it. It stays
unmapped, and the selector falls back to the full suite, when:

- native code, a shell script or a JavaScript file names it (a compiled test or
  a `pulp` command can run it, and no ctest input list records that), or CMake
  names it other than as the entry of a declared script test; this closes over
  every script such a script names in turn;
- one of its readers requires a CTest fixture, which a bounded run does not
  set up, or runs a build product no non-example target produces;
- it has no reader in the authoritative CTest corpus.

`.agents/skills/*/SKILL.md` maps to the declared script tests whose inputs
name the skills tree. Whole-tree tests (drift, lint, registry, sync, guard,
census, inventory and probe checks, the same names the test-receipt shadow
always runs) and environment-bound tests (a RESOURCE_LOCK, or a gpu,
browser-capture or audio-device label) are added through families whose paths
are every mapped path, so any bounded script or skill change also runs them.
Both sets are taken from the declared script tests, which exist in every
configuration; an undeclared environment-bound test reads nothing a mapped
script can reach without naming it.

    changed_surface_script_families.py --build-dir <dir> --write   # rewrite the block
    changed_surface_script_families.py --build-dir <dir> --check   # drift check
    changed_surface_script_families.py --static --base origin/main # no build: map flips only

The families live in `.shipyard/changed-surface-families.toml`, which the
selection names as its `families_file`. `--check` is diff-scoped like
`script_test_inputs.py --check`: drift blocks a change that touches a script,
a skill doc, the script-inputs list, this generator, the config or the
families file, because
that is when a stale family could bound the change wrongly; other drift (a
native file that started naming a script, say) is reported and blocks the
next change touching that script. A merge group reports drift without failing.

`--static` needs no build: it predicts the mapping of each script the change
adds, removes or re-reads, at the merge base and at HEAD, and blocks a mapping
flip the families file does not carry. It runs in the pre-push hook and in
gates.sh; the configured `--check` stays the authority when the two disagree.

Exit codes: 0 in sync, advisory drift, or written; 1 blocking drift; 2 an
input could not be read; 77 the build directory has no codemodel reply (a
skip, which ctest reports as one).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
from collections import Counter
from pathlib import Path
from typing import Any, Iterable

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "ci"))
import changed_surface_inventory as inventory  # noqa: E402
import script_test_inputs  # noqa: E402  (base resolution and merge-group advisory)

CONFIG = Path(".shipyard") / "config.toml"
SCRIPT_INPUTS = Path("test") / "ctest_script_inputs.json"
# Shipyard reads this from the protected base through the selection's
# `families_file` key, appends its families to the inline ones, and treats it
# as a policy path.
FAMILIES_FILE = Path(".shipyard") / "changed-surface-families.toml"
FAMILY_TABLE = "[[families]]"
SCRIPT_DIR = "tools/scripts/"
SKILL_DOC_PATTERN = ".agents/skills/*/SKILL.md"
SKILL_DOC_RE = re.compile(r"^\.agents/skills/[^/]+/SKILL\.md$")
SKILL_READER_RE = re.compile(r"\.agents|SKILL\.md")
# The test-receipt shadow's whole-tree names (tools/ci/test_receipts_shadow.py).
WHOLE_TREE_NAME_RE = re.compile(r"drift|census|registry|sync|guard|lint|inventory|probe", re.I)
NATIVE_SUFFIXES = (".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".mm", ".m", ".swift",
                   ".sh", ".bash", ".zsh", ".js", ".mjs", ".cjs", ".ts")
CMAKE_SUFFIXES = (".cmake", "CMakeLists.txt")
# Labels that tie a test to a shared host resource (the reuse replay's
# environment rule, plus audio devices).
ENVIRONMENT_LABELS = frozenset({"gpu", "browser-capture", "audio-device", "environment-bound"})
# Build-tree directories that hold example executables and plugin bundles.
EXAMPLE_PRODUCT_ROOTS = frozenset({"examples", "AU", "AUv3", "CLAP", "VST3", "LV2"})
# Prose and workflow files name scripts without executing them in a ctest.
NON_EXECUTING_PREFIXES = ("docs/", ".github/", "planning/", ".agents/", ".claude/", ".codex/")


SKIP_EXIT = 77


class GenerationError(RuntimeError):
    pass


def tracked_files(root: Path) -> list[str]:
    result = subprocess.run(["git", "-C", str(root), "ls-files", "-z"], check=True,
                            capture_output=True)
    return sorted(p for p in result.stdout.decode("utf-8", "replace").split("\0") if p)


def read_text(root: Path, rel: str) -> str:
    try:
        return (root / rel).read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def top_level_scripts(tracked: Iterable[str]) -> list[str]:
    return sorted(p for p in tracked
                  if p.startswith(SCRIPT_DIR) and p.endswith(".py") and "/" not in p[len(SCRIPT_DIR):])


def _without_comments_and_add_tests(cmake: str) -> str:
    """CMake text minus `#` comments and every add_test(...) call: what is
    left can run a script at configure or build time."""
    lines = []
    for line in cmake.splitlines():
        quoted = False
        for i, ch in enumerate(line):
            if ch == '"':
                quoted = not quoted
            elif ch == "#" and not quoted:
                line = line[:i]
                break
        lines.append(line)
    text, kept, pos = "\n".join(lines), [], 0
    for match in re.finditer(r"\badd_test\s*\(", text):
        if match.start() < pos:
            continue
        kept.append(text[pos:match.start()])
        depth, pos = 1, match.end()
        while pos < len(text) and depth:
            depth += {"(": 1, ")": -1}.get(text[pos], 0)
            pos += 1
    kept.append(text[pos:])
    return "".join(kept)


def seeds_reachability(script: str, native_text: str, cmake_text: str, cmake_running: str,
                       entries: set[str]) -> bool:
    """Whether code outside the declared script tests names `script` directly:
    native code at all, CMake anywhere for a script that is no test's entry,
    and CMake outside add_test and comments for one that is (a custom command
    or a configure-time call still runs it). native_reachable() and the
    static Prediction both decide their seeds here, so they cannot disagree."""
    name = os.path.basename(script)
    return name in native_text or (name in cmake_text and script not in entries) \
        or (script in entries and name in cmake_running)


_WORD = re.compile(r"\w+")


def word_set(text: str) -> frozenset[str]:
    return frozenset(_WORD.findall(text))


def names_stem(stem: str, text: str, words: frozenset[str]) -> bool:
    """Whether `text` names `stem` as a whole word, as `\\bstem\\b` decides.
    A stem made only of word characters matches exactly when it is one of the
    text's maximal word runs, so `words` (word_set(text)) answers it without a
    regex scan; any other stem falls back to the regex."""
    if _WORD.fullmatch(stem):
        return stem in words
    return re.search(r"\b" + re.escape(stem) + r"\b", text) is not None


def native_reachable(scripts: list[str], tracked: list[str], entries: set[str],
                     text: Any) -> set[str]:
    """Scripts that code outside the declared script tests can run."""
    native = [p for p in tracked if p.endswith(NATIVE_SUFFIXES)
              and not p.startswith(NON_EXECUTING_PREFIXES)]
    cmake = [p for p in tracked if p.endswith(CMAKE_SUFFIXES) and not p.startswith(NON_EXECUTING_PREFIXES)]
    native_text = "\n".join(text(p) for p in native)
    cmake_text = "\n".join(text(p) for p in cmake)
    cmake_running = "\n".join(_without_comments_and_add_tests(text(p)) for p in cmake)
    reached = {script for script in scripts
               if seeds_reachability(script, native_text, cmake_text, cmake_running, entries)}
    pending = sorted(reached)
    while pending:
        body = text(pending.pop())
        words = word_set(body)
        for script in scripts:
            if script in reached:
                continue
            if names_stem(os.path.basename(script)[:-3], body, words):
                reached.add(script)
                pending.append(script)
    return reached


def readers_of(path: str, declared: dict[str, dict]) -> set[str]:
    readers = set()
    for name, entry in declared.items():
        inputs = [i.rstrip("/") for i in entry.get("inputs") or []] + [entry.get("entry") or ""]
        if any(path == i or path.startswith(i + "/") for i in inputs if i):
            readers.add(name)
    return readers


def producer_targets(tests: list[dict], model: inventory.CodeModel) -> tuple[dict[str, set[str]], set[str]]:
    """The CMake targets whose products each test runs, and the tests a
    bounded run cannot be trusted to satisfy.

    A test is unsatisfiable when it needs a CTest fixture whose setup is not
    registered in this CTest inventory, or names a build-tree file that no
    non-example target produces. CTest runs a registered FIXTURES_SETUP test
    automatically for a bounded selection, so a complete fixture graph is
    safe to map. Example targets exist only when examples are configured, so a
    reader of their products would map differently per configuration; it
    blocks instead, in every configuration alike."""
    owner: dict[str, str] = {}
    for target in model.targets.values():
        if target.source_dir == "examples" or target.source_dir.startswith("examples/"):
            continue
        for artifact in target.artifacts:
            path = artifact if os.path.isabs(artifact) else os.path.join(model.build_root, artifact)
            owner[os.path.normpath(path)] = target.name
    build_root = os.path.normpath(model.build_root)
    targets: dict[str, set[str]] = {}
    unsatisfiable: set[str] = set()
    fixture_setup_tests: dict[str, set[str]] = {}
    provided_fixtures: set[str] = set()
    for test in tests:
        props = {p.get("name"): p.get("value") for p in test.get("properties") or []}
        setup = props.get("FIXTURES_SETUP")
        if isinstance(setup, list):
            fixtures = {str(fixture) for fixture in setup}
        elif setup:
            fixtures = {str(setup)}
        else:
            fixtures = set()
        provided_fixtures.update(fixtures)
        for fixture in fixtures:
            fixture_setup_tests.setdefault(fixture, set()).add(test["name"])
    for test in tests:
        props = {p.get("name"): p.get("value") for p in test.get("properties") or []}
        required = props.get("FIXTURES_REQUIRED")
        if isinstance(required, list):
            required_fixtures = {str(fixture) for fixture in required}
        elif required:
            required_fixtures = {str(required)}
        else:
            required_fixtures = set()
        if required_fixtures - provided_fixtures:
            unsatisfiable.add(test["name"])
        words = list(test.get("command") or [])
        for key in ("ENVIRONMENT", "REQUIRED_FILES"):
            value = props.get(key)
            words.extend(value if isinstance(value, list) else [value] if value else [])
        needed = targets.setdefault(test["name"], set())
        for word in words:
            for token in re.split(r"[=:;,\s]", str(word)):
                if not token or not os.path.isabs(token):
                    continue
                path = os.path.normpath(token)
                if path in owner:
                    needed.add(owner[path])
                elif path.startswith(build_root + os.sep) and (
                        Path(path).relative_to(build_root).parts[0] in EXAMPLE_PRODUCT_ROOTS
                        or os.path.splitext(path)[1] not in (".json", ".txt", ".log", "")):
                    # A build product nothing in this configuration owns.
                    unsatisfiable.add(test["name"])
    # CTest executes registered FIXTURES_SETUP tests as part of a bounded
    # reader selection. Include their executable targets in the family so the
    # setup product is built before the reader runs.
    for test in tests:
        props = {p.get("name"): p.get("value") for p in test.get("properties") or []}
        required = props.get("FIXTURES_REQUIRED")
        if isinstance(required, list):
            required_fixtures = {str(fixture) for fixture in required}
        elif required:
            required_fixtures = {str(required)}
        else:
            required_fixtures = set()
        needed = targets.setdefault(test["name"], set())
        for fixture in required_fixtures:
            for setup_name in fixture_setup_tests.get(fixture, set()):
                needed.update(targets.get(setup_name, set()))
    return targets, unsatisfiable


def generate(root: Path, tests: list[dict], model: inventory.CodeModel) -> list[dict[str, Any]]:
    declared_all = json.loads((root / SCRIPT_INPUTS).read_text(encoding="utf-8"))["tests"]
    authoritative = {t["name"] for t in inventory.authoritative_tests(tests)}
    counts = Counter(t["name"] for t in tests)
    duplicated = {name for name, count in counts.items() if count > 1}
    products, unsatisfiable = producer_targets(tests, model)
    # A reader outside the authoritative corpus does not run in the full suite
    # either, so it is dropped rather than counted. A reader the bounded run
    # cannot satisfy, or whose name is ambiguous, blocks every path it reads.
    declared = {n: e for n, e in declared_all.items() if n in authoritative}
    blocked_readers = (duplicated | unsatisfiable) & set(declared)

    tracked = tracked_files(root)
    cache: dict[str, str] = {}

    def text(rel: str) -> str:
        if rel not in cache:
            cache[rel] = read_text(root, rel)
        return cache[rel]

    scripts = top_level_scripts(tracked)
    entries = {e.get("entry") for e in declared_all.values()}
    reached = native_reachable(scripts, tracked, entries, text)

    groups: dict[tuple[str, ...], list[str]] = {}
    for script in scripts:
        if script in reached:
            continue
        readers = readers_of(script, declared)
        if not readers or readers & blocked_readers:
            continue
        groups.setdefault(tuple(sorted(readers)), []).append(script)

    skill_readers = set()
    for name, entry in declared.items():
        for rel in [entry.get("entry") or ""] + list(entry.get("inputs") or []):
            if rel and (root / rel).is_file() and SKILL_READER_RE.search(text(rel)):
                skill_readers.add(name)
                break
    skill_docs = [p for p in tracked if SKILL_DOC_RE.match(p)]
    families: list[dict[str, Any]] = []
    for readers, paths in sorted(groups.items(), key=lambda item: item[1][0]):
        families.append(_family(f"script-readers-{_digest(readers)}", paths, list(readers), products))
    if skill_docs and skill_readers and not skill_readers & blocked_readers:
        families.append(_family("agent-skill-docs", [SKILL_DOC_PATTERN], sorted(skill_readers), products))

    whole_tree = sorted(n for n in declared if WHOLE_TREE_NAME_RE.search(n))
    if families:
        if not whole_tree or set(whole_tree) & blocked_readers:
            raise GenerationError("whole-tree tests are missing or cannot run bounded; "
                                  "refusing to bound script changes")
        env_bound = sorted(n for n in declared if n in environment_bound(tests))
        if set(env_bound) & blocked_readers:
            raise GenerationError("environment-bound tests cannot run bounded: "
                                  + ", ".join(sorted(set(env_bound) & blocked_readers)))
        covered = sorted({p for f in families for p in f["paths"]})
        families.append(_family("script-surface-whole-tree", covered, whole_tree, products))
        if env_bound:
            families.append(_family("script-surface-environment-bound", covered, env_bound, products))
    return families


def environment_bound(tests: list[dict]) -> set[str]:
    """Tests whose outcome depends on a shared host resource rather than on
    their inputs: a RESOURCE_LOCK, or a gpu, browser-capture or audio-device
    label, or the explicit `environment-bound` label for a test whose host
    dependence no other label states. Their failures cannot be predicted from a diff, so a bounded run
    always includes them."""
    bound = set()
    for test in tests:
        props = {p.get("name"): p.get("value") for p in test.get("properties") or []}
        # CTest's JSON export serializes cache/property booleans as strings in
        # some generators (for example ``"TRUE"``), while synthetic fixtures
        # and older exports may carry a native bool.  Normalize both forms so
        # an optional browser/GPU test cannot accidentally become a bounded
        # required test merely because the exporter changed representation.
        optional = props.get("PULP_OPTIONAL")
        if optional is True or (
                isinstance(optional, str) and optional.strip().upper() == "TRUE"):
            continue
        labels = props.get("LABELS") or []
        if props.get("RESOURCE_LOCK") or ENVIRONMENT_LABELS & set(labels):
            bound.add(test["name"])
    return bound


def _digest(readers: Iterable[str]) -> str:
    return hashlib.sha256("\0".join(sorted(readers)).encode()).hexdigest()[:12]


def _family(name: str, paths: list[str], tests: list[str],
            products: dict[str, set[str]]) -> dict[str, Any]:
    build_targets = {"pulp-cli"}.union(*(products.get(test, set()) for test in tests))
    return {
        "name": name,
        "paths": sorted(paths),
        "tests": sorted(set(tests)),
        "build_targets": sorted(build_targets),
        "supported_build_types": ["debug", "release"],
        "risk_class": "low",
    }


def _toml_list(key: str, values: list[str]) -> list[str]:
    if len(values) == 1:
        return [f"{key} = [{json.dumps(values[0])}]"]
    return [f"{key} = ["] + [f"  {json.dumps(v)}," for v in values] + ["]"]


def render(families: list[dict[str, Any]]) -> str:
    lines = ["# Generated by tools/scripts/changed_surface_script_families.py --write.",
             "# Do not edit by hand. Regenerate after test/ctest_script_inputs.json or a",
             "# script's callers change; `changed-surface-script-families-drift` checks it.",
             "# Resolve a merge conflict in this file by regenerating, never by hand."]
    for family in families:
        lines += ["", FAMILY_TABLE, f"name = {json.dumps(family['name'])}"]
        lines += _toml_list("paths", family["paths"])
        lines += _toml_list("tests", family["tests"])
        lines += _toml_list("build_targets", family["build_targets"])
        lines += _toml_list("supported_build_types", family["supported_build_types"])
        lines.append(f"risk_class = {json.dumps(family['risk_class'])}")
    return "\n".join(lines) + "\n"


# Static pre-check. `--check` needs a configured build (the ctest inventory and
# the codemodel reply), so a fresh worktree cannot run it before a push, and a
# script added without regenerating the families file used to surface only on
# the required gate. `--static` predicts each affected script's mapping from
# the committed test/ctest_script_inputs.json and the tree, with the reader and
# reachability rules generate() uses, at the merge base and at HEAD. What the
# prediction cannot see without a build (the authoritative corpus, fixtures,
# build products) is cancelled by comparing against the base: a script is
# flagged only when its prediction agreed with the families file at the base
# and disagrees at HEAD. A mapping flip blocks; a reader-set change is
# reported, since a reader the full suite excludes by label would not appear in
# a regenerated file.

def _git(root: Path, *args: str, stdin: bytes | None = None) -> bytes:
    command = ["git", "-C", str(root), *args]
    if stdin is None:
        return subprocess.run(command, check=True, capture_output=True).stdout
    # Keep both sides out of pipes.  `git cat-file --batch` can emit a large
    # response before consuming all object IDs; feeding it through
    # subprocess.run(input=...) or capturing its response through a pipe can
    # then deadlock when either pipe fills.
    with tempfile.TemporaryFile() as request, tempfile.TemporaryFile() as response:
        request.write(stdin)
        request.seek(0)
        subprocess.run(command, stdin=request, stdout=response, check=True,
                       stderr=subprocess.PIPE)
        response.seek(0)
        return response.read()


class Snapshot:
    """A revision's tracked files, read from the object store in one batch."""

    def __init__(self, root: Path, rev: str, blobs: dict[str, str]) -> None:
        self.root = root
        self.files: dict[str, str] = {}
        for record in _git(root, "ls-tree", "-r", "-z", rev).decode("utf-8", "replace").split("\0"):
            meta, _, path = record.partition("\t")
            parts = meta.split()
            if len(parts) == 3 and parts[1] == "blob":
                self.files[path] = parts[2]
        self.blobs = blobs

    def load(self, paths: Iterable[str]) -> None:
        wanted = sorted({self.files[p] for p in paths if p in self.files} - set(self.blobs))
        if not wanted:
            return
        out = _git(self.root, "cat-file", "--batch", stdin=("\n".join(wanted) + "\n").encode())
        pos = 0
        while pos < len(out):
            end = out.index(b"\n", pos)
            header = out[pos:end].split()
            pos = end + 1
            if len(header) < 3:
                continue
            size = int(header[2])
            self.blobs[header[0].decode()] = out[pos:pos + size].decode("utf-8", "replace")
            pos += size + 1

    def text(self, rel: str) -> str:
        sha = self.files.get(rel)
        if sha is None:
            return ""
        if sha not in self.blobs:
            self.load([rel])
        return self.blobs.get(sha, "")


class Prediction:
    """generate()'s mapping rule for one revision, without a configured build."""

    def __init__(self, snap: Snapshot, declared: dict[str, dict]) -> None:
        self.snap = snap
        self.declared = declared
        files = sorted(snap.files)
        self.scripts = top_level_scripts(files)
        self.entries = {e.get("entry") for e in declared.values()}
        native = [p for p in files if p.endswith(NATIVE_SUFFIXES) and not p.startswith(NON_EXECUTING_PREFIXES)]
        cmake = [p for p in files if p.endswith(CMAKE_SUFFIXES) and not p.startswith(NON_EXECUTING_PREFIXES)]
        snap.load(native + cmake + self.scripts)
        self.native_text = "\n".join(snap.text(p) for p in native)
        self.cmake_text = "\n".join(snap.text(p) for p in cmake)
        self.cmake_running = "\n".join(_without_comments_and_add_tests(snap.text(p)) for p in cmake)
        self.memo: dict[str, bool] = {}
        self.words: dict[str, frozenset[str]] = {}

    def _words(self, script: str) -> frozenset[str]:
        if script not in self.words:
            self.words[script] = word_set(self.snap.text(script))
        return self.words[script]

    def _seed(self, script: str) -> bool:
        return seeds_reachability(script, self.native_text, self.cmake_text, self.cmake_running,
                                  self.entries)

    def reached(self, script: str) -> bool:
        """native_reachable() for one script, walked backwards: it is reached
        when it is a seed or a reached script names its stem."""
        if script in self.memo:
            return self.memo[script]
        seen, queue = {script}, [script]
        while queue:
            node = queue.pop()
            if self.memo.get(node) or self._seed(node):
                self.memo[script] = True
                return True
            stem = os.path.basename(node)[:-3]
            for other in self.scripts:
                if other in seen or self.memo.get(other) is False:
                    continue
                if names_stem(stem, self.snap.text(other), self._words(other)):
                    seen.add(other)
                    queue.append(other)
        for node in seen:
            self.memo[node] = False
        return False

    def readers(self, script: str) -> frozenset[str] | None:
        """The predicted readers, or None when the script stays unmapped."""
        if script not in self.snap.files:
            return None
        readers = readers_of(script, self.declared)
        if not readers or self.reached(script):
            return None
        return frozenset(readers)


def parse_families(text: str) -> dict[str, frozenset[str]]:
    """Each script's readers in the generated file (its script-readers family)."""
    try:
        import tomllib
        body = tomllib.loads(text) if text else {}
        families = body.get("families", [])
    except ModuleNotFoundError:
        families = _parse_rendered(text)
    owners: dict[str, frozenset[str]] = {}
    for family in families:
        if str(family.get("name", "")).startswith("script-readers-"):
            for path in family.get("paths", []):
                owners[path] = frozenset(family.get("tests", []))
    return owners


def _parse_rendered(text: str) -> list[dict[str, Any]]:
    """render()'s output read back without tomllib (Python before 3.11): one
    key per line, a list either inline or one JSON string per line."""
    families: list[dict[str, Any]] = []
    key, values = None, []
    for line in text.splitlines():
        stripped = line.strip()
        if stripped == FAMILY_TABLE:
            families.append({})
        elif key is not None:
            if stripped == "]":
                families[-1][key], key = values, None
            else:
                values.append(json.loads(stripped.rstrip(",")))
        elif families and " = " in stripped:
            name, _, value = stripped.partition(" = ")
            if value == "[":
                key, values = name, []
            else:
                families[-1][name] = json.loads(value)
    return families


def static_drift(root: Path, base: str, head: str = "HEAD") -> tuple[list[str], list[str], int]:
    """(blocking lines, advisory lines, scripts examined) for ``head`` against ``base``."""
    mb = subprocess.run(["git", "-C", str(root), "merge-base", base, head], capture_output=True, text=True)
    anchor = mb.stdout.strip() if mb.returncode == 0 and mb.stdout.strip() else base
    changed = {p for p in _git(root, "diff", "--name-only", "-z", anchor, head)
               .decode("utf-8", "replace").split("\0") if p}
    blobs: dict[str, str] = {}
    snaps = {"base": Snapshot(root, anchor, blobs), "head": Snapshot(root, head, blobs)}
    declared, owners = {}, {}
    for side, snap in snaps.items():
        listing = snap.text(str(SCRIPT_INPUTS))
        declared[side] = json.loads(listing)["tests"] if listing else {}
        owners[side] = parse_families(snap.text(str(FAMILIES_FILE)))
    scripts = set(top_level_scripts(snaps["base"].files)) | set(top_level_scripts(snaps["head"].files))
    affected = {p for p in changed if p in scripts}
    if declared["base"] != declared["head"]:
        affected |= {s for s in scripts
                     if readers_of(s, declared["base"]) != readers_of(s, declared["head"])}
    affected |= {s for s in scripts if owners["base"].get(s) != owners["head"].get(s)}
    if not affected:
        return [], [], 0
    predict = {side: Prediction(snaps[side], declared[side]) for side in snaps}
    blocking, advisory = [], []
    for script in sorted(affected):
        pb, ph = predict["base"].readers(script), predict["head"].readers(script)
        fb, fh = owners["base"].get(script), owners["head"].get(script)
        if (pb is None) == (fb is None) and (ph is None) != (fh is None):
            blocking.append(f"{script}: {'newly mapped' if ph else 'no longer mapped'}")
        elif ph and fh and ph != fh and not (pb and fb and (pb - fb, fb - pb) == (ph - fh, fh - ph)):
            advisory.append(f"{script}: readers +{sorted(ph - fh)[:3]} -{sorted(fh - ph)[:3]}")
    return blocking, advisory, len(affected)


def static_main(root: Path, base: str | None, head: str = "HEAD") -> int:
    if base is None:
        print("changed-surface script families (static): no base to compare against; pass --base")
        return 2
    try:
        blocking, advisory, examined = static_drift(root, base, head)
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        print(f"changed-surface script families (static): cannot predict: {error}", file=sys.stderr)
        return 2
    for line in advisory:
        print(f"changed-surface script families (static): note: {line} (a regeneration may "
              "differ; the configured --check decides)")
    if not blocking:
        print(f"changed-surface script families (static): OK, {examined} affected script(s) "
              f"predicted against {base}")
        return 0
    print(f"changed-surface script families (static): {FAMILIES_FILE} was not regenerated for "
          f"{len(blocking)} script(s) this change maps or unmaps:", file=sys.stderr)
    for line in blocking[:12]:
        print(f"  {line}", file=sys.stderr)
    print("  regenerate from a configured build (`tools/scripts/gates.sh` configures build-gate "
          "and prints the exact command), then commit the file:\n"
          "  python3 tools/scripts/changed_surface_script_families.py --build-dir <dir> --write",
          file=sys.stderr)
    if script_test_inputs.advisory_here():
        return 0
    return 1


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--repo-root", type=Path, default=HERE.parents[1])
    parser.add_argument("--build-dir", type=Path,
                        help="a configured build with a codemodel reply (required except with --static)")
    parser.add_argument("--base", default=None,
                        help="diff-scope --check to changes since this ref (default: as "
                             "script_test_inputs.py --check resolves it; none means a full check)")
    parser.add_argument("--head", default="HEAD", help="--static: the revision to check (default HEAD)")
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--write", action="store_true")
    mode.add_argument("--check", action="store_true")
    mode.add_argument("--static", action="store_true",
                      help="predict, without a build, whether this change maps or unmaps a script "
                           "the families file was not regenerated for")
    args = parser.parse_args(argv)
    root = args.repo_root.resolve()
    if args.static:
        return static_main(root, script_test_inputs.resolve_base(root, args.base), args.head)
    if args.build_dir is None:
        parser.error("--build-dir is required with --write and --check")
    families_path = root / FAMILIES_FILE
    if not inventory.codemodel_reply_available(args.build_dir):
        # Without the file-API reply the producer targets cannot be read. The
        # gate and the Shipyard lane both request it before configuring.
        print(f"changed-surface script families: SKIP: {args.build_dir} has no CMake file-API "
              "codemodel reply (touch .cmake/api/v1/query/codemodel-v2 and reconfigure)")
        return SKIP_EXIT
    try:
        current = families_path.read_text(encoding="utf-8") if families_path.is_file() else ""
        tests = inventory.load_ctest_json(args.build_dir)
        model = inventory.load_codemodel_targets(args.build_dir)
        updated = render(generate(root, tests, model))
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError, GenerationError) as error:
        print(f"changed-surface script families: cannot generate: {error}", file=sys.stderr)
        return 2
    if args.write:
        if updated != current:
            families_path.write_text(updated, encoding="utf-8")
        return 0
    if updated == current:
        print(f"changed-surface script families: OK, {current.count(FAMILY_TABLE)} "
              f"generated families checked against {args.build_dir}")
        return 0
    fix = ("regenerate with\n  python3 tools/scripts/changed_surface_script_families.py "
           "--build-dir <dir> --write")
    base = script_test_inputs.resolve_base(root, args.base)
    touched = sorted(script_test_inputs.changed_files(root, base)) if base else None
    if touched is not None and not any(blocks_on(path) for path in touched):
        print(f"changed-surface script families: note: {FAMILIES_FILE} drifted, but this change touches "
              f"none of its inputs; the next change to a mapped surface must {fix}")
        return 0
    print(f"changed-surface script families drifted in {FAMILIES_FILE}; {fix}", file=sys.stderr)
    for line in describe_drift(current, updated)[:12]:
        print(f"  {line}", file=sys.stderr)
    if script_test_inputs.advisory_here():
        print("::warning title=changed-surface script families stale (advisory in a merge group)::"
              "regenerate on the next push")
        return 0
    return 1


def describe_drift(current: str, regenerated: str) -> list[str]:
    """Which mapped paths a regeneration would add, drop or move."""
    def mapping(text: str) -> dict[str, set[str]]:
        import tomllib
        try:
            body = tomllib.loads(text)
        except tomllib.TOMLDecodeError:
            return {}
        owners: dict[str, set[str]] = {}
        for family in body.get("families", []):
            for path in family["paths"]:
                owners.setdefault(path, set()).update(family["tests"])
        return owners
    old, new = mapping(current), mapping(regenerated)
    lines = []
    for path in sorted(set(old) | set(new)):
        if path not in new:
            lines.append(f"{path}: no longer mapped")
        elif path not in old:
            lines.append(f"{path}: newly mapped")
        elif old[path] != new[path]:
            added, dropped = sorted(new[path] - old[path]), sorted(old[path] - new[path])
            lines.append(f"{path}: readers +{added[:3]} -{dropped[:3]}")
    return lines


def blocks_on(path: str) -> bool:
    """A changed path that a stale generated family could bound wrongly."""
    return (path in {str(CONFIG), str(FAMILIES_FILE), str(SCRIPT_INPUTS),
                     "tools/scripts/changed_surface_script_families.py"}
            or (path.startswith(SCRIPT_DIR) and path.endswith(".py"))
            or bool(SKILL_DOC_RE.match(path)))


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
