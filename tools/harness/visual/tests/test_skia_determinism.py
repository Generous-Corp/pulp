"""B.0 visual determinism smoke tests."""

from __future__ import annotations

import hashlib
import json
import os
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[3]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tools.harness.visual import pins  # noqa: E402


def _manifest_entry(name: str) -> dict:
    manifest = json.loads((REPO_ROOT / "tools/deps/manifest.json").read_text())
    for dep in manifest["dependencies"]:
        if dep["name"] == name:
            return dep
    raise AssertionError(f"{name} missing from tools/deps/manifest.json")


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def test_visual_dependency_pins_match_manifest() -> None:
    skia = _manifest_entry("Skia")
    determinism = skia["determinism"]

    assert determinism["skia_branch"] == pins.SKIA_BRANCH
    assert determinism["skia_builder_fork"] == pins.SKIA_BUILDER_FORK
    assert determinism["skia_python_smoke_version"] == pins.SKIA_PYTHON_SMOKE_VERSION

    # m149+ tracks the fork's branch HEAD rather than a specific commit;
    # determinism is anchored at the release_assets sha256 level instead.
    # `skia_commit` / `skia_builder_ref` / `bundled_pins` are not required
    # keys on the m149 entry.
    for platform, expected_sha in pins.RELEASE_ASSET_SHA256.items():
        assert determinism["release_assets"][platform]["sha256"] == expected_sha


def test_bundled_font_hashes_are_stable() -> None:
    for relpath, expected_sha in pins.FONT_SHA256.items():
        assert _sha256(REPO_ROOT / relpath) == expected_sha


def _import_locked_skia():
    try:
        import skia  # type: ignore[import-not-found]
    except ImportError as exc:
        if os.environ.get("PULP_VISUAL_REQUIRE_SKIA") == "1":
            raise AssertionError(
                "skia-python is required in this environment but could not "
                f"be imported ({exc}); install skia-python=="
                f"{pins.SKIA_PYTHON_SMOKE_VERSION} and its platform runtime "
                "libraries"
            ) from exc
        pytest.skip(
            "locked skia-python smoke dependency unavailable "
            f"({exc}); install skia-python=="
            f"{pins.SKIA_PYTHON_SMOKE_VERSION} with its platform runtime "
            "libraries or run ci/visual-harness.Dockerfile"
        )

    actual_version = getattr(skia, "__version__", None)
    if actual_version != pins.SKIA_PYTHON_SMOKE_VERSION:
        raise AssertionError(
            "wrong skia-python version for deterministic smoke: "
            f"expected {pins.SKIA_PYTHON_SMOKE_VERSION}, got {actual_version!r}"
        )
    return skia


def _render_picture_png(skia) -> bytes:
    width = 96
    height = 96
    recorder = skia.PictureRecorder()
    canvas = recorder.beginRecording(skia.Rect.MakeWH(width, height))

    canvas.clear(skia.ColorWHITE)

    fill = skia.Paint(AntiAlias=False)
    fill.setColor(skia.ColorSetARGB(255, 30, 96, 180))
    canvas.drawRect(skia.Rect.MakeXYWH(8, 10, 48, 34), fill)

    accent = skia.Paint(AntiAlias=True)
    accent.setColor(skia.ColorSetARGB(224, 245, 176, 47))
    canvas.drawCircle(62, 58, 18, accent)

    stroke = skia.Paint(AntiAlias=False, Style=skia.Paint.kStroke_Style)
    stroke.setColor(skia.ColorSetARGB(255, 20, 24, 31))
    stroke.setStrokeWidth(3)
    path = skia.Path()
    path.moveTo(12, 82)
    path.lineTo(84, 18)
    canvas.drawPath(path, stroke)

    picture = recorder.finishRecordingAsPicture()

    surface = skia.Surface(width, height)
    surface_canvas = surface.getCanvas()
    surface_canvas.clear(skia.ColorTRANSPARENT)
    surface_canvas.drawPicture(picture)
    image = surface.makeImageSnapshot()
    data = image.encodeToData(skia.kPNG, 100)
    return bytes(data)


def test_skia_picture_renders_byte_identical_twice() -> None:
    skia = _import_locked_skia()

    first = _render_picture_png(skia)
    second = _render_picture_png(skia)

    assert first == second
    assert hashlib.sha256(first).hexdigest() == hashlib.sha256(second).hexdigest()
