#!/usr/bin/env python3
"""Run the editor-host acceptance test on macOS against an installed Vellum SDK.

    python3 tools/scripts/editor_host_acceptance.py --vellum-prefix <vellum-sdk>

The acceptance test is section 3 of the Phase 5/6 UI-split plan: one processor
(a gain parameter and a meter channel) whose audio code never changes, built
headless, with a Vellum editor, with an RmlUi editor and with a bring-your-own
native editor, and hosted in-process by Pulp's VST3, CLAP and AU v2 adapters
and its standalone app through the editor-host contract.

The runner configures a build directory with
PULP_EDITOR_HOST_SLICE_VELLUM_PREFIX, builds the editor-host slice
(test/editor_host_slice) through tools/ci/governed-build.sh, runs its CTest
label and Pulp's core -> Vellum direction check, and prints one verdict per
acceptance item. Every check is a named line a harness printed ("[kind] ok:
...") or a named CTest result; an item passes only when all of its checks are
seen to pass. An item this tree cannot run yet is SKIP with the reason, never
PASS, and a run with any SKIP says so in its summary.

Item 3g is a mutation check the runner performs itself: through
tools/scripts/confirm_failure.sh it removes the RmlUi editor's gesture calls,
rebuilds the RmlUi slice, and requires the harness to fail, then restores the
source and requires it to pass again. confirm_failure.sh restores through git,
so it needs that editor source unmodified; with --skip-build the mutation is
not run and 3g is SKIP.

Exit status: 0 when no item failed (SKIPs allowed), 1 when any item failed,
2 when the run could not start (no Vellum SDK, configure or build failure).
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import os
import re
import subprocess
import sys
from pathlib import Path
from typing import Iterable

REPO_ROOT = Path(__file__).resolve().parents[2]
SLICE_LABEL = "editor-host-slice"
SLICE_TARGET = "pulp-editor-host-slice"

PASS, FAIL, SKIP = "PASS", "FAIL", "SKIP"


# ── What each acceptance item requires ──────────────────────────────────────


@dataclasses.dataclass(frozen=True)
class TestPassed:
    """A CTest test that must have run and passed."""

    name: str

    def describe(self) -> str:
        return f"ctest {self.name}"


@dataclasses.dataclass(frozen=True)
class Line:
    """A harness check that must have printed "[tag] ok: <text>" and never
    "[tag] FAIL: <text>"."""

    tag: str
    text: str

    def describe(self) -> str:
        return f"[{self.tag}] {self.text}"


@dataclasses.dataclass(frozen=True)
class TagClean:
    """Every check under a harness tag passed, and there were at least `minimum`."""

    tag: str
    minimum: int

    def describe(self) -> str:
        return f"every [{self.tag}] check (at least {self.minimum})"


@dataclasses.dataclass(frozen=True)
class Mutation:
    """A confirm_failure.sh run that must report CONFIRMED: the harness passes
    with the code and fails with the named piece of it removed."""

    name: str

    def describe(self) -> str:
        return f"mutation {self.name}"


@dataclasses.dataclass(frozen=True)
class Direction:
    """Pulp's core -> Vellum direction check passed."""

    def describe(self) -> str:
        return "core_vellum_direction.py"


@dataclasses.dataclass(frozen=True)
class Item:
    key: str
    title: str
    checks: tuple = ()
    skip_reason: str = ""
    # A CTest test the build registers instead of this item's checks when an
    # optional SDK component is missing. When it passed and none of the
    # item's own tests ran, the item is SKIP with `absent_reason`.
    absent_marker: str = ""
    absent_reason: str = ""


def _format_checks(kind: str) -> tuple:
    checks: list = [TestPassed(f"pulp-editor-host-formats-{kind}")]
    for fmt in ("vst3", "clap", "au"):
        tag = f"{kind}/{fmt}"
        checks += [
            TagClean(tag, 10),
            Line(tag, "attach to a real NSView"),
            Line(tag, "a knob drag through the editor"),
            Line(tag, "no gesture is left open after closing"),
            Line(tag, "the reopened editor shows the surviving value"),
        ]
    return tuple(checks)


