#!/usr/bin/env python3
"""GPU backend and render-receipt contract for pulp-design-ir-observe.

`--backend gpu` renders through Skia Graphite on a Dawn device and
`--render-receipt` records which rasterizer actually ran, the adapter, the
frame outcome, whether readback completed and whether the device survived the
frame. A GPU parity lane trusts that receipt only as far as these tests prove
it: a GPU request must never come back as a CPU render, a lost device must fail
the render, and each seeded `--gpu-fault` must produce the failure it names.

Exits 77 (skip) only when the observer itself reports that this machine has no
native GPU adapter; every other outcome is a pass or a failure.

Usage: test_design_ir_observe_gpu.py --observer <pulp-design-ir-observe>
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

FIXTURE = Path(__file__).resolve().parent / "fixtures/design-ir-observe/controls.ir.json"
OBSERVER: Path | None = None
SKIP = 77
NO_ADAPTER_ERRORS = ("no Dawn device", "no native GPU adapter")


def observe(out: Path, *args: str) -> subprocess.CompletedProcess:
    out.mkdir(parents=True, exist_ok=True)
    argv = [
        str(OBSERVER),
        "--input", str(FIXTURE),
        "--render", str(out / "render.png"),
        "--layout", str(out / "layout.json"),
        "--render-receipt", str(out / "receipt.json"),
        "--width", "240",
        "--height", "160",
        *args,
    ]
    return subprocess.run(argv, capture_output=True, text=True, encoding="utf-8", timeout=120)


def receipt(out: Path) -> dict:
    return json.loads((out / "receipt.json").read_text(encoding="utf-8"))


def gpu_unavailable_reason(scratch: Path) -> str | None:
    """The observer's own adapter report, never an empty render, decides that
    the machine has no GPU."""
    result = observe(scratch / "probe", "--backend", "gpu")
    if result.returncode == 0:
        return None
    error = receipt(scratch / "probe").get("error", "")
    if any(marker in error for marker in NO_ADAPTER_ERRORS):
        return error
    raise AssertionError(f"GPU render failed for a reason other than a missing adapter: "
                         f"{error!r}\n{result.stderr}")


class GpuBackendTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def render(self, name: str, *args: str) -> tuple[subprocess.CompletedProcess, Path]:
        out = self.tmp / name
        return observe(out, *args), out

    def test_gpu_render_is_recorded_as_gpu(self) -> None:
        result, out = self.render("gpu", "--backend", "gpu")
        self.assertEqual(result.returncode, 0, result.stderr)
        r = receipt(out)
        self.assertEqual((r["backend_requested"], r["backend_used"], r["ok"]), ("gpu", "gpu", True))
        gpu = r["gpu"]
        self.assertEqual(gpu["frame_outcome"], "offscreen")
        self.assertTrue(gpu["readback_completed"])
        self.assertFalse(gpu["device_lost"])
        self.assertEqual((gpu["pixel_width"], gpu["pixel_height"]), (480, 320))
        self.assertTrue(gpu["adapter"]["available"])
        self.assertFalse(gpu["adapter"]["null_backend"])
        self.assertRegex(r["dawn_version"], r"^[0-9a-f]{40}$")
        self.assertGreater(r["skia_milestone"], 0)

    def test_gpu_render_is_deterministic_and_differs_from_cpu(self) -> None:
        first, a = self.render("gpu-a", "--backend", "gpu")
        second, b = self.render("gpu-b", "--backend", "gpu")
        cpu, c = self.render("cpu", "--backend", "skia")
        for result in (first, second, cpu):
            self.assertEqual(result.returncode, 0, result.stderr)
        gpu_png = (a / "render.png").read_bytes()
        self.assertEqual(gpu_png, (b / "render.png").read_bytes())
        # The two rasterizers anti-alias differently, which is what lets a
        # parity lane tell a silent CPU fallback from a GPU render.
        self.assertNotEqual(gpu_png, (c / "render.png").read_bytes())
        r = receipt(c)
        self.assertEqual((r["backend_requested"], r["backend_used"]), ("skia", "skia"))
        self.assertNotIn("gpu", r)

    def test_device_destroyed_fails_the_render(self) -> None:
        result, out = self.render("lost", "--backend", "gpu", "--gpu-fault", "device-destroyed")
        self.assertEqual(result.returncode, 1)
        r = receipt(out)
        self.assertFalse(r["ok"])
        self.assertTrue(r["gpu"]["device_lost"])
        self.assertTrue(r["gpu"]["surface_created"])
        self.assertFalse((out / "render.png").exists())

    def test_no_frame_is_recorded(self) -> None:
        result, out = self.render("no-frame", "--backend", "gpu", "--gpu-fault", "no-frame")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(receipt(out)["gpu"]["frame_outcome"], "not-drawn")

    def test_raster_fallback_faults(self) -> None:
        cpu, c = self.render("cpu", "--backend", "skia")
        reported, r = self.render("reported", "--backend", "gpu", "--gpu-fault", "raster-fallback")
        silent, s = self.render("silent", "--backend", "gpu", "--gpu-fault",
                                "raster-fallback-unreported")
        for result in (cpu, reported, silent):
            self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(receipt(r)["backend_used"], "skia")
        # The unreported variant claims the GPU backend while rendering on the
        # CPU, which is exactly what the lane must catch from evidence other
        # than the receipt.
        self.assertEqual(receipt(s)["backend_used"], "gpu")
        cpu_png = (c / "render.png").read_bytes()
        self.assertEqual((r / "render.png").read_bytes(), cpu_png)
        self.assertEqual((s / "render.png").read_bytes(), cpu_png)

    def test_bad_arguments(self) -> None:
        for args, message in (
            (("--backend", "metal"), "--backend expects skia or gpu"),
            (("--backend", "gpu", "--gpu-fault", "melt"), "unknown --gpu-fault"),
            (("--gpu-fault", "no-frame"), "--gpu-fault requires --backend gpu"),
        ):
            with self.subTest(args=args):
                result, _ = self.render("bad-" + "-".join(a.strip("-") for a in args), *args)
                self.assertEqual(result.returncode, 2)
                self.assertIn(message, result.stderr)


def main() -> int:
    global OBSERVER
    parser = argparse.ArgumentParser()
    parser.add_argument("--observer", required=True, type=Path)
    args, _ = parser.parse_known_args()
    OBSERVER = args.observer
    if not OBSERVER.is_file():
        print(f"observer not built: {OBSERVER}", file=sys.stderr)
        return 1
    with tempfile.TemporaryDirectory() as scratch:
        reason = gpu_unavailable_reason(Path(scratch))
    if reason:
        print(f"SKIP: the observer reports no native GPU adapter: {reason}", file=sys.stderr)
        return SKIP
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(GpuBackendTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
