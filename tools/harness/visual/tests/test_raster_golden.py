"""Byte-identity gate for the committed raster golden.

What this proves, and what it does not:

  * PROVEN, on whatever host runs it: the pinned rasterizer produces the same
    PNG bytes every time, across separate processes, and those bytes equal the
    committed golden. That is a real within-host determinism gate.
  * NOT PROVEN: that the bytes are identical on every host. The golden was
    recorded on darwin-arm64 and confirmed on darwin-x86_64; no Linux
    measurement existed when it was recorded. The Linux lane runs this same
    comparison, so it either confirms cross-host identity or reports the exact
    digest that disproves it. Until it has run, treat the claim as open.

A skip is never a pass here: CI sets PULP_VISUAL_REQUIRE_SKIA=1, which turns a
missing rasterizer into a hard failure naming the absent dependency.
"""

from __future__ import annotations

import hashlib
import json
import subprocess
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[3]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tools.harness.visual import pins  # noqa: E402
from tools.harness.visual import raster  # noqa: E402
from tools.harness.visual import runner  # noqa: E402
from tools.harness.visual import spec as visual_spec  # noqa: E402

FIXTURE_ID = "canvas2d/raster-determinism"
SURFACE, ENTRY = FIXTURE_ID.split("/")
FIXTURE_PATH = REPO_ROOT / "tools/harness/visual/fixtures" / SURFACE / f"{ENTRY}.json"
GOLDEN_PATH = REPO_ROOT / "tools/harness/visual/goldens" / SURFACE / f"{ENTRY}.png"


def _skia_or_skip():
    try:
        return raster.import_locked_skia()
    except raster.LockedSkiaUnavailable as exc:
        if raster.require_locked_skia_env():
            raise AssertionError(
                f"PULP_VISUAL_REQUIRE_SKIA=1 but the raster gate cannot run: {exc}"
            ) from exc
        pytest.skip(str(exc))


def _sha256(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def test_committed_golden_matches_recorded_digest() -> None:
    """Golden bytes and recorded digest agree, with no rasterizer involved.

    This runs on every host regardless of Skia, so a golden regenerated without
    updating its recorded digest (or the reverse) is caught even where the
    raster dependency is unavailable.
    """
    assert GOLDEN_PATH.is_file(), f"missing committed golden {GOLDEN_PATH}"
    expected = pins.RASTER_GOLDEN_SHA256[FIXTURE_ID]
    actual = _sha256(GOLDEN_PATH.read_bytes())
    assert actual == expected, (
        f"committed golden {GOLDEN_PATH.relative_to(REPO_ROOT)} has sha256 {actual}, "
        f"but pins.RASTER_GOLDEN_SHA256[{FIXTURE_ID!r}] records {expected}. "
        "One of the two was changed without the other."
    )


def test_fixture_resolves_to_a_png_golden() -> None:
    """The fixture's declared kind is what routes it to a PNG golden."""
    fixture = visual_spec.fixture_spec_from_file(FIXTURE_PATH, default_surface=SURFACE)
    assert fixture.kind == "render"
    assert fixture.driver == raster.DECLARATIVE_RASTER_DRIVER
    assert fixture.capture_format == "png"
    assert visual_spec.golden_path(REPO_ROOT, fixture) == GOLDEN_PATH


def test_fixture_declares_no_text() -> None:
    """Text is excluded on purpose.

    Font rasterization (CoreText vs fontconfig/FreeType, hinting, subpixel
    positioning) is the largest source of cross-platform divergence. Keeping the
    fixture text-free means a byte mismatch indicts the raster stack rather than
    font plumbing.
    """
    fixture = json.loads(FIXTURE_PATH.read_text(encoding="utf-8"))
    ops = {op.get("op") for op in fixture["ops"]}
    assert "text" not in ops
    assert "text" not in FIXTURE_PATH.read_text(encoding="utf-8")


def test_raster_is_byte_stable_across_processes() -> None:
    """The determinism gate that holds on every host, including unmeasured ones.

    Two renders in this process plus one in a fresh interpreter must agree. This
    catches nondeterminism seeded by allocation order, hash seeds, or per-process
    Skia state, and it is meaningful even where the cross-host claim is not.
    """
    skia = _skia_or_skip()
    fixture = raster.load_fixture(FIXTURE_PATH)

    first = raster.render_fixture_png(skia, fixture)
    second = raster.render_fixture_png(skia, fixture)
    assert _sha256(first) == _sha256(second), (
        "the pinned rasterizer is not deterministic within a single process: "
        f"{_sha256(first)} then {_sha256(second)}"
    )

    script = (
        "import hashlib,sys;"
        f"sys.path.insert(0, {str(REPO_ROOT)!r});"
        "from tools.harness.visual import raster;"
        "skia = raster.import_locked_skia();"
        f"png = raster.render_fixture_png(skia, raster.load_fixture({str(FIXTURE_PATH)!r}));"
        "sys.stdout.write(hashlib.sha256(png).hexdigest())"
    )
    proc = subprocess.run(
        [sys.executable, "-B", "-c", script],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    assert proc.returncode == 0, (
        "fresh-interpreter render failed: "
        f"{proc.stderr.decode('utf-8', errors='replace').strip()}"
    )
    assert proc.stdout.decode().strip() == _sha256(first), (
        "the pinned rasterizer is not deterministic across processes: "
        f"in-process {_sha256(first)}, subprocess {proc.stdout.decode().strip()}"
    )


def test_raster_matches_committed_golden() -> None:
    """The cross-host comparison, stated honestly in its failure message."""
    skia = _skia_or_skip()
    actual = raster.render_fixture_png(skia, raster.load_fixture(FIXTURE_PATH))
    expected = GOLDEN_PATH.read_bytes()
    key = raster.platform_key()

    if actual == expected:
        return

    from tools.harness.visual import differ

    detail = differ.format_byte_difference(expected, actual)
    if key in pins.RASTER_GOLDEN_VERIFIED_PLATFORMS:
        raise AssertionError(
            f"{FIXTURE_ID} no longer matches its golden on {key}, a platform where "
            f"the golden was recorded as verified: {detail}. "
            "Something in the pinned raster stack changed."
        )
    raise AssertionError(
        f"{FIXTURE_ID} does not match its golden on {key}: {detail}. "
        f"This host is not in pins.RASTER_GOLDEN_VERIFIED_PLATFORMS "
        f"{pins.RASTER_GOLDEN_VERIFIED_PLATFORMS}, so this is the first measurement "
        f"here and it DISPROVES cross-host byte identity. The digest on this host is "
        f"{_sha256(actual)}. Record it as a per-platform expectation rather than "
        "regenerating the shared golden."
    )


def test_harness_runner_verifies_the_fixture() -> None:
    """The golden is reachable through the harness runner, not only this test."""
    _skia_or_skip()
    rc = runner.verify(
        None,
        REPO_ROOT,
        SURFACE,
        [FIXTURE_PATH],
    )
    assert rc == 0, f"runner.verify rejected {FIXTURE_ID}"