def _standalone_checks(kind: str) -> tuple:
    tag = f"{kind}/standalone"
    return (
        TestPassed(f"pulp-editor-host-standalone-{kind}"),
        Line(tag, "launch, open the editor window, capture, quit"),
        Line(tag, "the screenshot is the editor's PNG frame"),
        Line(tag, "Space, forwarded by the editor, stopped the host transport"),
        Line(tag, "no audio device was created"),
    )


ITEMS: tuple = (
    Item("1", "Headless: render with no UI package; no Vellum, RmlUi, Skia or Dawn symbols", (
        TestPassed("pulp-editor-host-slice-headless"),
        TestPassed("pulp-editor-host-slice-symbols"),
        Line("headless", "this binary links no editor factory"),
    )),
    Item("2a", "With Vellum: the same render is byte-identical", (
        TestPassed("pulp-editor-host-slice-vellum"),
        TestPassed("pulp-editor-host-slice-audio-identical"),
        Line("vellum", "audio with an editor attached is byte-identical"),
    )),
    Item("2b", "With Vellum: VST3, CLAP and AU v2 editors in process "
               "(open, attach, resize, close, reopen, close; gestures released; value survives)",
         _format_checks("vellum")),
    Item("2c", "With Vellum: editor and inspector together, with a parameter round trip", (
        Line("vellum", "an inspector opens beside the editor"),
        Line("vellum", "the editor follows the inspector's edit"),
    )),
    Item("2d", "With Vellum: Space with no focused field is forwarded to the host", (
        Line("vellum", "Space with no focused field is forwarded to the host"),
        Line("vellum/vst3", "Space is forwarded to the host"),
        Line("vellum/clap", "Space is forwarded to the host"),
        Line("vellum/au", "Space is forwarded to the host"),
    )),
    Item("2e", "With Vellum: Space is consumed when a text field has focus", (
        Line("vellum", "the editor's text field takes focus"),
        Line("vellum", "Space is consumed when a text field has focus"),
        Line("vellum", "the focused text field received the Space"),
        Line("vellum", "Space is forwarded again once the field loses focus"),
    )),
    Item("2f", "With Vellum: standalone launch, window, screenshot, quit",
         _standalone_checks("vellum")),
    Item("3a", "With RmlUi: soft-rendered .rml frame equals the golden image byte for byte", (
        TestPassed("pulp-editor-host-slice-rmlui"),
        Line("rmlui", "the frame at gain 0.75 equals the module's golden image byte for byte"),
    )),
    Item("3b", "With RmlUi: an empty document as the golden image's negative control", (
        Line("rmlui", "an empty document renders"),
        Line("rmlui", "an empty document's frame does not equal the golden image"),
        Line("rmlui", "the knob document's frame equals the golden image again"),
    )),
    Item("3c", "With RmlUi: the Vellum renderer within a geometry/texture tolerance", (
        TestPassed("pulp-editor-host-slice-rmlui-renderer"),
        Line("rmlui-renderer", "the soft renderer's frame equals the module's golden image"),
        Line("rmlui-renderer", "the Vellum renderer renders the knob document"),
        Line("rmlui-renderer",
             "the Vellum renderer's frame is within the tolerance of the soft renderer's"),
        Line("rmlui-renderer", "an empty document falls outside that tolerance"),
        Line("rmlui-renderer", "the knob at gain 0.2 falls outside that tolerance"),
    ), absent_marker="pulp-editor-host-slice-rmlui-renderer-absent",
         absent_reason="Vellum SDK built without VELLUM_RMLUI_VELLUM_RENDERER"),
    Item("3d", "With RmlUi: a knob drag is begin -> set(0.75) -> end", (
        Line("rmlui", "the drag lands on exactly 0.75"),
        Line("rmlui", "the host sees set(0.75) immediately before end"),
    )),
    Item("3e", "With RmlUi: a host-set 0.2 turns the LED off after one frame", (
        Line("rmlui", "the LED turns off after one frame at 0.2"),
    )),
    Item("3f", "With RmlUi: two editor instances open together", (
        Line("rmlui", "two editor instances are open at once"),
    )),
    Item("3g", "With RmlUi: removing the gesture calls makes the drag check fail", (
        Line("rmlui", "the drag opens one host gesture first"),
        Mutation("rmlui-gesture"),
    )),
    Item("3h", "With RmlUi: VST3, CLAP and AU v2 editors in process, and the standalone",
         _format_checks("rmlui") + _standalone_checks("rmlui")),
    Item("4", "Bring your own UI: a plain AppKit editor over the contract alone", (
        TestPassed("pulp-editor-host-slice-native"),
        Line("native", "the drag lands on exactly 0.75"),
        Line("native", "an inspector opens beside the editor"),
    ) + _format_checks("native") + _standalone_checks("native")),
    Item("5", "Dependency direction: core/ includes and links nothing from Vellum", (
        Direction(),
    )),
    Item("6", "One GPU device: single-Dawn census per artifact; two Vellum versions in one REAPER",
         skip_reason="needs the gpu-runtime subproject and the Pulp SDK bundling the Vellum SDK "
                     "(M2 gpu-runtime, M3)"),
    Item("7", "Sterile default checkout: clone, pulp build, pulp create, items 1-2 without Vellum",
         skip_reason="needs the Pulp SDK to bundle the Vellum SDK and pulp create --ui (M3)"),
)


