#!/usr/bin/env python3
"""Pure helper tests for tools/motion/visual/analyze_sequence.py."""

from __future__ import annotations

import json
import contextlib
import io
import sys
import tempfile
import types
import unittest
from pathlib import Path
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parent.parent
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tools.motion.visual import analyze_sequence as az


def _frame(index: int) -> az.FrameInfo:
    return az.FrameInfo(
        index=index,
        path=f"frames/frame_{index:04d}.png",
        width=160,
        height=90,
        mean_brightness=0.25 + index * 0.01,
    )


def _pair(
    from_index: int,
    to_index: int,
    *,
    ssim: float,
    mean: float = 0.02,
    max_diff: float = 0.15,
    diff: str | None = None,
) -> az.PairMetrics:
    return az.PairMetrics(
        from_index=from_index,
        to_index=to_index,
        ssim=ssim,
        pixel_diff_mean=mean,
        pixel_diff_max=max_diff,
        diff_png_path=diff,
    )


class FakeCrop:
    def __init__(self, values: list[int]) -> None:
        self.values = values

    def convert(self, mode: str) -> "FakeCrop":
        self.mode = mode
        return self

    def getdata(self) -> list[int]:
        return self.values


class FakeImage:
    def __init__(self, values: list[int]) -> None:
        self.size = (20, 12)
        self.values = values
        self.crop_boxes: list[tuple[int, int, int, int]] = []

    def crop(self, box: tuple[int, int, int, int]) -> FakeCrop:
        self.crop_boxes.append(box)
        return FakeCrop(self.values)


class FakeArray:
    def __init__(self, brightness: float) -> None:
        self.brightness = brightness

    def mean(self) -> float:
        return self.brightness

    def astype(self, dtype: str) -> "FakeArray":
        self.dtype = dtype
        return self


class FakeVector:
    def __init__(self, values: list[float]) -> None:
        self.values = values

    def __sub__(self, other: "FakeVector") -> "FakeVector":
        return FakeVector([a - b for a, b in zip(self.values, other.values)])

    def __mul__(self, factor: float) -> "FakeVector":
        return FakeVector([value * factor for value in self.values])

    def mean(self) -> float:
        return sum(self.values) / len(self.values)

    def max(self) -> float:
        return max(self.values)

    def astype(self, dtype: str) -> tuple[str, tuple[float, ...]]:
        return dtype, tuple(self.values)


class FakeFrameArray:
    def __init__(self, luma: list[float]) -> None:
        self.luma = luma

    def __matmul__(self, weights: object) -> FakeVector:
        return FakeVector(self.luma)


class FakeLumaMatrix:
    shape = (3, 3)

    def __init__(self, values: list[float]) -> None:
        self.values = values

    def mean(self) -> float:
        return sum(self.values) / len(self.values)

    def astype(self, dtype: str) -> tuple[str, tuple[float, ...]]:
        return dtype, tuple(self.values)


class FakeAffineFrameArray:
    def __init__(self, values: list[float]) -> None:
        self.values = values

    def __matmul__(self, weights: object) -> FakeLumaMatrix:
        return FakeLumaMatrix(self.values)


class FakePoints:
    def __init__(self, points: list[tuple[float, float]]) -> None:
        self.points = points
        self.shape: tuple[int, int, int] | None = None

    def reshape(self, *shape: int) -> "FakePoints":
        self.shape = shape
        return self


class FakeAffineMatrix:
    values = {
        (0, 0): 0.0,
        (0, 2): 3.5,
        (1, 0): 4.0,
        (1, 2): -2.25,
    }

    def __getitem__(self, key: tuple[int, int]) -> float:
        return self.values[key]


class FakeAffineNumpy:
    float32 = "float32"

    @staticmethod
    def array(values: list[float], dtype: str) -> tuple[str, tuple[float, ...]]:
        return dtype, tuple(values)

    @staticmethod
    def float32(values: list[tuple[float, float]]) -> FakePoints:  # type: ignore[assignment]
        return FakePoints(values)

    @staticmethod
    def arctan2(y: float, x: float) -> tuple[str, float, float]:
        return "atan2", y, x

    @staticmethod
    def degrees(value: tuple[str, float, float]) -> float:
        return 90.0


