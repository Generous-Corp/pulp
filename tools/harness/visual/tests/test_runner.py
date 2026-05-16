"""Tests for visual snapshot fixture discovery and coverage counts."""

from __future__ import annotations

import io
import json
import os
import sys
import tempfile
import unittest
from unittest import mock
from contextlib import redirect_stderr
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[3]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tools.harness.visual import runner  # noqa: E402
from tools.harness.visual import spec  # noqa: E402


class RunnerTests(unittest.TestCase):
    def test_find_repo_root_walks_up_or_returns_start_when_no_markers(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            repo = root / "repo"
            child = repo / "a" / "b"
            child.mkdir(parents=True)
            (repo / "compat.json").write_text("{}", encoding="utf-8")
            (repo / "CMakeLists.txt").write_text("# fixture\n", encoding="utf-8")

            self.assertEqual(runner.find_repo_root(child), repo.resolve())
            self.assertEqual(runner.find_repo_root(root / "outside"), (root / "outside").resolve())

    def test_resolve_fixtures_accepts_surface_prefixed_entries(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            fixture_dir = root / "tools" / "harness" / "visual" / "fixtures" / "yoga"
            fixture_dir.mkdir(parents=True)
            fixture = fixture_dir / "box-sizing.json"
            fixture.write_text("{}", encoding="utf-8")

            self.assertEqual(
                runner.resolve_fixtures(root, "yoga", ["yoga/box-sizing"]),
                [fixture],
            )

    def test_resolve_fixtures_accepts_paths_and_reports_known_names(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            fixture_dir = root / "tools" / "harness" / "visual" / "fixtures" / "yoga"
            fixture_dir.mkdir(parents=True)
            fixture = fixture_dir / "box-sizing.json"
            fixture.write_text("{}", encoding="utf-8")

            self.assertEqual(runner.resolve_fixtures(root, "yoga", [str(fixture)]), [fixture])
            with self.assertRaisesRegex(ValueError, "unknown visual fixture"):
                runner.resolve_fixtures(root, "yoga", ["missing"])

    def test_list_fixtures_missing_surface_is_empty_and_entry_name_uses_stem(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.assertEqual(runner.list_fixtures(root, "missing"), [])
            self.assertEqual(runner.entry_name(Path("nested/example.fixture.json")), "example.fixture")

    def test_fixture_spec_keeps_existing_yoga_defaults(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            fixture_dir = root / "tools" / "harness" / "visual" / "fixtures" / "yoga"
            fixture_dir.mkdir(parents=True)
            fixture = fixture_dir / "box-sizing.json"
            fixture.write_text("{}", encoding="utf-8")

            parsed = spec.fixture_spec_from_file(fixture)

            self.assertEqual(parsed.id, "yoga/box-sizing")
            self.assertEqual(parsed.surface, "yoga")
            self.assertEqual(parsed.driver, "native_tree")
            self.assertEqual(parsed.capture_format, "json")
            self.assertEqual(runner.golden_path_for(root, "yoga", fixture), spec.golden_path(root, parsed))
            self.assertEqual(
                spec.golden_path(root, parsed),
                root
                / "tools"
                / "harness"
                / "visual"
                / "goldens"
                / "yoga"
                / "box-sizing.json",
            )

    def test_fixture_spec_parses_ids_capture_formats_viewport_and_validation_refs(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            fixture = self._write_fixture(
                root,
                "canvas2d",
                "paint",
                {
                    "id": "paint",
                    "surface": "canvas2d",
                    "kind": "render",
                    "capture": {"format": "PNG"},
                    "viewport": {"width": 320, "height": 240},
                    "tolerance": {"semantic": 0.2},
                },
            )

            parsed = spec.fixture_spec_from_file(fixture)

        self.assertEqual(parsed.id, "canvas2d/paint")
        self.assertEqual(parsed.driver, "render")
        self.assertEqual(parsed.capture_format, "png")
        self.assertEqual(parsed.golden_suffix, ".png")
        self.assertEqual(parsed.viewport, {"width": 320, "height": 240})
        self.assertEqual(parsed.tolerance, {"semantic": 0.2})
        self.assertTrue(spec.parse_validation_ref("visual:canvas2d/paint").is_runtime_fixture)
        self.assertTrue(
            spec.parse_validation_ref("cannot-validate:platform-bound").excludes_from_visual_denominator
        )
        self.assertIsNone(spec.parse_validation_ref("invalid:target"))
        self.assertIsNone(spec.parse_validation_ref("visual:"))
        self.assertEqual(
            [ref.raw for ref in spec.runtime_fixture_refs(["visual:canvas2d/paint", "unit:test_paint"])],
            ["visual:canvas2d/paint"],
        )

    def test_fixture_specs_handles_missing_roots_and_non_object_json(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.assertEqual(spec.fixture_specs(root), [])
            fixture_dir = root / "tools" / "harness" / "visual" / "fixtures" / "yoga"
            fixture_dir.mkdir(parents=True)
            bad = fixture_dir / "bad.json"
            bad.write_text("[]", encoding="utf-8")

            with self.assertRaisesRegex(ValueError, "must contain a JSON object"):
                spec.fixture_spec_from_file(bad)

    def test_visual_counts_use_typed_runtime_refs_and_supported_denominator(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self._write_fixture_with_golden(root, "yoga", "a")
            (root / "compat.json").write_text(
                json.dumps(
                    {
                        "yoga": {
                            "yoga/a": {"status": "supported", "tests": ["semantic:yoga/a"]},
                            "yoga/b": {"status": "supported", "tests": ["unit:test_b"]},
                            "yoga/c": {"status": "missing", "tests": ["semantic:yoga/c"]},
                            "yoga/__note": {
                                "status": "supported",
                                "tests": ["semantic:yoga/note"],
                            },
                            "yoga/d": {"status": "supported", "tests": ["cannot-validate:issue-1"]},
                            "yoga/stale": {
                                "status": "supported",
                                "tests": ["semantic:yoga/stale"],
                            },
                        }
                    }
                ),
                encoding="utf-8",
            )

            visual_counts = runner.visual_pass_counts(root, ["yoga"])
            validation_counts = runner.validation_route_counts(root, ["yoga"])

            self.assertEqual(visual_counts["yoga"]["pass"], 1)
            self.assertEqual(visual_counts["yoga"]["total"], 3)
            self.assertEqual(visual_counts["yoga"]["label"], "1/3")
            self.assertEqual(
                visual_counts["yoga"]["missing_refs"],
                ["yoga/stale: semantic:yoga/stale"],
            )
            self.assertEqual(validation_counts["yoga"]["pass"], 3)
            self.assertEqual(validation_counts["yoga"]["total"], 4)
            self.assertEqual(validation_counts["yoga"]["excluded"], 1)
            self.assertEqual(validation_counts["yoga"]["unvalidated_entries"], ["yoga/stale"])

    def test_counts_handle_missing_or_non_mapping_catalogs(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)

            self.assertEqual(runner.visual_pass_counts(root, ["missing"])["missing"]["label"], "0/0")
            self.assertEqual(
                runner.validation_route_counts(root, ["missing"])["missing"]["unvalidated_entries"],
                [],
            )

            (root / "compat.json").write_text(json.dumps({"yoga": []}), encoding="utf-8")
            self.assertEqual(runner.visual_pass_counts(root, ["yoga"])["yoga"]["total"], 0)
            self.assertEqual(runner.validation_route_counts(root, ["yoga"])["yoga"]["total"], 0)

    def test_duplicate_fixture_ids_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            fixture_dir = root / "tools" / "harness" / "visual" / "fixtures" / "yoga"
            fixture_dir.mkdir(parents=True)
            (fixture_dir / "a.json").write_text(
                json.dumps({"id": "yoga/shared"}),
                encoding="utf-8",
            )
            (fixture_dir / "b.json").write_text(
                json.dumps({"id": "yoga/shared"}),
                encoding="utf-8",
            )

            with self.assertRaisesRegex(ValueError, "duplicate visual fixture id"):
                spec.fixture_ids_with_goldens(root, ["yoga"])

    def test_orphaned_golden_paths_are_reported(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self._write_fixture_with_golden(root, "yoga", "a")
            golden_dir = root / "tools" / "harness" / "visual" / "goldens" / "yoga"
            orphan = golden_dir / "orphan.json"
            orphan.write_text("{}", encoding="utf-8")

            self.assertEqual(spec.orphaned_golden_paths(root, ["yoga"]), [orphan])

    def test_fixture_ids_with_goldens_uses_suffix_and_ignores_missing_goldens(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self._write_fixture(
                root,
                "canvas2d",
                "paint",
                {"id": "canvas2d/paint", "kind": "render", "capture_format": "png"},
            )
            self._write_fixture(root, "canvas2d", "missing", {"id": "canvas2d/missing"})
            golden = root / "tools" / "harness" / "visual" / "goldens" / "canvas2d" / "paint.png"
            golden.parent.mkdir(parents=True)
            golden.write_bytes(b"png")

            self.assertEqual(spec.fixture_ids_with_goldens(root, ["canvas2d"]), {"canvas2d/paint"})
            self.assertEqual(spec.orphaned_golden_paths(root, ["missing"]), [])

    def test_locate_binary_uses_override_build_candidates_and_path(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            override = root / "override-bin"
            self.assertEqual(runner.locate_binary(root, None, override), override)

            build_binary = root / "build" / "test" / f"pulp-test-visual{'.exe' if os.name == 'nt' else ''}"
            build_binary.parent.mkdir(parents=True)
            build_binary.write_text("#!/bin/sh\n", encoding="utf-8")
            self.assertEqual(runner.locate_binary(root, root / "build", None), build_binary)
            build_binary.unlink()

            with mock.patch.object(runner.shutil, "which", return_value=str(root / "path-bin")):
                self.assertEqual(runner.locate_binary(root, None, None), root / "path-bin")

            with mock.patch.object(runner.shutil, "which", return_value=None):
                with self.assertRaisesRegex(FileNotFoundError, "pulp-test-visual binary not found"):
                    runner.locate_binary(root, root / "missing-build", None)

    def test_run_capture_and_snapshot_report_child_failures(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            fixture = self._write_fixture(root, "yoga", "a", {"actual": {"ok": True}})
            binary = root / "fake-failing-visual"
            binary.write_text(
                "#!/usr/bin/env python3\n"
                "import sys\n"
                "sys.stderr.write('capture failed')\n"
                "sys.exit(7)\n",
                encoding="utf-8",
            )
            binary.chmod(0o755)

            with self.assertRaisesRegex(RuntimeError, "capture failed"):
                runner.run_capture(binary, fixture)

            ok_binary = self._write_fake_visual_binary(root)
            self.assertEqual(runner.run_snapshot(ok_binary, fixture), {"ok": True})

    def test_verify_json_writes_actual_on_semantic_diff(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            binary = self._write_fake_visual_binary(root)
            fixture = self._write_fixture(
                root,
                "yoga",
                "diff",
                {"id": "yoga/diff", "actual": {"value": 2}},
            )
            golden = root / "tools" / "harness" / "visual" / "goldens" / "yoga" / "diff.json"
            golden.parent.mkdir(parents=True, exist_ok=True)
            golden.write_text(json.dumps({"value": 1}), encoding="utf-8")

            rc = runner.verify(
                binary,
                root,
                "yoga",
                [fixture],
                actuals_dir=Path("build/visual-actuals"),
            )

            self.assertEqual(rc, 1)
            actual = root / "build" / "visual-actuals" / "yoga" / "diff.json"
            self.assertEqual(json.loads(actual.read_text(encoding="utf-8")), {"value": 2})

    def test_verify_reports_missing_json_golden_and_accepts_matching_json(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            binary = self._write_fake_visual_binary(root)
            fixture = self._write_fixture(root, "yoga", "match", {"actual": {"value": 1}})
            golden = root / "tools" / "harness" / "visual" / "goldens" / "yoga" / "match.json"
            golden.parent.mkdir(parents=True)
            golden.write_text(json.dumps({"value": 1}), encoding="utf-8")

            self.assertEqual(runner.verify(binary, root, "yoga", [fixture]), 0)

            missing = self._write_fixture(root, "yoga", "missing", {"actual": {"value": 1}})
            self.assertEqual(
                runner.verify(binary, root, "yoga", [missing], actuals_dir=Path("actuals")),
                1,
            )
            self.assertEqual(json.loads((root / "actuals" / "yoga" / "missing.json").read_text()), {"value": 1})

    def test_generate_png_writes_png_golden_bytes(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            binary = self._write_fake_visual_binary(root)
            fixture = self._write_fixture(
                root,
                "canvas2d",
                "paint",
                {
                    "id": "canvas2d/paint",
                    "kind": "render",
                    "capture_format": "png",
                    "actual_hex": "89504e470d0a1a0a",
                },
            )

            rc = runner.generate(binary, root, "canvas2d", [fixture])

            self.assertEqual(rc, 0)
            golden = root / "tools" / "harness" / "visual" / "goldens" / "canvas2d" / "paint.png"
            self.assertEqual(golden.read_bytes(), bytes.fromhex("89504e470d0a1a0a"))

    def test_verify_png_uses_exact_bytes_and_writes_actual_on_mismatch(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            binary = self._write_fake_visual_binary(root)
            fixture = self._write_fixture(
                root,
                "canvas2d",
                "paint",
                {
                    "id": "canvas2d/paint",
                    "kind": "render",
                    "capture_format": "png",
                    "actual_hex": "89504e470d0a1a0a",
                },
            )
            golden = root / "tools" / "harness" / "visual" / "goldens" / "canvas2d" / "paint.png"
            golden.parent.mkdir(parents=True, exist_ok=True)
            golden.write_bytes(b"not-png")

            rc = runner.verify(
                binary,
                root,
                "canvas2d",
                [fixture],
                actuals_dir=root / "actuals",
            )

            self.assertEqual(rc, 1)
            actual = root / "actuals" / "canvas2d" / "paint.png"
            self.assertEqual(actual.read_bytes(), bytes.fromhex("89504e470d0a1a0a"))

    def test_verify_png_accepts_matching_bytes_and_rejects_bad_capture_format(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            binary = self._write_fake_visual_binary(root)
            fixture = self._write_fixture(
                root,
                "canvas2d",
                "paint",
                {
                    "id": "canvas2d/paint",
                    "kind": "render",
                    "capture_format": "png",
                    "actual_hex": "89504e47",
                },
            )
            golden = root / "tools" / "harness" / "visual" / "goldens" / "canvas2d" / "paint.png"
            golden.parent.mkdir(parents=True)
            golden.write_bytes(bytes.fromhex("89504e47"))

            self.assertEqual(runner.verify(binary, root, "canvas2d", [fixture]), 0)

            bad = self._write_fixture(
                root,
                "canvas2d",
                "bad",
                {"id": "canvas2d/bad", "capture_format": "bmp"},
            )
            with self.assertRaisesRegex(ValueError, "unsupported visual capture format"):
                runner.generate(binary, root, "canvas2d", [bad])

    def test_generate_requires_explicit_all_or_entry(self) -> None:
        # Codex P2 on PR #1598 — `--generate` without `--all` or
        # `--entry` previously regenerated every golden silently.
        # Now require an explicit opt-in to bulk rewrites.
        buf = io.StringIO()
        with redirect_stderr(buf):
            rc = runner.main(["--generate", "--surface", "yoga"])
        self.assertEqual(rc, 2)
        self.assertIn("--generate requires", buf.getvalue())
        self.assertIn("--all", buf.getvalue())
        self.assertIn("--entry", buf.getvalue())

    def test_verify_with_all_emits_deprecation_warning(self) -> None:
        # `--all` is redundant with `--verify` (omitting --entry already
        # runs every fixture). Emit a warning to discourage the pattern.
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self._write_fixture_with_golden(root, "yoga", "a")
            buf = io.StringIO()
            with redirect_stderr(buf):
                # We expect either rc != 0 (no binary built) OR rc == 0;
                # the warning must be emitted regardless. Use --build-dir
                # pointing at a non-existent path so we exit early after
                # the warning (binary not found → rc=2 with FileNotFoundError).
                runner.main([
                    "--verify",
                    "--all",
                    "--surface", "yoga",
                    "--repo-root", str(root),
                    "--build-dir", str(root / "build-nonexistent"),
                ])
            self.assertIn("--all is redundant with --verify", buf.getvalue())

    def test_generate_with_all_does_not_complain_about_missing_entry(self) -> None:
        # The `--all` flag IS the explicit opt-in for `--generate`, so
        # `pulp harness visual --generate --surface=yoga --all` should
        # NOT print the "requires --all OR --entry" error.
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self._write_fixture_with_golden(root, "yoga", "a")
            buf = io.StringIO()
            with redirect_stderr(buf):
                runner.main([
                    "--generate",
                    "--all",
                    "--surface", "yoga",
                    "--repo-root", str(root),
                    "--build-dir", str(root / "build-nonexistent"),
                ])
            self.assertNotIn("--generate requires", buf.getvalue())

    def test_main_rejects_all_plus_entry_and_strips_visual_prefix(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            fixture = self._write_fixture(root, "yoga", "a", {"actual": {"value": 1}})
            golden = root / "tools" / "harness" / "visual" / "goldens" / "yoga" / "a.json"
            golden.parent.mkdir(parents=True)
            golden.write_text(json.dumps({"value": 1}), encoding="utf-8")
            binary = self._write_fake_visual_binary(root)

            buf = io.StringIO()
            with redirect_stderr(buf):
                rc = runner.main(["--all", "--entry", "a"])
            self.assertEqual(rc, 2)
            self.assertIn("pass --all OR --entry", buf.getvalue())

            self.assertEqual(
                runner.main([
                    "visual",
                    "--verify",
                    "--surface",
                    "yoga",
                    "--entry",
                    "a",
                    "--repo-root",
                    str(root),
                    "--binary",
                    str(binary),
                ]),
                0,
            )

            self.assertTrue(fixture.exists())

    def _write_fixture_with_golden(self, root: Path, surface: str, name: str) -> None:
        fixture_dir = root / "tools" / "harness" / "visual" / "fixtures" / surface
        golden_dir = root / "tools" / "harness" / "visual" / "goldens" / surface
        fixture_dir.mkdir(parents=True, exist_ok=True)
        golden_dir.mkdir(parents=True, exist_ok=True)
        (fixture_dir / f"{name}.json").write_text(
            json.dumps({"id": f"{surface}/{name}"}),
            encoding="utf-8",
        )
        (golden_dir / f"{name}.json").write_text("{}", encoding="utf-8")

    def _write_fixture(
        self,
        root: Path,
        surface: str,
        name: str,
        payload: dict[str, object],
    ) -> Path:
        fixture_dir = root / "tools" / "harness" / "visual" / "fixtures" / surface
        fixture_dir.mkdir(parents=True, exist_ok=True)
        fixture = fixture_dir / f"{name}.json"
        fixture.write_text(json.dumps(payload), encoding="utf-8")
        return fixture

    def _write_fake_visual_binary(self, root: Path) -> Path:
        binary = root / "fake-pulp-test-visual"
        binary.write_text(
            "\n".join(
                [
                    "#!/usr/bin/env python3",
                    "import json",
                    "import sys",
                    "from pathlib import Path",
                    "fixture = Path(sys.argv[sys.argv.index('--fixture') + 1])",
                    "data = json.loads(fixture.read_text(encoding='utf-8'))",
                    "fmt = data.get('capture_format') or data.get('captureFormat')",
                    "capture = data.get('capture')",
                    "if isinstance(capture, dict):",
                    "    fmt = fmt or capture.get('format')",
                    "fmt = (fmt or ('png' if data.get('kind') == 'render' else 'json')).lower()",
                    "if fmt == 'png':",
                    "    sys.stdout.buffer.write(bytes.fromhex(data.get('actual_hex', '89504e47')))",
                    "else:",
                    "    json.dump(data.get('actual', {}), sys.stdout)",
                    "",
                ]
            ),
            encoding="utf-8",
        )
        binary.chmod(0o755)
        return binary


if __name__ == "__main__":
    unittest.main()