# ── Reading what the harnesses printed ──────────────────────────────────────

# "[vellum/vst3] ok: attach to a real NSView", optionally behind ctest -V's
# "12: " prefix.
_LINE = re.compile(r"^(?:\d+: )?\[(?P<tag>[a-z0-9_/-]+)\] (?P<state>ok|FAIL): (?P<text>.*?)\s*$")
_RESULT = re.compile(
    r"Test\s+#\d+: (?P<name>\S+) \.*\s*(?:\*+)?(?P<state>Passed|Failed|Not Run|Timeout|"
    r"Exception|Skipped)")


@dataclasses.dataclass
class Observations:
    ok: set = dataclasses.field(default_factory=set)
    failed: set = dataclasses.field(default_factory=set)
    tests: dict = dataclasses.field(default_factory=dict)
    direction_passed: bool | None = None
    # confirm_failure.sh verdicts by mutation name ("CONFIRMED", "NOT
    # CONFIRMED", "INCONCLUSIVE"), and why mutations did not run at all.
    mutations: dict = dataclasses.field(default_factory=dict)
    mutation_skip_reason: str = ""

    def ok_count(self, tag: str) -> int:
        return sum(1 for t, _ in self.ok if t == tag)

    def failed_in(self, tag: str) -> list:
        return sorted(text for t, text in self.failed if t == tag)


def parse_ctest_output(text: str, observations: Observations | None = None) -> Observations:
    obs = observations or Observations()
    for raw in text.splitlines():
        line = raw.rstrip()
        m = _LINE.match(line)
        if m:
            key = (m["tag"], m["text"])
            (obs.ok if m["state"] == "ok" else obs.failed).add(key)
            continue
        r = _RESULT.search(line)
        if r:
            obs.tests[r["name"]] = r["state"]
    return obs


def evaluate_check(check, obs: Observations) -> tuple[bool, str]:
    if isinstance(check, TestPassed):
        state = obs.tests.get(check.name)
        if state is None:
            return False, f"{check.describe()} did not run"
        return state == "Passed", f"{check.describe()}: {state}"
    if isinstance(check, Line):
        if (check.tag, check.text) in obs.failed:
            return False, f"{check.describe()}: FAIL"
        if (check.tag, check.text) not in obs.ok:
            return False, f"{check.describe()}: not reported"
        return True, f"{check.describe()}: ok"
    if isinstance(check, TagClean):
        failed = obs.failed_in(check.tag)
        if failed:
            return False, f"[{check.tag}] failed: " + "; ".join(failed)
        count = obs.ok_count(check.tag)
        if count < check.minimum:
            return False, f"[{check.tag}] reported {count} checks, expected at least {check.minimum}"
        return True, f"[{check.tag}] {count} checks ok"
    if isinstance(check, Mutation):
        verdict = obs.mutations.get(check.name)
        if verdict is None:
            return False, f"{check.describe()} did not run"
        return verdict == "CONFIRMED", f"{check.describe()}: {verdict}"
    if isinstance(check, Direction):
        if obs.direction_passed is None:
            return False, "core_vellum_direction.py did not run"
        return obs.direction_passed, f"core_vellum_direction.py: {'ok' if obs.direction_passed else 'FAIL'}"
    raise TypeError(f"unknown check {check!r}")


