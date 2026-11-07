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

# The only modules allowed to import `artifacts`: the report assembler that
# attaches the block after the verdict is final, and the CLI that forwards it.
ALLOWED_IMPORTERS = {"pipeline.py", "cli.py"}


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
        "sys.exit(0)\n"
    )
    script.chmod(script.stat().st_mode | stat.S_IXUSR)
    return script


@pytest.fixture
def no_pulp_bin(monkeypatch):
    monkeypatch.delenv(artifacts.PULP_BIN_ENV, raising=False)


def test_report_with_and_without_artifacts_has_identical_gate_view(tmp_path, monkeypatch):
    without = pipeline.run("identity")
    before = _gated_view(without)

    # Skipped fence: nothing to render, block records why.
    monkeypatch.delenv(artifacts.PULP_BIN_ENV, raising=False)
    skipped = artifacts.attach(pipeline.run("identity"), {"reference": "r.wav"}, str(tmp_path))
    assert _gated_view(skipped) == before
    block = skipped["advisory"]["artifacts"]
    assert block["display_only"] is True
    assert block["images"]["reference"]["status"] == "skipped"
    assert "PULP_BIN" in block["images"]["reference"]["reason"]

    # Fake CLI renders: the block fills in, the gate view does not move.
    fake = _write_fake_pulp(tmp_path)
    monkeypatch.setenv(artifacts.PULP_BIN_ENV, str(fake))
    wav = tmp_path / "ref.wav"
    wav.write_bytes(b"RIFF")
    rendered = artifacts.attach(pipeline.run("identity"), {"reference": str(wav)}, str(tmp_path))
    assert _gated_view(rendered) == before
    images = rendered["advisory"]["artifacts"]["images"]["reference"]
    assert images["status"] == "ok"
    kinds = {item["kind"] for item in images["items"]}
    assert kinds == {"spectrogram", "waveform"}
    assert (tmp_path / "reference.spectrogram.png").exists()
    assert (tmp_path / "reference.waveform.png.json").exists()
    # The attached block is the only place the images appear.
    assert "artifacts" not in rendered
    assert set(rendered.keys()) == set(without.keys())


def test_no_verdict_participating_module_imports_artifacts():
    offenders = []
    for path in sorted(PACKAGE_DIR.rglob("*.py")):
        if path.name == "artifacts.py":
            continue
        tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
        for node in ast.walk(tree):
            names: list[str] = []
            if isinstance(node, ast.Import):
                names = [alias.name for alias in node.names]
            elif isinstance(node, ast.ImportFrom):
                module = node.module or ""
                names = [module] + [f"{module}.{alias.name}" if module else alias.name for alias in node.names]
            if any(name == "artifacts" or name.endswith(".artifacts") for name in names):
                offenders.append(path.relative_to(PACKAGE_DIR).as_posix())
    unexpected = [o for o in offenders if Path(o).name not in ALLOWED_IMPORTERS or "/" in o]
    assert not unexpected, f"only the report assembler may import artifacts: {unexpected}"
    # Control: the assembler really does import it, so the scan can see imports.
    assert "pipeline.py" in offenders


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
    script.write_text("#!/usr/bin/env python3\nimport sys\nsys.stderr.write('Error: no frames\\n')\nsys.exit(2)\n")
    script.chmod(script.stat().st_mode | stat.S_IXUSR)
    monkeypatch.setenv(artifacts.PULP_BIN_ENV, str(script))
    block = artifacts.render(str(tmp_path / "x.wav"), str(tmp_path))
    assert block["status"] == "partial"
    assert all(item["status"] == "refused" and item["exit_code"] == 2 for item in block["items"])
    assert all("no frames" in item["reason"] for item in block["items"])