class FakeNumpy:
    float32 = "float32"
    uint8 = "uint8"

    @staticmethod
    def array(values: list[float], dtype: str) -> tuple[str, tuple[float, ...]]:
        return dtype, tuple(values)

    @staticmethod
    def abs(vector: FakeVector) -> FakeVector:
        return FakeVector([abs(value) for value in vector.values])

    @staticmethod
    def clip(vector: FakeVector, low: float, high: float) -> FakeVector:
        return FakeVector([min(high, max(low, value)) for value in vector.values])


class FakeOpenImage:
    size = (13, 7)

    def convert(self, mode: str) -> "FakeOpenImage":
        self.mode = mode
        return self


class FakeOpenImageModule:
    opened: list[str] = []

    @classmethod
    def open(cls, path: str) -> FakeOpenImage:
        cls.opened.append(path)
        return FakeOpenImage()


class FakeAsArray:
    @staticmethod
    def asarray(image: FakeOpenImage) -> tuple[str, str]:
        return "array", image.mode


class FakeSavedImage:
    def __init__(self, size: tuple[int, int] = (4, 3)) -> None:
        self.size = size
        self.saved: list[tuple[str, str | None]] = []
        self.pastes: list[tuple[object, tuple[int, int]]] = []

    def convert(self, mode: str) -> "FakeSavedImage":
        self.mode = mode
        return self

    def save(self, path: str, format: str | None = None) -> None:
        self.saved.append((path, format))

    def paste(self, image: object, xy: tuple[int, int]) -> None:
        self.pastes.append((image, xy))


class FakeImageModule:
    fromarray_calls: list[tuple[object, str]] = []
    opened: list[str] = []
    sprites: list[FakeSavedImage] = []

    @classmethod
    def fromarray(cls, array: object, mode: str) -> FakeSavedImage:
        cls.fromarray_calls.append((array, mode))
        return FakeSavedImage()

    @classmethod
    def open(cls, path: str) -> FakeSavedImage:
        cls.opened.append(path)
        return FakeSavedImage(size=(5, 4))

    @classmethod
    def new(cls, mode: str, size: tuple[int, int], color: tuple[int, int, int]) -> FakeSavedImage:
        sprite = FakeSavedImage(size=size)
        sprite.mode = mode
        sprite.color = color
        cls.sprites.append(sprite)
        return sprite


class FakeGridCrop:
    def __init__(self, values: list[int]) -> None:
        self.values = values
        self.mode = ""

    def convert(self, mode: str) -> "FakeGridCrop":
        self.mode = mode
        return self

    def getdata(self) -> list[int]:
        return self.values


class FakeGridImage:
    saved: list[tuple[str, str | None]] = []

    def __init__(
        self,
        mode: str = "RGB",
        size: tuple[int, int] = (12, 8),
        color: tuple[int, ...] = (255, 255, 255),
        values: list[int] | None = None,
    ) -> None:
        self.mode = mode
        self.size = size
        self.color = color
        self.values = values if values is not None else [240, 245, 250]
        self.crop_boxes: list[tuple[int, int, int, int]] = []

    def crop(self, box: tuple[int, int, int, int]) -> FakeGridCrop:
        self.crop_boxes.append(box)
        return FakeGridCrop(self.values)

    def convert(self, mode: str) -> "FakeGridImage":
        self.mode = mode
        return self

    def save(self, path: str, format: str | None = None) -> None:
        self.saved.append((path, format))


class FakeGridImageModule:
    opened: list[str] = []
    new_calls: list[tuple[str, tuple[int, int], tuple[int, ...]]] = []
    composites: list[tuple[FakeGridImage, FakeGridImage]] = []

    @classmethod
    def open(cls, path: str) -> FakeGridImage:
        cls.opened.append(path)
        return FakeGridImage()

    @classmethod
    def new(
        cls,
        mode: str,
        size: tuple[int, int],
        color: tuple[int, ...],
    ) -> FakeGridImage:
        cls.new_calls.append((mode, size, color))
        return FakeGridImage(mode=mode, size=size, color=color)

    @classmethod
    def alpha_composite(
        cls, base: FakeGridImage, overlay: FakeGridImage
    ) -> FakeGridImage:
        cls.composites.append((base, overlay))
        return FakeGridImage(size=base.size)