@dataclasses.dataclass
class Verdict:
    key: str
    title: str
    status: str
    details: list


def evaluate(items: Iterable[Item], obs: Observations) -> list:
    verdicts = []
    for item in items:
        if item.skip_reason:
            verdicts.append(Verdict(item.key, item.title, SKIP, [item.skip_reason]))
            continue
        own_tests = [c.name for c in item.checks if isinstance(c, TestPassed)]
        if (item.absent_marker and obs.tests.get(item.absent_marker) == "Passed" and
                not any(name in obs.tests for name in own_tests)):
            verdicts.append(Verdict(item.key, item.title, SKIP, [item.absent_reason]))
            continue
        mutations = [c for c in item.checks if isinstance(c, Mutation)]
        if obs.mutation_skip_reason and any(c.name not in obs.mutations for c in mutations):
            verdicts.append(Verdict(item.key, item.title, SKIP, [obs.mutation_skip_reason]))
            continue
        if not item.checks:
            # An item with nothing to check proves nothing.
            verdicts.append(Verdict(item.key, item.title, FAIL, ["no checks defined"]))
            continue
        results = [evaluate_check(c, obs) for c in item.checks]
        failed = [msg for ok, msg in results if not ok]
        verdicts.append(Verdict(item.key, item.title, FAIL if failed else PASS,
                                failed or [f"{len(results)} checks passed"]))
    return verdicts


def render_table(verdicts: list) -> str:
    width = max(len(v.title) for v in verdicts)
    width = min(width, 96)
    lines = [f"{'Item':<5} {'Verdict':<7} Acceptance check", f"{'-' * 5} {'-' * 7} {'-' * width}"]
    for v in verdicts:
        title = v.title if len(v.title) <= width else v.title[: width - 1] + "…"
        lines.append(f"{v.key:<5} {v.status:<7} {title}")
        if v.status != PASS:
            for detail in v.details:
                lines.append(f"{'':<14}- {detail}")
    counts = {s: sum(1 for v in verdicts if v.status == s) for s in (PASS, FAIL, SKIP)}
    summary = f"{counts[PASS]} PASS, {counts[FAIL]} FAIL, {counts[SKIP]} SKIP"
    if counts[FAIL]:
        summary += " — acceptance FAILED"
    elif counts[SKIP]:
        summary += " — no failures, but SKIPPED items are not passes; acceptance is incomplete"
    else:
        summary += " — acceptance passed"
    lines += ["", summary]
    return "\n".join(lines)


# ── Running it ──────────────────────────────────────────────────────────────


def _run(cmd: list, **kwargs) -> subprocess.CompletedProcess:
    print("$ " + " ".join(str(c) for c in cmd), file=sys.stderr, flush=True)
    return subprocess.run(cmd, text=True, encoding="utf-8", errors="replace", **kwargs)


# Item 3g: the RmlUi editor without its gesture calls must fail the harness.
GESTURE_MUTATION = {
    "name": "rmlui-gesture",
    "file": "test/editor_host_slice/rmlui_editor.cpp",
    "break": r"s/\n\s*params\.begin\(kGainParam\);//; s/\n\s*params\.end\(kGainParam\);//",
    "target": "pulp-slice-rmlui",
}

_CONFIRM_VERDICT = re.compile(r"confirm-failure: (CONFIRMED|NOT CONFIRMED|INCONCLUSIVE)")


def parse_confirm_failure(text: str) -> str:
    """The verdict confirm_failure.sh printed, or INCONCLUSIVE when none."""
    found = _CONFIRM_VERDICT.findall(text)
    return found[-1] if found else "INCONCLUSIVE"


