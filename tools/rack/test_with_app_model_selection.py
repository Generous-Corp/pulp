#!/usr/bin/env python3
"""Headless checks for the Forge app-selection and Codex-context boundary."""
from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


HERE = Path(__file__).resolve().parent
WRAPPER = HERE / "with_app_model_selection.py"
ENV_NAMES = (
    "FORGE_MODEL_PROVIDER",
    "FORGE_CLAUDE_BIN",
    "FORGE_CLAUDE_MODEL",
    "FORGE_CLAUDE_REASONING_EFFORT",
    "FORGE_CODEX_BIN",
    "FORGE_CODEX_MODEL",
    "FORGE_CODEX_REASONING_EFFORT",
    "FORGE_CODEX_HOME",
    "FORGE_CODEX_PROFILE",
    "CODEX_BIN",
    "CODEX_HOME",
)
READ_ENV = (
    "import json, os; "
    f"print(json.dumps({{name: os.environ.get(name) for name in {ENV_NAMES!r}}}, "
    "sort_keys=True))"
)


class AppModelSelectionTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        root = Path(self.temp.name)
        self.settings = root / "settings.json"
        self.codex = root / "codex"
        self.codex.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        self.codex.chmod(0o755)
        self.codex_home = root / "codex-home"
        self.codex_home.mkdir()

    def codex_settings(self, **overrides: object) -> dict:
        document = {
            "dsp_provider": "codex",
            "dsp_model": "gpt-5.6-sol",
            "reasoning_effort": "medium",
            "codex_executable": str(self.codex),
            "codex_home": str(self.codex_home),
        }
        document.update(overrides)
        return document

    def write_settings(self, document: dict) -> None:
        self.settings.write_text(json.dumps(document), encoding="utf-8")

    def run_wrapper(self, *, environment: dict[str, str] | None = None,
                    settings: Path | None = None,
                    command: list[str] | None = None,
                    role: str | None = None) -> subprocess.CompletedProcess:
        env = dict(os.environ)
        env.update({name: f"stale-{name.lower()}" for name in ENV_NAMES})
        if environment:
            env.update(environment)
        argv = [sys.executable, str(WRAPPER), "--settings",
                str(settings or self.settings)]
        if role is not None:
            argv += ["--role", role]
        argv += ["--"]
        argv += command or [sys.executable, "-c", READ_ENV]
        return subprocess.run(argv, capture_output=True, text=True, env=env,
                              timeout=10, check=False)

    def selected_environment(self, **kwargs) -> dict[str, str | None]:
        result = self.run_wrapper(**kwargs)
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(result.stdout)

    def test_codex_route_exports_exact_saved_context_and_clears_stale_routes(self) -> None:
        self.write_settings(self.codex_settings(
            dsp_model="gpt-5.6", reasoning_effort="high"))

        captured = self.selected_environment()

        self.assertEqual(captured["FORGE_MODEL_PROVIDER"], "codex")
        self.assertEqual(captured["FORGE_CODEX_MODEL"], "gpt-5.6-sol")
        self.assertEqual(captured["FORGE_CODEX_REASONING_EFFORT"], "high")
        self.assertEqual(captured["FORGE_CODEX_BIN"], str(self.codex))
        self.assertEqual(captured["FORGE_CODEX_HOME"], str(self.codex_home))
        self.assertEqual(captured["CODEX_HOME"], str(self.codex_home))
        for name in ("FORGE_CLAUDE_BIN", "FORGE_CLAUDE_MODEL",
                     "FORGE_CLAUDE_REASONING_EFFORT", "FORGE_CODEX_PROFILE",
                     "CODEX_BIN"):
            self.assertIsNone(captured[name], name)

    def test_explicit_role_selects_the_same_distinct_ui_or_dsp_model(self) -> None:
        self.write_settings(self.codex_settings(
            dsp_model="gpt-5.6-sol",
            ui_provider="codex", ui_model="gpt-5.6-terra"))

        dsp = self.selected_environment(role="dsp")
        ui = self.selected_environment(role="ui")

        self.assertEqual(dsp["FORGE_CODEX_MODEL"], "gpt-5.6-sol")
        self.assertEqual(ui["FORGE_CODEX_MODEL"], "gpt-5.6-terra")
        self.assertEqual(dsp["FORGE_CODEX_BIN"], ui["FORGE_CODEX_BIN"])
        self.assertEqual(dsp["CODEX_HOME"], ui["CODEX_HOME"])

    def test_default_role_remains_dsp_and_role_can_fall_back_to_codex_default(self) -> None:
        document = self.codex_settings(
            default_provider="codex", default_model="gpt-default")
        del document["dsp_provider"]
        del document["dsp_model"]
        self.write_settings(document)

        captured = self.selected_environment()

        self.assertEqual(captured["FORGE_CODEX_MODEL"], "gpt-default")

    def test_default_path_follows_the_same_projects_directory_override(self) -> None:
        projects = Path(self.temp.name) / "projects"
        projects.mkdir()
        self.settings = projects / "settings.json"
        self.write_settings(self.codex_settings())
        env = dict(os.environ)
        env["FORGE_PROJECTS_DIR"] = str(projects)
        argv = [sys.executable, str(WRAPPER), "--", sys.executable, "-c", READ_ENV]

        result = subprocess.run(argv, capture_output=True, text=True, env=env,
                                timeout=10, check=False)

        self.assertEqual(result.returncode, 0, result.stderr)
        captured = json.loads(result.stdout)
        self.assertEqual(captured["FORGE_CODEX_BIN"], str(self.codex))
        self.assertEqual(captured["CODEX_HOME"], str(self.codex_home))

    def test_claude_selection_is_rejected_without_launching_the_child(self) -> None:
        self.write_settings(self.codex_settings(
            dsp_provider="claude", dsp_model="claude-opus-5"))
        marker = Path(self.temp.name) / "claude-child-ran"
        child = [sys.executable, "-c",
                 "from pathlib import Path; import sys; "
                 "Path(sys.argv[1]).write_text('ran')", str(marker)]

        result = self.run_wrapper(command=child)

        self.assertEqual(result.returncode, 2, result)
        self.assertIn("requires codex", result.stderr)
        self.assertFalse(marker.exists())

    def test_invalid_or_incomplete_context_fails_before_the_child_runs(self) -> None:
        missing_executable = Path(self.temp.name) / "missing-codex"
        plain_file = Path(self.temp.name) / "plain-codex"
        plain_file.write_text("not executable", encoding="utf-8")
        missing_home = Path(self.temp.name) / "missing-home"
        cases = (
            ("missing", None, "cannot read"),
            ("corrupt", "{", "cannot parse"),
            ("invalid-utf8", b"\xff", "cannot decode"),
            ("array", "[]", "one JSON object"),
            ("no-provider", self.codex_settings(dsp_provider=None),
             "must be a non-empty string"),
            ("unknown-provider", self.codex_settings(dsp_provider="openrouter"),
             "requires codex"),
            ("empty-model", self.codex_settings(dsp_model=" "),
             "must be a non-empty string"),
            ("malformed-model", self.codex_settings(dsp_model="--profile surprise"),
             "saved model is malformed"),
            ("unknown-effort", self.codex_settings(reasoning_effort="ultra"),
             "must be one of"),
            ("no-executable", self.codex_settings(codex_executable=""),
             "must be a non-empty string"),
            ("relative-executable", self.codex_settings(codex_executable="codex"),
             "must be an absolute path"),
            ("missing-executable", self.codex_settings(
                codex_executable=str(missing_executable)), "must be an executable file"),
            ("plain-executable", self.codex_settings(
                codex_executable=str(plain_file)), "must be an executable file"),
            ("relative-home", self.codex_settings(codex_home=".codex"),
             "must be an absolute path"),
            ("missing-home", self.codex_settings(codex_home=str(missing_home)),
             "must be an existing directory"),
            ("duplicate", '{"dsp_provider":"codex","dsp_provider":"codex"}',
             "repeats key"),
        )
        for name, body, expected in cases:
            with self.subTest(name=name):
                settings = Path(self.temp.name) / f"{name}.json"
                if body is not None:
                    if isinstance(body, bytes):
                        settings.write_bytes(body)
                    elif isinstance(body, str):
                        settings.write_text(body, encoding="utf-8")
                    else:
                        settings.write_text(json.dumps(body), encoding="utf-8")
                marker = Path(self.temp.name) / f"{name}.ran"
                child = [sys.executable, "-c",
                         "from pathlib import Path; import sys; "
                         "Path(sys.argv[1]).write_text('ran')", str(marker)]

                result = self.run_wrapper(settings=settings, command=child)

                self.assertEqual(result.returncode, 2, result)
                self.assertIn(expected, result.stderr)
                self.assertFalse(marker.exists(), "invalid context launched child")


if __name__ == "__main__":
    unittest.main()
