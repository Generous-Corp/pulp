"""Display-only image artifacts never touch the verdict.

Three structural guarantees, each a gate for the `advisory.artifacts` block:

1. A report built with and without artifacts attached has byte-identical
   `verdict`, `worst_regions` and detector results, whether the CLI fence is
   unset (skipped) or a fake CLI renders real files.
2. No module under `quality_lab/` other than the report assembler imports the
   artifacts module, so a detector can never reach `advisory.artifacts`.
3. The attached block carries no absolute paths.
"""
from __future__ import annotations

import ast
import json
import os
import stat
import sys
from pathlib import Path

import pytest

from quality_lab import artifacts, pipeline

PACKAGE_DIR = Path(artifacts.__file__).resolve().parent



def _gated_view(report: dict) -> str:
    """Everything the gate sees, serialized deterministically."""
    return json.dumps(
        {
            "verdict": report["verdict"],
            "worst_regions": report["worst_regions"],
            "detectors": report["detectors"],
            "advisory_detectors": report["advisory"]["detectors"],
        },
        sort_keys=True,
    )


def _write_fake_pulp(path: Path) -> Path:
    """A stand-in `pulp` that honours `audio spectrogram|waveform <file> --png <p> --json <j>`
    by writing a PNG signature and a basename-only sidecar, so the attach path is
    exercised end to end without a built CLI. Its sidecar deliberately includes a
    tempting `peak_db` so the byte-identity test would catch a detector reading it."""
    script = path / "pulp"
    script.write_text(
        "#!/usr/bin/env python3\n"
        "import json, os, sys\n"
        "args = sys.argv[1:]\n"
        "verb = args[1]\n"
        "png = args[args.index('--png') + 1]\n"
        "side = args[args.index('--json') + 1]\n"
        "with open(png, 'wb') as f:\n"
        "    f.write(b'\\x89PNG\\r\\n\\x1a\\n' + b'\\x00' * 16)\n"
        "doc = {'schema': 'pulp.audio.image.v1', 'display_only': True, 'verb': verb,\n"
        "       'input': {'name': os.path.basename(args[2]), 'sha256': '0' * 64},\n"
        "       'true_peak': {'linear': 0.5, 'dbfs': -6.02, 'clipped': False, 'peak_db': -6.02},\n"
        "       'settings': {'fft': 2048}}\n"
        "with open(side, 'w') as f:\n"
        "    json.dump(doc, f)\n"
        "sys.exit(0)\n",
        encoding="utf-8",
    )
    script.chmod(script.stat().st_mode | stat.S_IXUSR)
    return script


@pytest.fixture
def no_pulp_bin(monkeypatch):
    monkeypatch.delenv(artifacts.PULP_BIN_ENV, raising=False)


def test_report_with_and_without_artifacts_has_identical_gate_view(tmp_path, monkeypatch):
    """Drive the REAL attach site (pipeline.run_and_export) twice over the same
    deterministic case: once with the CLI fence unset, once with a fake CLI that
    renders files. Every gated field must come out byte-identical, and the
    plain pipeline.run() report (which never attaches) must agree with both."""
    baseline = _gated_view(pipeline.run("identity"))

    monkeypatch.delenv(artifacts.PULP_BIN_ENV, raising=False)
    without = pipeline.run_and_export("identity", str(tmp_path / "without"))
    assert _gated_view(without) == baseline
    block = without["advisory"]["artifacts"]
    assert block["display_only"] is True
    for name in ("reference", "candidate"):
        assert block["images"][name]["status"] == "skipped"
        assert "PULP_BIN" in block["images"][name]["reason"]

    fake = _write_fake_pulp(tmp_path)
    monkeypatch.setenv(artifacts.PULP_BIN_ENV, str(fake))
    with_images = pipeline.run_and_export("identity", str(tmp_path / "with"))
    assert _gated_view(with_images) == baseline
    assert _gated_view(with_images) == _gated_view(without)
    images = with_images["advisory"]["artifacts"]["images"]
    for name in ("reference", "candidate"):
        assert images[name]["status"] == "ok"
        assert {item["kind"] for item in images[name]["items"]} == {"spectrogram", "waveform"}
        assert (tmp_path / "with" / f"{name}.spectrogram.png").exists()
        assert (tmp_path / "with" / f"{name}.waveform.png.json").exists()
    # The attached block is the only place the images appear.
    assert "artifacts" not in with_images
    assert set(with_images.keys()) == set(without.keys())


def test_attach_alone_never_touches_the_gate_view(tmp_path, monkeypatch):
    report = pipeline.run("identity")
    before = _gated_view(report)
    fake = _write_fake_pulp(tmp_path)
    monkeypatch.setenv(artifacts.PULP_BIN_ENV, str(fake))
    wav = tmp_path / "ref.wav"
    wav.write_bytes(b"RIFF")
    assert _gated_view(artifacts.attach(report, {"reference": str(wav)}, str(tmp_path))) == before


def _artifact_imports(path: Path) -> list[tuple[str, str | None]]:
    """Every import of the artifacts module in `path` as (kind, enclosing function
    or None for module level). Walks with parent links so a function-local import
    can be told from a module-level one."""
    tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    for parent in ast.walk(tree):
        for child in ast.iter_child_nodes(parent):
            setattr(child, "_parent", parent)
    found: list[tuple[str, str | None]] = []
    for node in ast.walk(tree):
        names: list[str] = []
        if isinstance(node, ast.Import):
            names = [alias.name for alias in node.names]
        elif isinstance(node, ast.ImportFrom):
            module = node.module or ""
            names = [module] + [f"{module}.{alias.name}" if module else alias.name for alias in node.names]
        else:
            continue
        if not any(name == "artifacts" or name.endswith(".artifacts") for name in names):
            continue
        scope = None
        cursor = getattr(node, "_parent", None)
        while cursor is not None:
            if isinstance(cursor, (ast.FunctionDef, ast.AsyncFunctionDef)):
                scope = cursor.name
                break
            cursor = getattr(cursor, "_parent", None)
        found.append((type(node).__name__, scope))
    return found