def find_binary(build: Path, name: str) -> Path | None:
    for candidate in sorted(build.rglob(name)):
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate
    return None


def run_mutation(build: Path, mutation: dict) -> str:
    binary = find_binary(build, mutation["target"])
    if binary is None:
        return "INCONCLUSIVE"
    out_dir = build / "editor-host-acceptance-mutation"
    out_dir.mkdir(parents=True, exist_ok=True)
    result = _run([str(REPO_ROOT / "tools/scripts/confirm_failure.sh"),
                   "--file", mutation["file"],
                   "--break", f"perl -0pi -e '{mutation['break']}'",
                   "--build-dir", str(build), "--target", mutation["target"],
                   "--test", f"{binary} {out_dir}"],
                  capture_output=True, cwd=str(REPO_ROOT))
    return parse_confirm_failure(result.stdout + "\n" + result.stderr)


def check_vellum_prefix(prefix: Path) -> str:
    for needed in ("lib/cmake/Vellum/VellumConfig.cmake", "lib/cmake/VellumRmlUi"):
        if not (prefix / needed).exists():
            return f"{prefix} is not an installed Vellum SDK with the mirror and RmlUi ({needed} missing)"
    return ""


def main(argv: list | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--vellum-prefix", required=True, type=Path,
                        help="installed Vellum SDK built with the mirror and the RmlUi module")
    parser.add_argument("--build-dir", type=Path, default=REPO_ROOT / "build-acceptance")
    parser.add_argument("--skip-configure", action="store_true")
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--json", type=Path, help="also write the verdicts as JSON here")
    args = parser.parse_args(argv)

    if sys.platform != "darwin":
        print("editor-host acceptance: macOS only", file=sys.stderr)
        return 2
    prefix = args.vellum_prefix.expanduser().resolve()
    problem = check_vellum_prefix(prefix)
    if problem:
        print(f"editor-host acceptance: {problem}", file=sys.stderr)
        return 2
    build = args.build_dir.expanduser().resolve()

    if not args.skip_configure:
        configure = _run(["cmake", "-S", str(REPO_ROOT), "-B", str(build), "-G", "Ninja",
                          "-DCMAKE_BUILD_TYPE=Release", "-DPULP_BUILD_EXAMPLES=OFF",
                          f"-DPULP_EDITOR_HOST_SLICE_VELLUM_PREFIX={prefix}"])
        if configure.returncode != 0:
            print("editor-host acceptance: configure failed", file=sys.stderr)
            return 2
    if not args.skip_build:
        built = _run([str(REPO_ROOT / "tools/ci/governed-build.sh"), "cmake", "--build",
                      str(build), "--target", SLICE_TARGET])
        if built.returncode != 0:
            print("editor-host acceptance: build failed", file=sys.stderr)
            return 2

    ctest = _run(["ctest", "--test-dir", str(build), "-L", SLICE_LABEL, "-V"],
                 capture_output=True)
    obs = parse_ctest_output(ctest.stdout + "\n" + ctest.stderr)
    if not obs.tests:
        print("editor-host acceptance: ctest ran no editor-host-slice test; is the build "
              "configured with PULP_EDITOR_HOST_SLICE_VELLUM_PREFIX?", file=sys.stderr)
        return 2

    direction = _run([sys.executable, str(REPO_ROOT / "tools/scripts/core_vellum_direction.py"),
                      "--root", str(REPO_ROOT)], capture_output=True)
    obs.direction_passed = direction.returncode == 0

    if args.skip_build:
        obs.mutation_skip_reason = ("not run: --skip-build (the mutation check rebuilds the "
                                    "RmlUi slice)")
    else:
        obs.mutations[GESTURE_MUTATION["name"]] = run_mutation(build, GESTURE_MUTATION)

    verdicts = evaluate(ITEMS, obs)
    print(render_table(verdicts))
    if args.json:
        args.json.write_text(json.dumps([dataclasses.asdict(v) for v in verdicts], indent=2) + "\n",
                             encoding="utf-8")
    return 1 if any(v.status == FAIL for v in verdicts) else 0


if __name__ == "__main__":
    sys.exit(main())
