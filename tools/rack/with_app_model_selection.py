#!/usr/bin/env python3
"""Run a headless Forge command with the app's saved Codex context.

The Forge UI persists its provider, model, reasoning effort, Codex executable,
and CODEX_HOME in settings.json. The app snapshots those values into FORGE_*
variables immediately before it launches the Rack generator. This wrapper gives
headless runs the same boundary: read the document once, validate the exact
Codex route, clear inherited provider/context variables, and replace this
process with the requested command.

Unlike Forge's interactive settings loader, this path has no built-in defaults.
A missing, corrupt, incomplete, or unsupported selection is an invocation error,
because silently choosing a model would make a headless comparison unrepeatable.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
import os
from pathlib import Path
import re
import sys
from typing import Mapping


REASONING_EFFORTS = ("low", "medium", "high", "max")
MODEL_PATTERN = re.compile(r"[A-Za-z0-9][A-Za-z0-9._:/-]*\Z")
SELECTION_ENV_VARS = (
    "FORGE_MODEL_PROVIDER",
    "FORGE_CLAUDE_BIN",
    "FORGE_CLAUDE_MODEL",
    "FORGE_CODEX_MODEL",
    "FORGE_CLAUDE_REASONING_EFFORT",
    "FORGE_CODEX_REASONING_EFFORT",
    "FORGE_CODEX_BIN",
    "FORGE_CODEX_HOME",
    "FORGE_CODEX_PROFILE",
    "CODEX_BIN",
    "CODEX_HOME",
)


class SelectionError(ValueError):
    """The saved app selection is absent, ambiguous, or unusable."""


@dataclass(frozen=True)
class ModelSelection:
    provider: str
    model: str
    reasoning_effort: str
    codex_executable: str
    codex_home: str


def default_settings_path(environment: Mapping[str, str]) -> Path:
    """Locate the Forge Modular settings file without inventing a value."""
    projects = environment.get("FORGE_PROJECTS_DIR", "").strip()
    if projects:
        return Path(projects).expanduser() / "settings.json"
    home = environment.get("HOME", "").strip()
    if not home:
        raise SelectionError(
            "cannot locate settings.json: pass --settings or set HOME")
    return (Path(home).expanduser() / "Library" / "Application Support" /
            "Forge Modular" / "projects" / "settings.json")


def _object_without_duplicates(pairs: list[tuple[str, object]]) -> dict:
    result = {}
    for key, value in pairs:
        if key in result:
            raise SelectionError(f"settings.json repeats key {key!r}")
        result[key] = value
    return result


def _saved_string(document: dict, primary: str, fallback: str | None = None) -> str:
    key = primary
    if key not in document and fallback is not None:
        key = fallback
    if key not in document:
        names = primary if fallback is None else f"{primary!r} or {fallback!r}"
        raise SelectionError(f"settings.json has no saved {names} value")
    value = document[key]
    if not isinstance(value, str) or not value.strip():
        raise SelectionError(f"settings.json key {key!r} must be a non-empty string")
    return value


def load_selection(path: Path, role: str = "dsp") -> ModelSelection:
    """Read one validated role selection and Codex context from ``path``."""
    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as exc:
        if isinstance(exc, UnicodeDecodeError):
            raise SelectionError(
                f"cannot decode {path} as UTF-8 settings JSON: {exc}") from exc
        raise SelectionError(f"cannot read {path}: {exc.strerror or exc}") from exc
    try:
        document = json.loads(text, object_pairs_hook=_object_without_duplicates)
    except SelectionError:
        raise
    except json.JSONDecodeError as exc:
        raise SelectionError(f"cannot parse {path} as settings JSON: {exc}") from exc
    if not isinstance(document, dict):
        raise SelectionError("settings.json must contain one JSON object")

    provider = _saved_string(document, f"{role}_provider", "default_provider")
    model = _saved_string(document, f"{role}_model", "default_model")
    effort = _saved_string(document, "reasoning_effort")

    if provider != "codex":
        raise SelectionError(
            f"unsupported saved provider {provider!r}; Forge Modular Rack "
            "generation requires codex")
    if not MODEL_PATTERN.fullmatch(model):
        raise SelectionError(
            "saved model is malformed; expected one provider model identifier")
    if effort not in REASONING_EFFORTS:
        raise SelectionError(
            "saved reasoning effort must be one of: " +
            ", ".join(REASONING_EFFORTS))

    executable = _saved_string(document, "codex_executable")
    codex_home = _saved_string(document, "codex_home")
    executable_path = Path(executable).expanduser()
    home_path = Path(codex_home).expanduser()
    if not executable_path.is_absolute():
        raise SelectionError(
            "saved codex_executable must be an absolute path")
    if not executable_path.is_file() or not os.access(executable_path, os.X_OK):
        raise SelectionError(
            "saved codex_executable must be an executable file")
    if not home_path.is_absolute():
        raise SelectionError("saved codex_home must be an absolute path")
    if not home_path.is_dir():
        raise SelectionError("saved codex_home must be an existing directory")

    # Forge previously persisted the friendly GPT-5.6 family alias. The
    # catalogue now shows that label on the runnable Sol variant, and the app
    # migrates the old saved slug at its provider boundary.
    if provider == "codex" and model == "gpt-5.6":
        model = "gpt-5.6-sol"
    return ModelSelection(provider, model, effort,
                          str(executable_path), str(home_path))


def selected_environment(selection: ModelSelection,
                         base: Mapping[str, str]) -> dict[str, str]:
    """Return a child environment containing only the selected model route."""
    environment = dict(base)
    for name in SELECTION_ENV_VARS:
        environment.pop(name, None)
    environment["FORGE_MODEL_PROVIDER"] = selection.provider
    environment["FORGE_CODEX_MODEL"] = selection.model
    environment["FORGE_CODEX_REASONING_EFFORT"] = selection.reasoning_effort
    environment["FORGE_CODEX_BIN"] = selection.codex_executable
    environment["FORGE_CODEX_HOME"] = selection.codex_home
    environment["CODEX_HOME"] = selection.codex_home
    return environment


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run a command with Forge's saved engineering model selection.")
    parser.add_argument(
        "--settings", type=Path,
        help=("settings.json to snapshot; defaults to FORGE_PROJECTS_DIR/"
              "settings.json or Forge Modular's standard app path"))
    parser.add_argument(
        "--role", choices=("dsp", "ui"), default="dsp",
        help=("Forge model role to snapshot; Forge Modular module/patch "
              "generation uses ui, while dsp remains the legacy engineering "
              "caller default (default: dsp)"))
    parser.add_argument(
        "command", nargs=argparse.REMAINDER,
        help="command and arguments, conventionally after --")
    args = parser.parse_args(argv)
    if args.command and args.command[0] == "--":
        args.command = args.command[1:]
    if not args.command:
        parser.error("a command is required after --")
    return args


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        settings = args.settings or default_settings_path(os.environ)
        selection = load_selection(settings, args.role)
        environment = selected_environment(selection, os.environ)
        os.execvpe(args.command[0], args.command, environment)
    except SelectionError as exc:
        print(f"with-app-model-selection: {exc}", file=sys.stderr)
        return 2
    except OSError as exc:
        print(f"with-app-model-selection: cannot run {args.command[0]!r}: {exc}",
              file=sys.stderr)
        return 2
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
