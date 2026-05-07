#!/usr/bin/env python3
"""Tests for compat_sync_check.py (#1029).

Two layers:
    1. Unit-style — pure-Python evaluation of compute_findings,
       parse_compat_update_trailer, _compat_json_satisfied, etc.
    2. Integration — throwaway git repo + real git commits + invoke
       the script via subprocess. Mirrors test_gates.py's pattern.

Run:
    python3 tools/scripts/test_compat_sync_check.py
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent.parent.parent
SCRIPT = REPO_ROOT / "tools" / "scripts" / "compat_sync_check.py"

# Make the script importable for unit-style tests.
sys.path.insert(0, str(REPO_ROOT / "tools" / "scripts"))
import compat_sync_check as csc  # noqa: E402


# ── Unit tests (pure-Python) ───────────────────────────────────────────


class TrailerParseTests(unittest.TestCase):

    def test_parses_skip_with_reason(self) -> None:
        trailers = {"compat-update": [
            'skip prefix=css reason="docs-only PR"',
        ]}
        self.assertEqual(
            csc.parse_compat_update_trailer(trailers),
            {"css": "docs-only PR"},
        )

    def test_parses_multiple_prefixes(self) -> None:
        trailers = {"compat-update": [
            'skip prefix=css reason="a"',
            'skip prefix=html reason="b"',
        ]}
        self.assertEqual(
            csc.parse_compat_update_trailer(trailers),
            {"css": "a", "html": "b"},
        )

    def test_wildcard_prefix(self) -> None:
        trailers = {"compat-update": [
            'skip prefix=* reason="multi-section change, no matrix entry yet"',
        ]}
        self.assertEqual(
            csc.parse_compat_update_trailer(trailers),
            {"*": "multi-section change, no matrix entry yet"},
        )

    def test_bare_skip_rejected(self) -> None:
        trailers = {"compat-update": ["skip"]}
        self.assertEqual(csc.parse_compat_update_trailer(trailers), {})

    def test_skip_without_reason_rejected(self) -> None:
        trailers = {"compat-update": ["skip prefix=css"]}
        self.assertEqual(csc.parse_compat_update_trailer(trailers), {})

    def test_skip_with_empty_reason_rejected(self) -> None:
        trailers = {"compat-update": ['skip prefix=css reason=""']}
        self.assertEqual(csc.parse_compat_update_trailer(trailers), {})


class CompatJsonSatisfiedTests(unittest.TestCase):

    def test_modified_in_diff_satisfies(self) -> None:
        ok, _ = csc._compat_json_satisfied(
            "css", {}, ["compat.json"], "compat.json",
        )
        self.assertTrue(ok)

    def test_empty_section_tolerated(self) -> None:
        ok, detail = csc._compat_json_satisfied(
            "css", {"css": {}}, [], "compat.json",
        )
        self.assertTrue(ok)
        self.assertIn("empty", detail.lower())

    def test_populated_section_satisfies(self) -> None:
        ok, _ = csc._compat_json_satisfied(
            "css", {"css": {"color": "supported"}}, [], "compat.json",
        )
        self.assertTrue(ok)

    def test_missing_section_fails(self) -> None:
        ok, detail = csc._compat_json_satisfied(
            "css", {}, [], "compat.json",
        )
        self.assertFalse(ok)
        self.assertIn("css", detail)


class ComputeFindingsTests(unittest.TestCase):

    def _stub_compat_map(self) -> csc.CompatMap:
        return csc.CompatMap(paths={
            "core/view/js/web-compat-canvas.js": [
                csc.Requirement(kind="compat-json", prefix="canvas2d",
                                path=None, glob=None),
                csc.Requirement(kind="doc", prefix=None,
                                path="docs/reference/compat/canvas2d.md",
                                glob=None),
                csc.Requirement(kind="test", prefix=None, path=None,
                                glob="test/test_canvas_widget*.cpp"),
            ],
        })

    def test_unrelated_diff_no_findings(self) -> None:
        findings = csc.compute_findings(
            changed=["README.md"],
            compat_map=self._stub_compat_map(),
            compat_data={"canvas2d": {}},
            bypasses={},
        )
        self.assertEqual(findings, [])

    def test_touched_source_with_empty_section_satisfies_compat_json(self) -> None:
        findings = csc.compute_findings(
            changed=["core/view/js/web-compat-canvas.js"],
            compat_map=self._stub_compat_map(),
            compat_data={"canvas2d": {}},
            bypasses={},
        )
        compat_findings = [f for f in findings
                           if f.requirement.kind == "compat-json"]
        self.assertEqual(len(compat_findings), 1)
        self.assertTrue(compat_findings[0].satisfied)

    def test_missing_doc_and_test_fails(self) -> None:
        findings = csc.compute_findings(
            changed=["core/view/js/web-compat-canvas.js"],
            compat_map=self._stub_compat_map(),
            compat_data={"canvas2d": {}},
            bypasses={},
        )
        unsatisfied = [f for f in findings if not f.satisfied
                       and f.bypass_reason is None]
        kinds = {f.requirement.kind for f in unsatisfied}
        self.assertEqual(kinds, {"doc", "test"})

    def test_doc_and_test_present_satisfies(self) -> None:
        findings = csc.compute_findings(
            changed=[
                "core/view/js/web-compat-canvas.js",
                "docs/reference/compat/canvas2d.md",
                "test/test_canvas_widget.cpp",
            ],
            compat_map=self._stub_compat_map(),
            compat_data={"canvas2d": {}},
            bypasses={},
        )
        self.assertTrue(all(f.satisfied for f in findings))

    def test_bypass_trailer_marks_finding_bypassed(self) -> None:
        findings = csc.compute_findings(
            changed=["core/view/js/web-compat-canvas.js"],
            compat_map=self._stub_compat_map(),
            compat_data={"canvas2d": {}},
            bypasses={"canvas2d": "test-only PR"},
        )
        unsatisfied_with_bypass = [
            f for f in findings
            if not f.satisfied and f.bypass_reason
        ]
        self.assertTrue(len(unsatisfied_with_bypass) >= 1)
        for f in unsatisfied_with_bypass:
            self.assertEqual(f.bypass_reason, "test-only PR")

    def test_wildcard_bypass_covers_all_prefixes(self) -> None:
        findings = csc.compute_findings(
            changed=["core/view/src/widget_bridge.cpp"],
            compat_map=csc.CompatMap(paths={
                "core/view/src/widget_bridge.cpp": [
                    csc.Requirement(kind="compat-json", prefix="*",
                                    path=None, glob=None),
                    csc.Requirement(
                        kind="doc", prefix=None,
                        path="docs/reference/compat/{prefix}.md",
                        glob=None,
                    ),
                ],
            }),
            compat_data={
                "css": {}, "html": {}, "canvas2d": {},
            },
            bypasses={"*": "infrastructure-only PR"},
        )
        # Every unsatisfied finding should be marked bypassed.
        for f in findings:
            if not f.satisfied:
                self.assertEqual(f.bypass_reason, "infrastructure-only PR")

    def test_doc_template_expansion_per_prefix(self) -> None:
        compat_map = csc.CompatMap(paths={
            "core/view/src/widget_bridge.cpp": [
                csc.Requirement(kind="compat-json", prefix="*",
                                path=None, glob=None),
                csc.Requirement(
                    kind="doc", prefix=None,
                    path="docs/reference/compat/{prefix}.md",
                    glob=None,
                ),
            ],
        })
        findings = csc.compute_findings(
            changed=[
                "core/view/src/widget_bridge.cpp",
                "docs/reference/compat/css.md",
            ],
            compat_map=compat_map,
            compat_data={"css": {}, "html": {}},
            bypasses={},
        )
        # css doc must be satisfied; html doc must not be.
        css_doc = [f for f in findings
                   if f.requirement.kind == "doc"
                   and f.resolved_prefix == "css"]
        html_doc = [f for f in findings
                    if f.requirement.kind == "doc"
                    and f.resolved_prefix == "html"]
        self.assertTrue(css_doc and css_doc[0].satisfied)
        self.assertTrue(html_doc and not html_doc[0].satisfied)


class SelfCheckTests(unittest.TestCase):

    def test_unknown_prefix_flagged(self) -> None:
        compat_map = csc.CompatMap(paths={
            "core/view/src/x.cpp": [
                csc.Requirement(kind="compat-json", prefix="bogus",
                                path=None, glob=None),
            ],
        })
        errors = csc.self_check(compat_map, {})
        self.assertTrue(any("bogus" in e for e in errors), msg=errors)

    def test_missing_doc_path_flagged(self) -> None:
        compat_map = csc.CompatMap(paths={
            "x": [csc.Requirement(kind="doc", prefix=None,
                                  path="", glob=None)],
        })
        errors = csc.self_check(compat_map, {})
        self.assertTrue(any("no path" in e for e in errors), msg=errors)

    def test_missing_test_glob_flagged(self) -> None:
        compat_map = csc.CompatMap(paths={
            "x": [csc.Requirement(kind="test", prefix=None,
                                  path=None, glob="")],
        })
        errors = csc.self_check(compat_map, {})
        self.assertTrue(any("no glob" in e for e in errors), msg=errors)

    def test_unknown_compat_json_section_flagged(self) -> None:
        errors = csc.self_check(
            csc.CompatMap(paths={}),
            {"css": {}, "made-up": {}},
        )
        self.assertTrue(any("made-up" in e for e in errors), msg=errors)


# ── Integration tests (real git repo) ──────────────────────────────────


def _git(cwd: Path, *args: str) -> None:
    env = os.environ.copy()
    env["GIT_AUTHOR_NAME"] = env["GIT_COMMITTER_NAME"] = "Test"
    env["GIT_AUTHOR_EMAIL"] = env["GIT_COMMITTER_EMAIL"] = "test@example.com"
    subprocess.run(
        ["git", "-C", str(cwd), *args],
        check=True, capture_output=True, env=env,
    )


def _run_script(cwd: Path, *args: str) -> tuple[int, str]:
    # Drop PULP_ENFORCE_PREPUSH from the inherited env so advisory-mode
    # tests (no --enforce flag) get a genuine advisory exit code.
    # CI sets PULP_ENFORCE_PREPUSH=1 globally to harden the gate
    # against drift; without scrubbing here, every advisory-mode test
    # silently runs in enforce mode and false-fails on missing artifacts.
    env = {k: v for k, v in os.environ.items() if k != "PULP_ENFORCE_PREPUSH"}
    result = subprocess.run(
        ["python3", str(SCRIPT), *args],
        cwd=cwd, capture_output=True, text=True, env=env,
    )
    return result.returncode, result.stdout + result.stderr


class IntegrationFixture:

    def __init__(self, root: Path) -> None:
        self.root = root

    def init(self) -> None:
        r = self.root
        _git(r, "init", "-q", "-b", "main")
        _git(r, "config", "user.email", "test@example.com")
        _git(r, "config", "user.name", "Test")
        _git(r, "config", "commit.gpgsign", "false")

        # Minimal layout the gate cares about.
        (r / "tools" / "scripts").mkdir(parents=True)
        (r / "core" / "view" / "src").mkdir(parents=True)
        (r / "core" / "view" / "js").mkdir(parents=True)
        (r / "test").mkdir()

        # Stub bridge file so we can later modify it to trigger the gate.
        (r / "core" / "view" / "src" / "widget_bridge.cpp").write_text(
            "// stub\n"
        )
        (r / "core" / "view" / "js" / "web-compat-canvas.js").write_text(
            "// stub\n"
        )
        (r / "core" / "view" / "js" / "web-compat-element.js").write_text(
            "// stub\n"
        )
        (r / "core" / "view" / "js" / "web-compat-style-decl.js").write_text(
            "// stub\n"
        )
        (r / "test" / "test_widget_bridge.cpp").write_text(
            "int main() { return 0; }\n"
        )
        (r / "test" / "test_canvas_widget.cpp").write_text(
            "int main() { return 0; }\n"
        )

        # compat_path_map.json — small but realistic.
        (r / "tools" / "scripts" / "compat_path_map.json").write_text(
            json.dumps({
                "compat-schema-version": "0.1",
                "schema_version": 1,
                "paths": {
                    "core/view/src/widget_bridge.cpp": [
                        {"kind": "compat-json", "prefix": "*"},
                        {"kind": "doc",
                         "path": "docs/reference/compat/{prefix}.md"},
                        {"kind": "test",
                         "glob": "test/test_widget_bridge*.cpp"},
                    ],
                    "core/view/js/web-compat-canvas.js": [
                        {"kind": "compat-json", "prefix": "canvas2d"},
                        {"kind": "doc",
                         "path": "docs/reference/compat/canvas2d.md"},
                        {"kind": "test",
                         "glob": "test/test_canvas_widget*.cpp"},
                    ],
                },
            }, indent=2) + "\n"
        )

        # Stub compat.json with all known prefixes empty — the
        # partial-blocker tolerance should cover the compat-json
        # requirements.
        (r / "compat.json").write_text(
            json.dumps({
                "compat-schema-version": "0.1",
                "css": {}, "rn": {}, "yoga": {}, "react": {},
                "html": {}, "canvas2d": {}, "imports": {},
            }, indent=2) + "\n"
        )

        _git(r, "add", "-A")
        _git(r, "commit", "-q", "-m", "initial")
        _git(r, "update-ref", "refs/remotes/origin/main", "HEAD")

    def write(self, rel: str, content: str) -> None:
        p = self.root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(content)

    def commit(self, message: str) -> None:
        _git(self.root, "add", "-A")
        _git(self.root, "commit", "-q", "-m", message)


class IntegrationTests(unittest.TestCase):

    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="pulp-compat-"))
        self.f = IntegrationFixture(self.tmp)
        self.f.init()

    def tearDown(self) -> None:
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _common_args(self) -> list[str]:
        return [
            "--base", "origin/main",
            "--config", str(self.tmp / "tools/scripts/compat_path_map.json"),
            "--compat-json", str(self.tmp / "compat.json"),
        ]

    def test_no_drift_on_unrelated_diff(self) -> None:
        self.f.write("README.md", "hello\n")
        self.f.commit("docs: add readme")
        code, out = _run_script(
            self.tmp, "--mode=report", "--enforce", *self._common_args(),
        )
        self.assertEqual(code, 0, msg=out)
        self.assertIn("nothing to verify", out)

    def test_drift_when_canvas_touched_without_doc_or_test(self) -> None:
        self.f.write("core/view/js/web-compat-canvas.js",
                     "// new content\n")
        self.f.commit("feat(view): tweak canvas shim")
        code, out = _run_script(
            self.tmp, "--mode=report", "--enforce", *self._common_args(),
        )
        self.assertEqual(code, 1, msg=out)
        self.assertIn("canvas2d", out)
        self.assertIn("FAILED", out)
        # Empty compat.json section should be tolerated — the failing
        # requirements are doc + test, NOT compat-json.
        self.assertIn("doc", out)
        self.assertIn("test", out)

    def test_satisfied_when_doc_and_test_updated(self) -> None:
        self.f.write("core/view/js/web-compat-canvas.js",
                     "// new content\n")
        self.f.write("docs/reference/compat/canvas2d.md",
                     "# canvas2d compat\n")
        self.f.write("test/test_canvas_widget.cpp",
                     "int main() { return 1; }\n")
        self.f.commit("feat(view): canvas shim + doc + test")
        code, out = _run_script(
            self.tmp, "--mode=report", "--enforce", *self._common_args(),
        )
        self.assertEqual(code, 0, msg=out)
        self.assertIn("✓", out)

    def test_bypass_trailer_passes(self) -> None:
        self.f.write("core/view/js/web-compat-canvas.js",
                     "// new content\n")
        _git(self.tmp, "add", "-A")
        _git(self.tmp, "commit", "-q", "-m",
             'chore(view): mechanical canvas rename\n\n'
             'Compat-Update: skip prefix=canvas2d reason="mechanical rename"')
        code, out = _run_script(
            self.tmp, "--mode=report", "--enforce", *self._common_args(),
        )
        self.assertEqual(code, 0, msg=out)
        self.assertIn("bypassed", out)

    def test_advisory_mode_does_not_block(self) -> None:
        self.f.write("core/view/js/web-compat-canvas.js",
                     "// new content\n")
        self.f.commit("feat(view): canvas shim only")
        # No --enforce, no PULP_ENFORCE_PREPUSH.
        code, out = _run_script(
            self.tmp, "--mode=report", *self._common_args(),
        )
        self.assertEqual(code, 0, msg=out)
        self.assertIn("advisory", out)

    def test_hint_mode_always_exit_zero(self) -> None:
        self.f.write("core/view/js/web-compat-canvas.js",
                     "// new content\n")
        self.f.commit("feat(view): canvas shim only")
        code, _ = _run_script(
            self.tmp, "--mode=hint", "--enforce", *self._common_args(),
        )
        self.assertEqual(code, 0)

    def test_empty_section_tolerance_smoke(self) -> None:
        """Critical: an empty compat.json section must NOT false-
        positive while #1027 is still open."""
        # Touch widget_bridge.cpp (wildcard prefix → expands to all
        # KNOWN_PREFIXES). The compat-json requirement should pass
        # for every prefix because all sections are empty stubs. The
        # only failures should be doc/test, not compat-json.
        self.f.write("core/view/src/widget_bridge.cpp",
                     "// new content\n")
        self.f.commit("feat(view): bridge tweak")
        code, out = _run_script(
            self.tmp, "--mode=report", "--enforce", *self._common_args(),
        )
        # We expect drift (doc/test missing) — but the compat-json
        # rows must each be ✓, not ✗.
        for line in out.splitlines():
            if "compat-json" in line:
                self.assertIn("✓", line, msg=line)

    def test_apply_mode_adds_missing_section(self) -> None:
        # Bake the missing-canvas2d state into origin/main so the diff
        # range we test against does NOT include the removal commit
        # (otherwise compat.json being modified in the diff would
        # auto-satisfy the compat-json requirement and there'd be
        # nothing for apply to fix).
        compat_path = self.tmp / "compat.json"
        data = json.loads(compat_path.read_text())
        del data["canvas2d"]
        compat_path.write_text(json.dumps(data, indent=2) + "\n")
        self.f.commit("chore: remove canvas2d section")
        _git(self.tmp, "update-ref", "refs/remotes/origin/main", "HEAD")

        # Now touch the shim → compat-json requirement is unsatisfied.
        self.f.write("core/view/js/web-compat-canvas.js",
                     "// new content\n")
        self.f.commit("feat(view): canvas shim")
        code, out = _run_script(
            self.tmp, "--mode=apply", *self._common_args(),
        )
        # apply mode should have re-added the section.
        new_data = json.loads(compat_path.read_text())
        self.assertIn("canvas2d", new_data, msg=out)
        self.assertEqual(new_data["canvas2d"], {})
        self.assertIn("added stub sections", out)

    def test_self_check_unknown_prefix_in_map(self) -> None:
        # Inject a bad entry into the path map.
        cfg = self.tmp / "tools/scripts/compat_path_map.json"
        data = json.loads(cfg.read_text())
        data["paths"]["core/view/src/bogus.cpp"] = [
            {"kind": "compat-json", "prefix": "not-real"},
        ]
        cfg.write_text(json.dumps(data, indent=2) + "\n")
        self.f.commit("chore: add bogus map entry")
        code, out = _run_script(
            self.tmp, "--mode=report", "--enforce", *self._common_args(),
        )
        self.assertEqual(code, 1, msg=out)
        self.assertIn("not-real", out)


if __name__ == "__main__":
    unittest.main(verbosity=2)

    def test_unknown_kind_in_map_raises_at_load(self) -> None:
        # pulp #1171 (Codex P2 on #1068) — a typo'd `kind` like "tests"
        # used to be silently dropped, leaving CI green while a required
        # compat-artifact gate was effectively disabled. The fix raises
        # at config-load time so the misconfiguration fails loudly.
        cfg = self.tmp / "tools/scripts/compat_path_map.json"
        data = json.loads(cfg.read_text())
        data["paths"].setdefault("core/view/src/scroll_view.cpp", []).append(
            {"kind": "tests", "glob": "test/test_scroll*.cpp"},  # <-- typo
        )
        cfg.write_text(json.dumps(data, indent=2) + "\n")
        self.f.commit("chore: typo in compat_path_map kind")
        code, out = _run_script(
            self.tmp, "--mode=report", "--enforce", *self._common_args(),
        )
        # Hard-fail: exit non-zero, name the bad kind, valid kinds in the
        # error message so the author can correct the typo.
        self.assertNotEqual(code, 0, msg=out)
        self.assertIn("tests", out)
        self.assertIn("compat-json", out)  # listed as a valid kind