class FakeGridDraw:
    instances: list["FakeGridDraw"] = []

    def __init__(self, image: FakeGridImage) -> None:
        self.image = image
        self.lines: list[tuple[list[tuple[int, int]], tuple[int, ...], int]] = []
        self.rectangles: list[tuple[tuple[int, int, int, int], tuple[int, ...]]] = []
        self.texts: list[tuple[tuple[int, int], str, tuple[int, ...], object]] = []
        self.textsize_calls: list[tuple[str, object]] = []
        self.textbbox_calls: list[tuple[tuple[int, int], str, object]] = []
        FakeGridDraw.instances.append(self)

    def line(
        self,
        points: list[tuple[int, int]],
        *,
        fill: tuple[int, ...],
        width: int,
    ) -> None:
        self.lines.append((points, fill, width))

    def textbbox(self, xy: tuple[int, int], text: str, *, font: object) -> tuple[int, int, int, int]:
        self.textbbox_calls.append((xy, text, font))
        raise RuntimeError("old pillow")

    def textsize(self, text: str, *, font: object) -> tuple[int, int]:
        self.textsize_calls.append((text, font))
        return (len(text) + 2, 3)

    def rectangle(
        self, box: tuple[int, int, int, int], *, fill: tuple[int, ...]
    ) -> None:
        self.rectangles.append((box, fill))

    def text(
        self,
        xy: tuple[int, int],
        text: str,
        *,
        fill: tuple[int, ...],
        font: object,
    ) -> None:
        self.texts.append((xy, text, fill, font))


class FakeImageDrawModule:
    @staticmethod
    def Draw(image: FakeGridImage) -> FakeGridDraw:
        return FakeGridDraw(image)


