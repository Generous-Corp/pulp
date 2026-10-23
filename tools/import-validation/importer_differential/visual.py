"""The visual lens: full-frame and authored-frame pixel similarity.

The full frame is scored as rendered, and the authenticated authored frame is
cropped from both renders and scored on its own. A full-frame residual is only
excused when a shift control proves it is exactly the panel translation.
"""

from __future__ import annotations

import math
from collections import Counter
from pathlib import Path
from typing import Any

from .model import PROMOTION_THRESHOLDS, LabError


def score_images(ref_image: Any, candidate_image: Any, output_dir: Path,
                 score_registration: dict[str, Any]) -> dict[str, Any]:
    """Score two same-size RGBA images and write their comparison overlays.

    The score is the lower of the whole-image similarity and the similarity
    over foreground pixels (pixels away from either image's own dominant
    colour), so a small defect cannot hide inside a large flat background.
    """
    from PIL import Image, ImageChops

    logical_size = candidate_image.size
    ref_pixels = list(ref_image.get_flattened_data())
    candidate_pixels = list(candidate_image.get_flattened_data())
    total_distance = 0.0
    alpha_distance = 0.0
    foreground_distance = 0.0
    foreground_samples = 0
    differing = 0
    threshold = 2
    reference_background = Counter(ref_pixels).most_common(1)[0][0]
    candidate_background = Counter(candidate_pixels).most_common(1)[0][0]
    for ref_pixel, candidate_pixel in zip(ref_pixels, candidate_pixels):
        delta = sum(abs(a - b) for a, b in zip(ref_pixel[:3], candidate_pixel[:3]))
        alpha_delta = abs(ref_pixel[3] - candidate_pixel[3])
        alpha_distance += alpha_delta
        total_distance += delta
        if delta > threshold or alpha_delta > threshold:
            differing += 1
        ref_foreground = sum(
            abs(a - b) for a, b in zip(ref_pixel[:3], reference_background[:3])) > 18
        candidate_foreground = sum(
            abs(a - b) for a, b in zip(
                candidate_pixel[:3], candidate_background[:3])) > 18
        if ref_foreground or candidate_foreground:
            foreground_distance += delta
            foreground_samples += 1
    samples = max(1, len(ref_pixels))
    mean_channel_error = total_distance / (samples * 3.0)
    mean_alpha_error = alpha_distance / samples
    combined_mean_error = (total_distance + alpha_distance) / (samples * 4.0)
    global_similarity = max(0.0, 1.0 - combined_mean_error / 255.0)
    foreground_mean_error = (
        foreground_distance / (foreground_samples * 3.0)
        if foreground_samples else mean_channel_error)
    foreground_similarity = max(0.0, 1.0 - foreground_mean_error / 255.0)
    similarity = min(global_similarity, foreground_similarity)
    output_dir.mkdir(parents=True, exist_ok=True)
    side_by_side = Image.new("RGBA", (logical_size[0] * 2, logical_size[1]))
    side_by_side.paste(ref_image, (0, 0))
    side_by_side.paste(candidate_image, (logical_size[0], 0))
    side_by_side.save(output_dir / "side-by-side.png")
    diff = ImageChops.difference(ref_image, candidate_image).convert("RGB")
    diff.point(lambda value: min(255, value * 4)).save(output_dir / "visual-diff.png")
    Image.blend(ref_image, candidate_image, 0.5).save(output_dir / "overlay.png")
    return {
        "score": round(similarity, 6),
        "global_score": round(global_similarity, 6),
        "foreground_score": round(foreground_similarity, 6),
        "mean_channel_error": round(mean_channel_error, 6),
        "mean_alpha_error": round(mean_alpha_error, 6),
        "combined_mean_channel_error": round(combined_mean_error, 6),
        "foreground_mean_channel_error": round(foreground_mean_error, 6),
        "foreground_pixel_count": foreground_samples,
        "differing_pixel_count": differing,
        "differing_pixel_fraction": round(differing / samples, 6),
        "width": logical_size[0],
        "height": logical_size[1],
        "registration": score_registration,
    }


