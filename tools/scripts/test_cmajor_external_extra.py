import argparse
import importlib.util
import io
import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock


SCRIPT = Path(__file__).resolve().parent / "cmajor_external.py"
SPEC = importlib.util.spec_from_file_location("cmajor_external", SCRIPT)
assert SPEC is not None
cmajor_external = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(cmajor_external)


class CmajorExternalExtraTests(unittest.TestCase):
    def make_source(self, root: Path) -> Path:
        source = root / "Test.cmajor"
        source.write_text("processor Test {}\n", encoding="utf-8")
        return source

    def write_patch(
        self,
        root: Path,
        *,
        filename: str = "Test.cmajorpatch",
        source: str = "Test.cmajor",
        raw: str | None = None,
    ) -> Path:
        patch = root / filename
        if raw is None:
            raw = json.dumps({"name": "Edge Test", "source": source})
        patch.write_text(raw, encoding="utf-8")
        return patch

    def test_resolve_patch_rejects_invalid_manifests(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            bad_json = self.write_patch(root, raw="{not-json")
            with self.assertRaisesRegex(
                cmajor_external.CmajorExternalError,
                "invalid patch JSON",
            ):
                cmajor_external.resolve_patch(str(bad_json))

            missing_source_key = self.write_patch(
                root,
                raw=json.dumps({"name": "No Source"}),
            )
            with self.assertRaisesRegex(
                cmajor_external.CmajorExternalError,
                "missing string 'source'",
            ):
                cmajor_external.resolve_patch(str(missing_source_key))

            missing_source_file = self.write_patch(root, source="missing.cmajor")
            with self.assertRaisesRegex(
                cmajor_external.CmajorExternalError,
                "source listed in manifest does not exist",
            ):
                cmajor_external.resolve_patch(str(missing_source_file))

    def test_resolve_patch_rejects_missing_file_and_wrong_suffix(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            with self.assertRaisesRegex(cmajor_external.CmajorExternalError, "not found"):
                cmajor_external.resolve_patch(str(root / "missing.cmajorpatch"))

            wrong_suffix = self.write_patch(root, filename="Test.json")
            with self.assertRaisesRegex(cmajor_external.CmajorExternalError, ".cmajorpatch"):
                cmajor_external.resolve_patch(str(wrong_suffix))

    def test_resolve_patch_accepts_repo_relative_path(self) -> None:
        repo_root = cmajor_external.repo_root()
        with tempfile.TemporaryDirectory(dir=repo_root) as tmp:
            root = Path(tmp)
            source = self.make_source(root)
            patch = self.write_patch(root, source=source.name)
            relative_patch = patch.relative_to(repo_root)

            resolved_patch, resolved_source, manifest = cmajor_external.resolve_patch(
                str(relative_patch)
            )

            self.assertEqual(resolved_patch, patch.resolve())
            self.assertEqual(resolved_source, source.resolve())
            self.assertEqual(manifest["name"], "Edge Test")

    def test_resolve_cmaj_skips_bad_explicit_path_then_uses_env(self) -> None:
        with tempfile.TemporaryDirectory() as tmp, mock.patch.dict(os.environ, {}, clear=False):
            root = Path(tmp)
            non_executable = root / "not-cmaj"
            non_executable.write_text("#!/bin/sh\n", encoding="utf-8")
            executable = root / "cmaj"
            executable.write_text("#!/bin/sh\n", encoding="utf-8")
            executable.chmod(0o755)

            os.environ["CMAJ_BIN"] = str(executable)

            with mock.patch.object(cmajor_external.shutil, "which", return_value=None):
                self.assertEqual(
                    cmajor_external.resolve_cmaj(str(non_executable)),
                    str(executable.resolve()),
                )
                self.assertEqual(cmajor_external.resolve_cmaj(None), str(executable.resolve()))

    def test_resolve_cmaj_uses_cmaj_exe_from_path(self) -> None:
        with tempfile.TemporaryDirectory() as tmp, mock.patch.dict(os.environ, {}, clear=True):
            executable = Path(tmp) / "cmaj.exe"
            executable.write_text("#!/bin/sh\n", encoding="utf-8")
            executable.chmod(0o755)

            with mock.patch.object(
                cmajor_external.shutil,
                "which",
                side_effect=[None, str(executable)],
            ):
                self.assertEqual(cmajor_external.resolve_cmaj(None), str(executable.resolve()))

    def test_build_generate_command_rejects_unsupported_target(self) -> None:
        with self.assertRaisesRegex(cmajor_external.CmajorExternalError, "unsupported target"):
            cmajor_external.build_generate_command(
                "/bin/cmaj",
                Path("Test.cmajorpatch"),
                target="juce",
                output="out",
                extra_args=[],
            )

    def test_cmd_generate_returns_subprocess_status(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.make_source(root)
            patch = self.write_patch(root)
            args = argparse.Namespace(
                patch=str(patch),
                cmaj="/fake/cmaj",
                dry_run=False,
                target="cpp",
                output=str(root / "out.cpp"),
                arg=["--extra"],
            )

            completed = mock.Mock(returncode=17)
            with mock.patch.object(cmajor_external, "resolve_cmaj", return_value="/fake/cmaj"):
                with mock.patch.object(
                    cmajor_external.subprocess,
                    "run",
                    return_value=completed,
                ) as run:
                    with mock.patch.object(
                        cmajor_external.sys,
                        "stdout",
                        new_callable=io.StringIO,
                    ):
                        self.assertEqual(cmajor_external.cmd_generate(args), 17)

            run.assert_called_once()
            self.assertEqual(run.call_args.args[0][0], "/fake/cmaj")
            self.assertIn("--extra", run.call_args.args[0])
            self.assertFalse(run.call_args.kwargs["check"])

    def test_cmd_generate_requires_tool_when_not_dry_run(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.make_source(root)
            patch = self.write_patch(root)
            args = argparse.Namespace(
                patch=str(patch),
                cmaj=None,
                dry_run=False,
                target="cpp",
                output=str(root / "out.cpp"),
                arg=[],
            )

            with mock.patch.object(cmajor_external, "resolve_cmaj", return_value=None):
                with self.assertRaisesRegex(
                    cmajor_external.CmajorExternalError,
                    "cmaj binary not found",
                ):
                    cmajor_external.cmd_generate(args)

    def test_main_reports_cmajor_external_error(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            missing = Path(tmp) / "missing.cmajorpatch"
            with mock.patch.object(
                cmajor_external.sys,
                "stderr",
                new_callable=io.StringIO,
            ) as stderr:
                self.assertEqual(cmajor_external.main(["doctor", "--patch", str(missing)]), 2)

            self.assertIn("patch file not found", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