class MotionVisualAnalyzeSequenceTests(unittest.TestCase):
    def test_dependency_import_success_and_failure_paths_are_explicit(self) -> None:
        real_import = __import__
        fake_np = object()
        fake_image = object()
        fake_ssim = object()

        def fake_success_import(name: str, *args: object, **kwargs: object) -> object:
            if name == "numpy":
                return fake_np
            if name == "PIL":
                return types.SimpleNamespace(Image=fake_image)
            if name == "skimage.metrics":
                return types.SimpleNamespace(structural_similarity=fake_ssim)
            return real_import(name, *args, **kwargs)

        with mock.patch("builtins.__import__", side_effect=fake_success_import):
            self.assertEqual(az._try_import_deps(), (fake_np, fake_image, fake_ssim))

        def fake_import(name: str, *args: object, **kwargs: object) -> object:
            if name == "numpy":
                raise ImportError("numpy unavailable")
            return real_import(name, *args, **kwargs)

        stderr = io.StringIO()
        with (
            mock.patch("builtins.__import__", side_effect=fake_import),
            contextlib.redirect_stderr(stderr),
        ):
            self.assertIsNone(az._try_import_deps())

        self.assertIn("missing dependency", stderr.getvalue())
        self.assertIn("requirements.txt", stderr.getvalue())

    def test_grid_font_loader_uses_first_available_truetype_font(self) -> None:
        real_import = __import__

        class FakeFontModule:
            calls: list[tuple[str, int]] = []

            @classmethod
            def truetype(cls, path: str, *, size: int) -> tuple[str, str, int]:
                cls.calls.append((path, size))
                return "font", path, size

            @staticmethod
            def load_default() -> str:
                return "default"

        def fake_import(name: str, *args: object, **kwargs: object) -> object:
            if name == "PIL":
                return types.SimpleNamespace(ImageFont=FakeFontModule)
            return real_import(name, *args, **kwargs)

        with mock.patch("builtins.__import__", side_effect=fake_import):
            font = az._grid_load_font(12)

        self.assertEqual(font[0], "font")
        self.assertEqual(font[2], 12)
        self.assertEqual(len(FakeFontModule.calls), 1)

    def test_grid_font_loader_falls_back_when_fonts_are_unavailable(self) -> None:
        real_import = __import__

        class FakeFontModule:
            calls: list[tuple[str, int]] = []

            @classmethod
            def truetype(cls, path: str, *, size: int) -> object:
                cls.calls.append((path, size))
                raise OSError("missing font")

            @staticmethod
            def load_default() -> str:
                return "default-font"

        def fake_import(name: str, *args: object, **kwargs: object) -> object:
            if name == "PIL":
                return types.SimpleNamespace(ImageFont=FakeFontModule)
            return real_import(name, *args, **kwargs)

        with mock.patch("builtins.__import__", side_effect=fake_import):
            font = az._grid_load_font(14)

        self.assertEqual(font, "default-font")
        self.assertEqual(len(FakeFontModule.calls), 5)
        self.assertEqual(FakeFontModule.calls[0][1], 14)
        self.assertIn("Arial.ttf", FakeFontModule.calls[0][0])
        self.assertIn("arial.ttf", FakeFontModule.calls[-1][0])

    def test_load_frame_array_and_pair_metrics_use_dependency_protocols(self) -> None:
        FakeOpenImageModule.opened.clear()
        arr, size = az.load_frame_array(Path("frame.png"), FakeAsArray, FakeOpenImageModule)

        self.assertEqual(FakeOpenImageModule.opened, ["frame.png"])
        self.assertEqual(arr, ("array", "RGB"))
        self.assertEqual(size, (13, 7))

        def fake_ssim(from_luma: FakeVector, to_luma: FakeVector, *, data_range: float) -> float:
            self.assertEqual(from_luma.values, [10.0, 20.0])
            self.assertEqual(to_luma.values, [20.0, 50.0])
            self.assertEqual(data_range, 255.0)
            return 0.625

        ssim, mean_diff, max_diff, diff = az.compute_pair_metrics(
            FakeFrameArray([10.0, 20.0]),
            FakeFrameArray([20.0, 50.0]),
            FakeNumpy,
            fake_ssim,
        )

        self.assertEqual(ssim, 0.625)
        self.assertAlmostEqual(mean_diff, 20.0 / 255.0)
        self.assertAlmostEqual(max_diff, 30.0 / 255.0)
        self.assertEqual(diff, ("uint8", (30.0, 90.0)))

    def test_list_frames_and_row_labels_are_deterministic(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            for name in ("frame_0002.png", "notes.txt", "frame_0001.png"):
                (root / name).write_text("x", encoding="utf-8")

            self.assertEqual(
                [path.name for path in az.list_frames(root, "frame_*.png")],
                ["frame_0001.png", "frame_0002.png"],
            )

        self.assertEqual(az._row_label(-1), "?")
        self.assertEqual(az._row_label(0), "A")
        self.assertEqual(az._row_label(25), "Z")
        self.assertEqual(az._row_label(26), "AA")
        self.assertEqual(az._row_label(51), "AZ")
        self.assertEqual(az._row_label(52), "BA")

    def test_grid_theme_auto_samples_corners_and_honors_explicit_modes(self) -> None:
        bright = FakeImage([240, 250, 245])
        dark = FakeImage([4, 10, 20])
        empty = FakeImage([])

        self.assertEqual(az._grid_pick_theme(bright, 2, 3, "auto"), "light")
        self.assertEqual(len(bright.crop_boxes), 4)
        self.assertEqual(az._grid_pick_theme(dark, 2, 3, "auto"), "dark")
        self.assertEqual(az._grid_pick_theme(empty, 2, 3, "auto"), "dark")
        self.assertEqual(az._grid_pick_theme(bright, 2, 3, "dark"), "dark")
        self.assertEqual(az._grid_pick_theme(dark, 2, 3, "light"), "light")

    def test_render_grid_overlay_draws_lines_labels_and_uses_textsize_fallback(self) -> None:
        FakeGridImage.saved.clear()
        FakeGridImageModule.opened.clear()
        FakeGridImageModule.new_calls.clear()
        FakeGridImageModule.composites.clear()
        FakeGridDraw.instances.clear()
        real_import = __import__

        class FakeFontModule:
            @staticmethod
            def truetype(path: str, *, size: int) -> tuple[str, int]:
                return "font", size

            @staticmethod
            def load_default() -> str:
                return "default"

        def fake_import(name: str, *args: object, **kwargs: object) -> object:
            if name == "PIL":
                return types.SimpleNamespace(
                    ImageDraw=FakeImageDrawModule,
                    ImageFont=FakeFontModule,
                )
            return real_import(name, *args, **kwargs)

        with mock.patch("builtins.__import__", side_effect=fake_import):
            az.render_grid_overlay(
                Path("source.png"),
                Path("grid.png"),
                FakeGridImageModule,
                rows=40,
                cols=0,
                theme="auto",
            )

        draw = FakeGridDraw.instances[0]
        self.assertEqual(FakeGridImageModule.opened, ["source.png"])
        self.assertEqual(FakeGridImageModule.new_calls, [("RGBA", (12, 8), (0, 0, 0, 0))])
        self.assertEqual(FakeGridImageModule.new_calls[0][0], "RGBA")
        self.assertEqual(FakeGridImageModule.new_calls[0][1], (12, 8))
        self.assertEqual(FakeGridImageModule.new_calls[0][2], (0, 0, 0, 0))
        self.assertEqual(len(FakeGridImageModule.composites), 1)
        self.assertEqual(FakeGridImageModule.composites[0][0].mode, "RGBA")
        self.assertEqual(FakeGridImageModule.composites[0][1].mode, "RGBA")
        self.assertEqual(len(draw.lines), 25)
        self.assertEqual(draw.lines[0][0], [(0, 0), (11, 0)])
        self.assertEqual(draw.lines[0][1], (0, 0, 0, 160))
        self.assertEqual(draw.lines[0][2], 1)
        self.assertEqual(draw.lines[-1][0], [(0, 8), (11, 8)])
        self.assertEqual(len(draw.textbbox_calls), 26)
        self.assertEqual(len(draw.textsize_calls), 26)
        self.assertEqual(len(draw.rectangles), 26)
        self.assertEqual(len(draw.texts), 26)
        self.assertEqual(draw.textbbox_calls[0][1], "A1")
        self.assertEqual(draw.textsize_calls[0][0], "A1")
        self.assertEqual(draw.rectangles[0][1], (255, 255, 255, 180))
        self.assertEqual(draw.texts[0][1], "A1")
        self.assertEqual(draw.texts[-1][1], "Z1")
        self.assertEqual(draw.texts[0][2], (0, 0, 0, 255))
        self.assertEqual(FakeGridImage.saved, [("grid.png", "PNG")])

    def test_detect_motion_window_handles_empty_idle_and_active_ranges(self) -> None:
        self.assertEqual(az.detect_motion_window([], threshold=0.01), (0, 0))
        self.assertEqual(
            az.detect_motion_window(
                [
                    _pair(0, 1, ssim=1.0, mean=0.0),
                    _pair(1, 2, ssim=1.0, mean=0.001),
                ],
                threshold=0.01,
            ),
            (0, 0),
        )
        self.assertEqual(
            az.detect_motion_window(
                [
                    _pair(0, 1, ssim=1.0, mean=0.0),
                    _pair(1, 2, ssim=0.8, mean=0.2),
                    _pair(2, 3, ssim=0.7, mean=0.15),
                    _pair(3, 4, ssim=1.0, mean=0.0),
                ],
                threshold=0.01,
            ),
            (1, 1),
        )

    def test_select_keyframes_dedupes_roles_and_sorts_by_frame_index(self) -> None:
        frames = [_frame(i) for i in range(6)]
        pairs = [
            _pair(0, 1, ssim=0.30),
            _pair(1, 2, ssim=0.90),
            _pair(2, 3, ssim=0.10),
            _pair(3, 4, ssim=0.80),
            _pair(4, 5, ssim=0.20),
        ]

        selected = az.select_keyframes(frames, pairs, top_delta=3)

        self.assertEqual([k.index for k in selected], [0, 1, 3, 5])
        self.assertEqual([k.role for k in selected], ["first", "top-delta", "mid", "last"])
        self.assertEqual(selected[0].path, "frames/frame_0000.png")
        self.assertEqual(selected[-1].path, "frames/frame_0005.png")
        self.assertEqual(len({k.index for k in selected}), len(selected))
        self.assertEqual(az.select_keyframes([], pairs, top_delta=2), [])
        self.assertEqual(
            [k.role for k in az.select_keyframes(frames[:2], pairs[:1], top_delta=0)],
            ["first", "last"],
        )

    def test_compute_confidence_covers_bands_neighbour_penalty_and_affine_signal(self) -> None:
        for ssim, expected in ((0.995, 0.85), (0.75, 0.95), (0.45, 0.70), (0.10, 0.45)):
            with self.subTest(ssim=ssim):
                pair = _pair(0, 1, ssim=ssim)
                az.compute_confidence([pair])
                self.assertAlmostEqual(pair.confidence, expected)

        neighbours = [
            _pair(0, 1, ssim=0.95),
            _pair(1, 2, ssim=0.20),
            _pair(2, 3, ssim=0.95),
        ]
        az.compute_confidence(neighbours)
        self.assertAlmostEqual(neighbours[0].confidence, 0.80)
        self.assertAlmostEqual(neighbours[1].confidence, 0.30)
        self.assertAlmostEqual(neighbours[2].confidence, 0.80)

        good = _pair(0, 1, ssim=0.75, mean=10.0 / 255.0)
        bad = _pair(0, 1, ssim=0.75, mean=0.0)
        affine = {"translation": {"dx": 10.0, "dy": 0.0}}
        az.compute_confidence([good], affine_block=affine)
        az.compute_confidence([bad], affine_block=affine)
        self.assertAlmostEqual(good.confidence, 0.98)
        self.assertAlmostEqual(bad.confidence, 0.90)

    def test_estimate_affine_uses_opencv_matches_when_available(self) -> None:
        real_import = __import__

        keypoints = [
            types.SimpleNamespace(pt=(1.0, 2.0)),
            types.SimpleNamespace(pt=(3.0, 4.0)),
            types.SimpleNamespace(pt=(5.0, 6.0)),
        ]
        matches = [
            types.SimpleNamespace(distance=3, queryIdx=2, trainIdx=0),
            types.SimpleNamespace(distance=1, queryIdx=0, trainIdx=1),
            types.SimpleNamespace(distance=2, queryIdx=1, trainIdx=2),
        ]

        class FakeOrb:
            calls: list[tuple[object, None]] = []

            def detectAndCompute(self, gray: object, mask: None) -> tuple[list[object], object]:
                self.calls.append((gray, mask))
                return keypoints, object()

        class FakeMatcher:
            def __init__(self, norm: int, *, crossCheck: bool) -> None:
                self.norm = norm
                self.cross_check = crossCheck

            def match(self, des1: object, des2: object) -> list[object]:
                self.descriptors = (des1, des2)
                return matches

        fake_orb = FakeOrb()
        fake_cv2 = types.SimpleNamespace(
            NORM_HAMMING=7,
            ORB_create=lambda *, nfeatures: fake_orb,
            BFMatcher=FakeMatcher,
            estimateAffinePartial2D=lambda src, dst: (FakeAffineMatrix(), "inliers"),
        )

        def fake_import(name: str, *args: object, **kwargs: object) -> object:
            if name == "cv2":
                return fake_cv2
            return real_import(name, *args, **kwargs)

        with mock.patch("builtins.__import__", side_effect=fake_import):
            affine = az.estimate_affine_first_to_last(
                FakeAffineFrameArray([10.0, 20.0, 30.0]),
                FakeAffineFrameArray([15.0, 25.0, 35.0]),
                FakeAffineNumpy,
                object(),
            )

        self.assertEqual(affine["translation"], {"dx": 3.5, "dy": -2.25})
        self.assertEqual(affine["rotation_deg"], 90.0)
        self.assertEqual(affine["scale_ratio"], 4.0)
        self.assertAlmostEqual(affine["opacity_delta"], 5.0 / 255.0)
        self.assertEqual(affine["method"], "opencv")
        self.assertEqual(len(fake_orb.calls), 2)
        self.assertEqual(fake_orb.calls[0][0][0], "uint8")
        self.assertEqual(fake_orb.calls[1][0][0], "uint8")

    def test_image_helpers_use_image_module_contracts_without_real_pillow(self) -> None:
        FakeImageModule.fromarray_calls.clear()
        FakeImageModule.opened.clear()
        FakeImageModule.sprites.clear()

        az.write_diff_heatmap("diff-array", Path("diff.png"), object(), FakeImageModule)
        self.assertEqual(FakeImageModule.fromarray_calls, [("diff-array", "L")])

        az.make_keyframe_sprite(
            [Path("frame_a.png"), Path("frame_b.png")],
            Path("sprite.png"),
            FakeImageModule,
        )
        self.assertEqual(FakeImageModule.opened, ["frame_a.png", "frame_b.png"])
        self.assertEqual(FakeImageModule.sprites[0].size, (10, 4))
        self.assertEqual(FakeImageModule.sprites[0].pastes[0][1], (0, 0))
        self.assertEqual(FakeImageModule.sprites[0].pastes[1][1], (5, 0))

        az.make_keyframe_sprite([], Path("unused.png"), FakeImageModule)
        empty_pairs: list[az.PairMetrics] = []
        az.compute_confidence(empty_pairs)
        self.assertEqual(empty_pairs, [])

    def test_analyze_orchestrates_trim_grid_affine_and_diff_selection(self) -> None:
        arrays = [FakeArray(10.0), FakeArray(20.0), FakeArray(30.0), FakeArray(40.0)]
        pair_metrics = iter(
            [
                (0.99, 0.0, 0.0, "diff-01"),
                (0.50, 0.2, 0.4, "diff-12"),
                (0.99, 0.0, 0.0, "diff-23"),
                (0.50, 0.2, 0.4, "diff-12-repeat"),
            ]
        )
        frames = [Path(f"frame_{i:04d}.png") for i in range(4)]
        affine = {
            "translation": {"dx": 4.0, "dy": 0.0},
            "rotation_deg": 0.0,
            "scale_ratio": 1.0,
            "opacity_delta": 0.0,
            "method": "pil-fallback",
        }

        with tempfile.TemporaryDirectory() as td:
            output = Path(td) / "report"
            with (
                mock.patch.object(az, "_try_import_deps", return_value=(object(), object(), object())),
                mock.patch.object(az, "list_frames", return_value=frames),
                mock.patch.object(
                    az,
                    "load_frame_array",
                    side_effect=[(array, (20, 10)) for array in arrays],
                ),
                mock.patch.object(az, "compute_pair_metrics", side_effect=lambda *args: next(pair_metrics)),
                mock.patch.object(az, "write_diff_heatmap") as write_diff,
                mock.patch.object(az, "make_keyframe_sprite") as make_sprite,
                mock.patch.object(az, "render_grid_overlay") as render_grid,
                mock.patch.object(az, "estimate_affine_first_to_last", return_value=affine) as estimate_affine,
            ):
                rc = az.analyze(
                    frames_dir=Path("frames"),
                    output_dir=output,
                    keyframes_top=1,
                    max_diff_frames=1,
                    grid=True,
                    grid_rows=2,
                    grid_cols=4,
                    grid_theme="dark",
                    trim=True,
                    trim_threshold=0.01,
                    affine=True,
                )

            data = json.loads((output / "analysis.json").read_text(encoding="utf-8"))

        self.assertEqual(rc, 0)
        self.assertEqual([frame["index"] for frame in data["frames"]], [1, 2])
        self.assertEqual(data["summary"]["trimmed_leading_frames"], 1)
        self.assertEqual(data["summary"]["trimmed_trailing_frames"], 1)
        self.assertEqual(data["affine_first_to_last"]["method"], "pil-fallback")
        self.assertEqual(data["pairs"][0]["diff_png_path"], "diff/diff_0001_0002.png")
        self.assertEqual(write_diff.call_count, 1)
        self.assertEqual(make_sprite.call_count, 1)
        self.assertEqual(render_grid.call_count, 4)
        self.assertEqual(estimate_affine.call_count, 1)

    def test_analyze_single_frame_reports_default_pair_summary(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            output = Path(td) / "report"
            with (
                mock.patch.object(az, "_try_import_deps", return_value=(object(), object(), object())),
                mock.patch.object(az, "list_frames", return_value=[Path("frame_0000.png")]),
                mock.patch.object(az, "load_frame_array", return_value=(FakeArray(64.0), (20, 10))),
                mock.patch.object(az, "make_keyframe_sprite"),
            ):
                rc = az.analyze(
                    frames_dir=Path("frames"),
                    output_dir=output,
                    max_diff_frames=0,
                    grid=False,
                    trim=False,
                    affine=False,
                )
            data = json.loads((output / "analysis.json").read_text(encoding="utf-8"))

        self.assertEqual(rc, 0)
        self.assertEqual(len(data["frames"]), 1)
        self.assertEqual(data["pairs"], [])
        self.assertEqual(data["summary"]["mean_ssim"], 1.0)
        self.assertEqual(data["summary"]["max_pixel_diff"], 0.0)

    def test_analyze_returns_dependency_and_empty_frame_statuses(self) -> None:
        with mock.patch.object(az, "_try_import_deps", return_value=None):
            self.assertEqual(az.analyze(Path("frames"), Path("out")), 3)

        stderr = io.StringIO()
        with (
            mock.patch.object(az, "_try_import_deps", return_value=(object(), object(), object())),
            mock.patch.object(az, "list_frames", return_value=[]),
            contextlib.redirect_stderr(stderr),
        ):
            rc = az.analyze(Path("frames"), Path("out"))

        self.assertEqual(rc, 2)
        self.assertIn("no frames matched", stderr.getvalue())

    def test_write_json_and_summary_include_claim_evidence_fields(self) -> None:
        report = az.Report(
            schema_version=az.REPORT_SCHEMA_VERSION,
            frames=[_frame(0), _frame(1)],
            pairs=[_pair(0, 1, ssim=0.25, mean=0.3, diff="diff/diff_0000_0001.png")],
            keyframes=[
                az.KeyframeInfo(index=0, role="first", path="frames/frame_0000.png"),
                az.KeyframeInfo(index=1, role="last", path="frames/frame_0001.png"),
            ],
            sprite_path="keyframes.png",
            summary={
                "mean_ssim": 0.25,
                "min_ssim": 0.25,
                "mean_pixel_diff": 0.3,
                "max_pixel_diff": 0.5,
                "mean_confidence": 0.5,
                "min_confidence": 0.5,
            },
            affine_first_to_last={
                "translation": {"dx": 2.0, "dy": -1.0},
                "rotation_deg": 0.0,
                "scale_ratio": 1.0,
                "opacity_delta": 0.1,
                "method": "pil-fallback",
            },
        )
        report.pairs[0].confidence = 0.5

        with tempfile.TemporaryDirectory() as td:
            output = Path(td)
            az.write_json(report, output)
            az.write_summary_md(report, output)

            data = json.loads((output / "analysis.json").read_text(encoding="utf-8"))
            summary = (output / "summary.md").read_text(encoding="utf-8")

        self.assertEqual(data["schema_version"], az.REPORT_SCHEMA_VERSION)
        self.assertEqual(data["frames"][1]["index"], 1)
        self.assertEqual(data["pairs"][0]["confidence"], 0.5)
        self.assertEqual(data["affine_first_to_last"]["method"], "pil-fallback")
        self.assertIn("Claim-evidence contract", summary)
        self.assertIn("Low-confidence pairs", summary)
        self.assertIn("diff/diff_0000_0001.png", summary)
        self.assertIn("dx=2.0px", summary)

    def test_main_forwards_cli_options_to_analyze(self) -> None:
        with mock.patch.object(az, "analyze", return_value=17) as analyze:
            rc = az.main(
                [
                    "--frames-dir",
                    "frames",
                    "--output",
                    "report",
                    "--pattern",
                    "shot_*.png",
                    "--keyframes",
                    "4",
                    "--max-diff-frames",
                    "0",
                    "--grid",
                    "--grid-rows",
                    "3",
                    "--grid-cols",
                    "5",
                    "--grid-theme",
                    "dark",
                    "--trim",
                    "--trim-threshold",
                    "0.02",
                    "--affine",
                ]
            )

        self.assertEqual(rc, 17)
        kwargs = analyze.call_args.kwargs
        self.assertEqual(kwargs["frames_dir"], Path("frames"))
        self.assertEqual(kwargs["output_dir"], Path("report"))
        self.assertEqual(kwargs["pattern"], "shot_*.png")
        self.assertEqual(kwargs["keyframes_top"], 4)
        self.assertEqual(kwargs["max_diff_frames"], 0)
        self.assertTrue(kwargs["grid"])
        self.assertEqual(kwargs["grid_rows"], 3)
        self.assertEqual(kwargs["grid_cols"], 5)
        self.assertEqual(kwargs["grid_theme"], "dark")
        self.assertTrue(kwargs["trim"])
        self.assertEqual(kwargs["trim_threshold"], 0.02)
        self.assertTrue(kwargs["affine"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
