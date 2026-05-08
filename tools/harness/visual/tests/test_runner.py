"""Tests for visual snapshot fixture discovery and coverage counts."""

from __future__ import annotations

import io
import json
import sys
import tempfile
import unittest
from contextlib import redirect_stderr
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[3]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tools.harness.visual import runner  # noqa: E402
from tools.harness.visual import spec  # noqa: E402


class RunnerTests(unittest.TestCase):
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
