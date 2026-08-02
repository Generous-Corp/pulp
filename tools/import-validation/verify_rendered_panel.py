#!/usr/bin/env python3
"""Score a rendered UI script against the render its importer claimed to produce.

The failure this exists to catch: the import pipeline reports a high similarity
for the DesignIR render it made in memory, while the *emitted script* — the
artifact that actually ships and that a plugin loads — renders to something else
entirely. Scoring the pipeline and calling it the product is how a panel at 0.13
was certified at 0.98.

So the subject is always the artifact on disk, supplied as a parameter, and the
reference is the importer's own render. Divergence between them is the bug.

Exit codes are distinct so a caller can tell apart "the panel is wrong" from
"the harness could not measure it":

    0  pass
    2  a required input is missing, empty, or unreadable
    3  rendering the artifact failed
    4  rendered and reference disagree on size (never scored: a size mismatch
       scores the intersection and returns a plausible number)
    5  similarity below threshold
    6  a direct assertion failed (missing image source, foreign colour)
    7  the renderer cannot execute this artifact — a harness fault, not a
       panel fault, and never reportable as a low score

Exit 7 exists because a stale binary is indistinguishable from a broken panel
if you only look at the number. A pulp-screenshot six weeks behind the importer
silently no-ops the artifact's setTheme/setColorToken calls, renders a stock
dark background, and scores 0.0006 against a cream design. Left unguarded that
reads as a catastrophic regression; it is a build-order mistake.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import struct
import subprocess
import sys
from pathlib import Path

EX_INPUT, EX_RENDER, EX_SIZE, EX_SCORE, EX_ASSERT, EX_HARNESS = 2, 3, 4, 5, 6, 7

# Runtime entry points whose absence silently changes what the panel looks
# like: the call is simply not in the renderer's JS global scope, so token and
# layout setup vanish and the panel renders with stock defaults instead of the
# design's. `setTheme` is deliberately NOT in this list — it is registered in
# theme_api.cpp but is not linked into every renderer, and its absence does not
# by itself stop the design's colours from applying. Listing it would block
# rendering on a binary that renders correctly, which is the same class of
# false signal this preflight exists to prevent.
REQUIRED_RUNTIME_API = ("setColorToken", "setSubpixelLayout")


def fail(code: int, message: str) -> "NoReturn":  # type: ignore[valid-type]
    print(f"FAIL: {message}", file=sys.stderr)
    sys.exit(code)


def png_size(path: Path) -> tuple[int, int]:
    with path.open("rb") as handle:
        header = handle.read(33)
    if len(header) < 24 or header[:8] != b"\x89PNG\r\n\x1a\n":
        fail(EX_INPUT, f"not a PNG: {path}")
    width, height = struct.unpack(">II", header[16:24])
    return width, height


def require_file(path: Path, what: str) -> Path:
    # A missing reference must be a hard failure, never a skip. A skip reads as
    # a pass in every CI summary that aggregates these.
    if not path.is_file():
        fail(EX_INPUT, f"{what} does not exist: {path}")
    if path.stat().st_size == 0:
        fail(EX_INPUT, f"{what} is empty: {path}")
    return path


def resolve_screenshot_binary(explicit: str | None) -> Path:
    if explicit:
        return require_file(Path(explicit), "screenshot binary")
    here = Path(__file__).resolve()
    for parent in here.parents:
        candidate = parent / "build" / "tools" / "screenshot" / "pulp-screenshot"
        if candidate.is_file():
            return candidate
    fail(EX_INPUT, "could not locate pulp-screenshot; pass --screenshot-bin")


def canvas_from_meta(artifact: Path) -> tuple[int, int] | None:
    meta = artifact.with_suffix(artifact.suffix + ".meta.json")
    if not meta.is_file():
        return None
    try:
        canvas = json.loads(meta.read_text()).get("canvas") or {}
        return int(canvas["width"]), int(canvas["height"])
    except (ValueError, KeyError, TypeError):
        return None


def assert_renderer_can_execute(binary: Path, artifact: Path) -> None:
    """Refuse to score when the renderer predates the artifact's runtime API.

    Checked against the binary's own string table rather than its build date:
    a date comparison invites "it looks recent enough", and the question is not
    how old the binary is but whether it implements what this artifact calls.
    """
    try:
        image = binary.read_bytes()
        source = artifact.read_text(errors="replace")
    except OSError as exc:
        fail(EX_HARNESS, f"could not inspect renderer capabilities: {exc}")
    used = {name for name in REQUIRED_RUNTIME_API
            if re.search(rf"\b{name}\s*\(", source)}
    binary_strings = set(re.split(rb"[^\x20-\x7e]+", image))
    missing = sorted(name for name in used if name.encode() not in binary_strings)
    if missing:
        fail(EX_HARNESS,
             f"renderer cannot execute this artifact — missing runtime API: "
             f"{', '.join(missing)}\n"
             f"  renderer: {binary}\n"
             f"  This is a stale-binary fault, NOT a panel regression. Rebuild "
             f"pulp-screenshot from the same tree as the importer and re-run. "
             f"Scoring past this point yields a near-zero similarity that looks "
             f"exactly like a broken design.")


def render(binary: Path, artifact: Path, out: Path, width: int, height: int,
           scale: float, backend: str) -> None:
    env = dict(os.environ)
    # Without this the capture backdrop — an oversize absolutely-positioned
    # child — is rescaled to the viewport, which moves the artwork out from
    # under the controls placed against it. The error runs in BOTH directions:
    # it flatters a broken panel and crushes a correct one, so it cannot be
    # corrected for after the fact.
    env["PULP_SHOT_NO_RECONCILE"] = "1"
    command = [
        str(binary), "--script", str(artifact), "--output", str(out),
        "--width", str(width), "--height", str(height),
        "--scale", str(scale), "--backend", backend,
    ]
    result = subprocess.run(command, env=env, capture_output=True, text=True)
    if result.returncode != 0:
        fail(EX_RENDER,
             f"render exited {result.returncode}\n"
             f"  command: {' '.join(command)}\n"
             f"  stderr: {result.stderr.strip()[:2000]}")


def parse_similarity(stdout: str) -> float | None:
    match = re.search(r"similarity=([0-9]*\.?[0-9]+)", stdout)
    return float(match.group(1)) if match else None


def assert_image_sources(artifact: Path) -> list[str]:
    """Every image node must carry a non-empty source.

    The regression this catches drops the backdrop entirely: the node is still
    emitted and still laid out, so node counts, layout assertions and a
    box-level diff all stay green while the artwork is simply absent.
    """
    text = artifact.read_text(errors="replace")
    problems: list[str] = []
    creates = re.findall(r"\bcreateImage\s*\(", text)
    # Two lanes reach the same ImageView and both count: the native
    # setImageSource the CLI emits, and the web-compat img.src the browser
    # lane reflects. Matching only one reports a missing source on an artifact
    # that has a perfectly good one.
    sources = re.findall(
        r"(?:setImageSource|setImageSrc|\.src\s*=|setAttribute\(\s*['\"]src['\"])",
        text)
    if creates and not sources:
        problems.append(
            f"{len(creates)} image node(s) emitted but no source assignment found")
    if re.search(r"setImageSource\(\s*[^,]+,\s*['\"]\s*['\"]\s*\)", text) or \
            re.search(r"\.src\s*=\s*['\"]['\"]", text):
        problems.append("an image source is assigned the empty string")
    return problems


def assert_no_foreign_colour(rendered: Path, tokens: Path | None,
                             tolerance: int, budget: float) -> list[str]:
    """Pixels should come from the design's palette, not the renderer's.

    Similarity alone is blind to this: stock colours land on small, dense areas
    — arcs, pointers, meters — that a whole-image score barely registers.
    """
    # Both of these were silent `return []` and therefore a PASS that named a
    # check it never performed — the exact defect this gate replaces. A check
    # that cannot run is a measurement gap, not a clean result. Similarity is
    # blind to the palette class (a panel painting stock blue on a cream design
    # scores 0.94), so skipping this one silently certifies that regression.
    if tokens is None or not tokens.is_file():
        fail(EX_HARNESS,
             "the foreign-colour check has no token allow-list: pass --tokens "
             "<tokens.json>, or --skip-colour-check to state the omission "
             "deliberately. Similarity alone cannot see a palette regression.")
    try:
        from PIL import Image
    except ImportError:
        fail(EX_HARNESS,
             "the foreign-colour check requires PIL and it is not importable. "
             "Install it in a venv (system python3 may be PEP-668 blocked), or "
             "pass --skip-colour-check to state the omission deliberately. "
             "Exiting rather than reporting a pass for a check that did not run.")

    palette: list[tuple[int, int, int]] = []
    def walk(node: object) -> None:
        if isinstance(node, dict):
            for value in node.values():
                walk(value)
        elif isinstance(node, list):
            for value in node:
                walk(value)
        elif isinstance(node, str):
            hexmatch = re.fullmatch(r"#([0-9a-fA-F]{6})", node.strip())
            if hexmatch:
                raw = hexmatch.group(1)
                palette.append(tuple(int(raw[i:i + 2], 16) for i in (0, 2, 4)))
    # strict=False: captured token values legitimately carry raw control
    # characters from the source stylesheet.
    walk(json.loads(tokens.read_text(errors="replace"), strict=False))
    if not palette:
        fail(EX_HARNESS,
             f"no colours parsed from {tokens} — the allow-list would be empty "
             f"and the check vacuously green")

    image = Image.open(rendered).convert("RGB")
    image.thumbnail((400, 400))
    total = 0
    foreign = 0
    for count, pixel in image.getcolors(maxcolors=1 << 24) or []:
        total += count
        near = any(abs(pixel[0] - r) <= tolerance
                   and abs(pixel[1] - g) <= tolerance
                   and abs(pixel[2] - b) <= tolerance
                   for r, g, b in palette)
        if not near:
            foreign += count
    if not total:
        return []
    ratio = foreign / total
    print(f"foreign_colour_ratio={ratio:.4f} budget={budget:.4f} "
          f"(palette={len(palette)} colours, tolerance={tolerance})")
    if ratio > budget:
        return [f"{ratio:.1%} of pixels fall outside the design palette "
                f"(budget {budget:.1%})"]
    return []


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifact", required=True,
                        help="the emitted UI script that actually ships")
    parser.add_argument("--reference", required=True,
                        help="the render the importer claimed to produce")
    parser.add_argument("--out-dir", default=None)
    parser.add_argument("--width", type=int, default=None)
    parser.add_argument("--height", type=int, default=None)
    parser.add_argument("--scale", type=float, default=None)
    parser.add_argument("--threshold", type=float, default=0.85)
    parser.add_argument("--backend", default="skia",
                        help="skia only; CoreGraphics draws filenames as text")
    parser.add_argument("--screenshot-bin", default=None)
    parser.add_argument("--tokens", default=None)
    parser.add_argument("--colour-tolerance", type=int, default=24)
    # 0.10 sits between a correct panel (~0.05) and a palette regression
    # (~0.19), roughly 2x margin either side. Antialiased edges and gradient
    # interpolation legitimately produce colours outside the token set, so the
    # floor scales with how much soft-edged chrome a design carries — a blurred
    # or photographic backdrop sits higher. The ratio prints on every run, pass
    # or fail, so drift is visible before it trips.
    parser.add_argument("--foreign-colour-budget", type=float, default=0.10)
    parser.add_argument("--skip-colour-check", action="store_true")
    args = parser.parse_args()

    artifact = require_file(Path(args.artifact).resolve(), "artifact")
    reference = require_file(Path(args.reference).resolve(), "reference render")
    binary = resolve_screenshot_binary(args.screenshot_bin)

    if args.backend != "skia":
        print(f"warning: backend '{args.backend}' is not skia; image content "
              f"may render as placeholder text", file=sys.stderr)

    out_dir = Path(args.out_dir).resolve() if args.out_dir else artifact.parent
    out_dir.mkdir(parents=True, exist_ok=True)
    rendered = out_dir / f"{artifact.stem}-gate-render.png"
    diff = out_dir / f"{artifact.stem}-gate-diff.png"

    ref_w, ref_h = png_size(reference)
    canvas = (args.width, args.height) if args.width and args.height \
        else canvas_from_meta(artifact)
    if not canvas:
        fail(EX_INPUT, "canvas size unknown: pass --width/--height or provide "
                       f"{artifact.name}.meta.json")
    width, height = canvas

    scale = args.scale if args.scale else (ref_w / width if width else 1.0)
    print(f"artifact={artifact}\nreference={reference} ({ref_w}x{ref_h})\n"
          f"canvas={width}x{height} scale={scale:g} backend={args.backend}")

    assert_renderer_can_execute(binary, artifact)
    render(binary, artifact, rendered, width, height, scale, args.backend)
    require_file(rendered, "rendered output")

    got_w, got_h = png_size(rendered)
    if (got_w, got_h) != (ref_w, ref_h):
        # Never score past this point. A size mismatch is compared over the
        # intersection and yields a plausible-looking number.
        fail(EX_SIZE, f"size mismatch: rendered {got_w}x{got_h} vs "
                      f"reference {ref_w}x{ref_h}")

    compare = subprocess.run(
        [str(binary), "--compare", str(reference), str(rendered),
         "--threshold", str(args.threshold), "--diff", str(diff)],
        capture_output=True, text=True)
    print(compare.stdout.strip())
    similarity = parse_similarity(compare.stdout)
    if compare.returncode == 2 or similarity is None:
        fail(EX_INPUT, f"comparison could not run: {compare.stderr.strip()[:500]}")

    problems = assert_image_sources(artifact)
    if not args.skip_colour_check:
        problems += assert_no_foreign_colour(
            rendered, Path(args.tokens).resolve() if args.tokens else None,
            args.colour_tolerance, args.foreign_colour_budget)

    if similarity < args.threshold:
        for problem in problems:
            print(f"  also: {problem}", file=sys.stderr)
        fail(EX_SCORE,
             f"similarity {similarity:.4f} below threshold {args.threshold:.4f}"
             f"\n  diff image: {diff}")
    if problems:
        for problem in problems:
            print(f"  - {problem}", file=sys.stderr)
        fail(EX_ASSERT, f"similarity {similarity:.4f} passed but "
                        f"{len(problems)} direct assertion(s) failed")

    print(f"PASS similarity={similarity:.4f} threshold={args.threshold:.4f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