def test_only_the_export_assembler_imports_artifacts():
    """pipeline.py may import artifacts ONLY inside run_and_export (the one place
    the block is attached, after the verdict is final); no module-level import
    there, and no import anywhere else under quality_lab/ including cli.py. A
    detector or verdict helper that reaches for it fails here, by construction."""
    offenders: dict[str, list[tuple[str, str | None]]] = {}
    for path in sorted(PACKAGE_DIR.rglob("*.py")):
        if path.name == "artifacts.py":
            continue
        imports = _artifact_imports(path)
        if imports:
            offenders[path.relative_to(PACKAGE_DIR).as_posix()] = imports
    assert set(offenders) <= {"pipeline.py"}, f"unexpected importers of artifacts: {offenders}"
    # Control: the assembler really does import it, inside run_and_export only.
    assert "pipeline.py" in offenders
    assert all(scope == "run_and_export" for _, scope in offenders["pipeline.py"]), offenders["pipeline.py"]
    # Control for the scanner itself: a module-level import must be seen as such.
    probe = ast.parse("from . import artifacts\n")
    for parent in ast.walk(probe):
        for child in ast.iter_child_nodes(parent):
            setattr(child, "_parent", parent)
    module_level = [n for n in ast.walk(probe) if isinstance(n, ast.ImportFrom)]
    assert module_level and not isinstance(getattr(module_level[0], "_parent"), ast.FunctionDef)


def test_attached_block_carries_no_absolute_paths(tmp_path, monkeypatch):
    fake = _write_fake_pulp(tmp_path)
    monkeypatch.setenv(artifacts.PULP_BIN_ENV, str(fake))
    nested = tmp_path / "deep" / "er"
    nested.mkdir(parents=True)
    wav = nested / "speaker-0417.wav"
    wav.write_bytes(b"RIFF")
    report = artifacts.attach(pipeline.run("identity"), {"candidate": str(wav)}, str(tmp_path))
    text = json.dumps(report["advisory"]["artifacts"])
    assert str(tmp_path) not in text
    assert "/deep/er" not in text
    assert "speaker-0417.wav" in text  # basename survives; the layout does not

    def walk(value):
        if isinstance(value, dict):
            for v in value.values():
                yield from walk(v)
        elif isinstance(value, list):
            for v in value:
                yield from walk(v)
        elif isinstance(value, str):
            yield value

    for s in walk(report["advisory"]["artifacts"]):
        assert not os.path.isabs(s), s


def test_cli_failure_and_refusal_are_recorded_not_raised(tmp_path, monkeypatch):
    script = tmp_path / "pulp"
    script.write_text(
        "#!/usr/bin/env python3\nimport sys\nsys.stderr.write('Error: no frames\\n')\nsys.exit(2)\n",
        encoding="utf-8",
    )
    script.chmod(script.stat().st_mode | stat.S_IXUSR)
    monkeypatch.setenv(artifacts.PULP_BIN_ENV, str(script))
    block = artifacts.render(str(tmp_path / "x.wav"), str(tmp_path))
    assert block["status"] == "partial"
    assert all(item["status"] == "refused" and item["exit_code"] == 2 for item in block["items"])
    assert all("no frames" in item["reason"] for item in block["items"])


def test_failure_reason_is_redacted_to_basenames(tmp_path, monkeypatch):
    """The CLI echoes the input path in its errors ("not a readable file: <abs>");
    the stored reason must not carry the absolute input, the output directory or
    any of their parents."""
    script = tmp_path / "pulp"
    script.write_text(
        "#!/usr/bin/env python3\n"
        "import sys\n"
        "wav = sys.argv[3]\n"
        "png = sys.argv[sys.argv.index('--png') + 1]\n"
        "sys.stderr.write('Error: input is not a readable file: ' + wav + '\\n')\n"
        "sys.stderr.write('Error: failed to write PNG ' + png + '\\n')\n"
        "sys.exit(1)\n",
        encoding="utf-8",
    )
    script.chmod(script.stat().st_mode | stat.S_IXUSR)
    monkeypatch.setenv(artifacts.PULP_BIN_ENV, str(script))
    nested = tmp_path / "corpus" / "speaker-0417"
    nested.mkdir(parents=True)
    wav = nested / "take-03.wav"
    out_dir = tmp_path / "run-out"
    out_dir.mkdir()
    block = artifacts.render(str(wav), str(out_dir))
    assert block["status"] == "partial"
    for item in block["items"]:
        assert item["status"] == "failed"
        reason = item["reason"]
        assert str(tmp_path) not in reason
        assert "corpus/speaker-0417" not in reason
        assert "take-03.wav" in reason  # the basename survives, the layout does not
        assert not any(token.startswith("/") for token in reason.split()), reason


def test_render_with_nothing_requested_is_skipped(tmp_path, monkeypatch):
    monkeypatch.setenv(artifacts.PULP_BIN_ENV, str(_write_fake_pulp(tmp_path)))
    block = artifacts.render(str(tmp_path / "x.wav"), str(tmp_path), spectrogram=False, waveform=False)
    assert block["status"] == "skipped"
    assert block["items"] == []