def outside_frame_ink(image: Any, own_frame: tuple[int, int, int, int]) -> dict[str, Any]:
    """How much of an image outside its authored frame departs from its background."""
    background = Counter(image.get_flattened_data()).most_common(1)[0][0]
    total = 0
    differing = 0
    distance = 0.0
    for y in range(image.height):
        for x in range(image.width):
            if (own_frame[0] <= x < own_frame[2]
                    and own_frame[1] <= y < own_frame[3]):
                continue
            total += 1
            pixel = image.getpixel((x, y))
            delta = sum(abs(a - b) for a, b in zip(pixel[:3], background[:3]))
            alpha_delta = abs(pixel[3] - background[3])
            distance += delta + alpha_delta
            if delta > 2 or alpha_delta > 2:
                differing += 1
    return {
        "pixel_count": total,
        "differing_pixel_count": differing,
        "differing_pixel_fraction": round(differing / max(1, total), 6),
        "mean_channel_alpha_error": round(distance / max(1, total * 4.0), 6),
        "background": list(background),
        "frame_px": list(own_frame),
    }


def classify_full_frame(
    full: dict[str, Any], reference: Any, candidate: Any,
    registration: dict[str, Any], artifacts_dir: Path, registered: bool,
    page_background_status: str | None = None,
) -> dict[str, Any]:
    """Say whether a full-frame residual is exactly the panel translation.

    Shift control, for a native render that draws the panel somewhere other
    than Chromium's authored origin (an unplaced panel-local render puts it at
    the native origin): translate the native render by the difference of the
    two crop boxes and fill the vacated margin with the native render's own
    outside-frame background. If the full-frame residual is only that
    translation, the shifted render reproduces the browser PNG and no
    outside-frame pixel differs. A render the observer already placed at the
    authored origin has no translation left to explain a residual.

    When the lab could not resolve the page colour Chromium paints
    (`page_background_status == "unresolved"`), the observer leaves the page
    around the panel unpainted, so a residual outside the frame is named
    `page-background-unresolved` instead of `unexplained`. Both block
    promotion; only `panel-translation` excuses a full-frame residual.
    """
    from PIL import Image, ImageChops

    threshold = PROMOTION_THRESHOLDS["visual"]
    if full["score"] >= threshold:
        return {"classification": "full-frame-pass", "threshold": threshold}
    if not registered:
        return {"classification": "unexplained", "threshold": threshold,
                "reason": "no authored frame to test a panel translation"}
    native_box = tuple(registration["native_crop_px"])
    offset = (registration["browser_crop_px"][0] - native_box[0],
              registration["browser_crop_px"][1] - native_box[1])
    outside_native = [
        pixel for index, pixel in enumerate(candidate.get_flattened_data())
        if not (native_box[0] <= index % candidate.width < native_box[2]
                and native_box[1] <= index // candidate.width < native_box[3])]
    if offset == (0, 0) or not outside_native:
        frame_box = registration["browser_crop_px"]
        diff = ImageChops.difference(reference, candidate)
        outside_differing = sum(
            1 for index, pixel in enumerate(diff.get_flattened_data())
            if not (frame_box[0] <= index % diff.width < frame_box[2]
                    and frame_box[1] <= index // diff.width < frame_box[3])
            and (sum(pixel[:3]) > 2 or pixel[3] > 2))
        locus = []
        if outside_differing:
            locus.append("outside-frame")
        if full["registered_visual"]["score"] < threshold:
            locus.append("in-frame")
        unresolved = page_background_status == "unresolved" and outside_differing > 0
        return {"classification": ("page-background-unresolved" if unresolved
                                   else "unexplained"),
                "threshold": threshold,
                "offset_px": list(offset),
                "outside_frame_differing_pixel_count": outside_differing,
                "page_background_status": page_background_status,
                "residual_locus": locus or ["outside-frame"],
                "reason": (
                    "the page colour around the panel could not be resolved, "
                    "so the observer left it unpainted" if unresolved else
                    "residual is not a panel translation: the native render "
                    "already draws the panel at the authored origin, or there "
                    "is no outside-frame area")}
    fill = Counter(outside_native).most_common(1)[0][0]
    shifted = Image.new("RGBA", candidate.size, fill)
    shifted.paste(candidate, offset)
    shifted_score = score_images(
        reference, shifted, artifacts_dir / "shifted",
        {"source": "panel-translation-shift-control",
         "offset_px": list(offset), "fill": list(fill)})
    diff = ImageChops.difference(reference, shifted)
    frame_box = registration["browser_crop_px"]
    outside_total = 0
    outside_differing = 0
    for index, pixel in enumerate(diff.get_flattened_data()):
        x, y = index % diff.width, index // diff.width
        if frame_box[0] <= x < frame_box[2] and frame_box[1] <= y < frame_box[3]:
            continue
        outside_total += 1
        if sum(pixel[:3]) > 2 or pixel[3] > 2:
            outside_differing += 1
    explained = shifted_score["score"] >= threshold and outside_differing == 0
    locus = []
    if outside_differing:
        locus.append("outside-frame")
    if full["registered_visual"]["score"] < threshold:
        locus.append("in-frame")
    if not explained and not locus:
        locus.append("shifted-frame")
    return {
        "classification": "panel-translation" if explained else "unexplained",
        "threshold": threshold,
        "offset_px": list(offset),
        "fill": list(fill),
        "shifted_score": shifted_score["score"],
        "shifted_outside_frame": {
            "pixel_count": outside_total,
            "differing_pixel_count": outside_differing,
            "differing_pixel_fraction": round(
                outside_differing / max(1, outside_total), 6),
        },
        "artifacts": {
            "side_by_side": "overlays/shifted/side-by-side.png",
            "heatmap": "overlays/shifted/visual-diff.png",
            "overlay": "overlays/shifted/overlay.png",
        },
        **({} if explained else {
            "residual_locus": locus,
            "reason": "residual remains after the panel translation: "
                      "a page-background, overflow, or in-frame defect"}),
    }


def visual_metrics(
    reference_path: Path,
    candidate_path: Path,
    artifacts_dir: Path,
    expected_size: tuple[int, int] | None = None,
    authored_frame: dict[str, Any] | None = None,
    device_scale_factor: float = 1.0,
    native_origin: tuple[float, float] = (0.0, 0.0),
    page_background_status: str | None = None,
) -> dict[str, Any]:
    """Score the full frame and the authored frame cropped from both renders.

    `native_origin` is where the native render drew the panel, in CSS px: the
    observer's placement origin, or (0, 0) for a panel-local render.
    """
    try:
        from PIL import Image
    except ImportError as exc:
        raise LabError("Pillow is required for visual comparison") from exc
    reference = Image.open(reference_path).convert("RGBA")
    candidate = Image.open(candidate_path).convert("RGBA")
    if expected_size and reference.size != expected_size:
        raise LabError(
            "browser screenshot dimensions do not match the captured DPR: "
            f"expected {expected_size}, got {reference.size}")
    if expected_size and candidate.size != expected_size:
        raise LabError(
            "candidate render dimensions do not match the captured DPR: "
            f"expected {expected_size}, got {candidate.size}")
    if reference.size != candidate.size:
        raise LabError(
            "browser and candidate render dimensions differ: "
            f"{reference.size} vs {candidate.size}")
    registration: dict[str, Any] = {
        "source": "full-frame",
        "device_scale_factor": device_scale_factor,
    }
    registered_reference = None
    registered_candidate = None
    if authored_frame is not None:
        try:
            frame_x = float(authored_frame["x"])
            frame_y = float(authored_frame["y"])
            frame_width = float(authored_frame["width"])
            frame_height = float(authored_frame["height"])
        except (KeyError, TypeError, ValueError) as exc:
            raise LabError("authored frame is missing visual registration bounds") from exc
        if not all(math.isfinite(value) and value >= 0.0 for value in
                   (frame_x, frame_y, frame_width, frame_height)):
            raise LabError("authored frame visual registration bounds are invalid")
        scale = float(device_scale_factor)
        if not math.isfinite(scale) or scale <= 0.0:
            raise LabError("device scale factor is invalid for visual registration")
        ref_left = round(frame_x * scale)
        ref_top = round(frame_y * scale)
        ref_right = round((frame_x + frame_width) * scale)
        ref_bottom = round((frame_y + frame_height) * scale)
        crop_width = ref_right - ref_left
        crop_height = ref_bottom - ref_top
        if crop_width <= 0 or crop_height <= 0:
            raise LabError("authored frame visual registration crop is empty")
        ref_box = (ref_left, ref_top, ref_right, ref_bottom)
        native_left = round(float(native_origin[0]) * scale)
        native_top = round(float(native_origin[1]) * scale)
        candidate_box = (native_left, native_top,
                         native_left + crop_width, native_top + crop_height)
        if ref_box[2] > reference.width or ref_box[3] > reference.height:
            raise LabError("authored frame visual registration exceeds browser PNG")
        if candidate_box[2] > candidate.width or candidate_box[3] > candidate.height:
            raise LabError("authored frame visual registration exceeds native PNG")
        registered_reference = reference.crop(ref_box)
        registered_candidate = candidate.crop(candidate_box)
        registration = {
            "source": "authenticated-authored-frame",
            "device_scale_factor": scale,
            "browser_crop_px": list(ref_box),
            "native_crop_px": list(candidate_box),
            "logical_frame": {
                "x": frame_x, "y": frame_y,
                "width": frame_width, "height": frame_height,
            },
        }

    artifacts_dir.mkdir(parents=True, exist_ok=True)
    full = score_images(reference, candidate, artifacts_dir, {
        "source": "full-frame",
        "device_scale_factor": device_scale_factor,
    })
    full["artifacts"] = {
        "side_by_side": "overlays/side-by-side.png",
        "heatmap": "overlays/visual-diff.png",
        "overlay": "overlays/overlay.png",
    }
    registered = registered_reference is not None and registered_candidate is not None
    if registered:
        # Keep the viewport-level background/overflow signal visible alongside
        # the panel-local score. This is diagnostic only: the panel-local crop
        # must never erase pixels outside the authored frame.
        browser_outside = outside_frame_ink(
            reference, tuple(registration["browser_crop_px"]))
        native_outside = outside_frame_ink(
            candidate, tuple(registration["native_crop_px"]))
        full["outside_frame_residual"] = {
            "browser": browser_outside,
            "native": native_outside,
            "differing_pixel_fraction": round(abs(
                browser_outside["differing_pixel_fraction"]
                - native_outside["differing_pixel_fraction"]), 6),
        }
        registered_score = score_images(
            registered_reference, registered_candidate,
            artifacts_dir / "registered", registration)
        registered_score["artifacts"] = {
            "side_by_side": "overlays/registered/side-by-side.png",
            "heatmap": "overlays/registered/visual-diff.png",
            "overlay": "overlays/registered/overlay.png",
        }
        full["registered_visual"] = registered_score
    else:
        full["outside_frame_residual"] = None
        full["registered_visual"] = None
    full["registration"] = registration
    full["full_frame_classification"] = classify_full_frame(
        full, reference, candidate, registration, artifacts_dir,
        registered_reference is not None, page_background_status)
    return full


def full_frame_ok(visual: dict[str, Any]) -> bool:
    """Full frame passes, or its residual is proven to be the panel translation.

    A full-frame score below threshold without a classification is a failure.
    """
    threshold = PROMOTION_THRESHOLDS["visual"]
    if visual.get("score", 0.0) >= threshold:
        return True
    classification = visual.get("full_frame_classification")
    return (isinstance(classification, dict)
            and classification.get("classification") == "panel-translation"
            and float(classification.get("shifted_score", 0.0)) >= threshold
            and (classification.get("shifted_outside_frame") or {}).get(
                "differing_pixel_count") == 0)
