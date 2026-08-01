#!/usr/bin/env python3
"""Tests for the design colour-provenance audit.

The audit decides whether a colour on screen belongs to the design or to Pulp.
Everything here is a case where it got that wrong during calibration and a
plausible answer came back instead of an error, so the negative controls carry
most of the weight:

  * a ``mask-image`` stencil's ``#000`` admitted as a paint colour, which then
    served as a backdrop and legitimised a flat black meter body;
  * a translucent ``rgba(225,235,250,0.1)`` highlight admitted at full strength,
    which legitimised a near-white pointer;
  * the two distances minimised independently, so a declared white answered the
    chromatic distance for *any* neutral Pulp could draw while a warm near-black
    answered the lightness one — no single design colour was near the grey, and
    it scored clean anyway.

Runs without a browser, a build, or a render: the image cases are synthesised.
"""

from __future__ import annotations

import json
import pathlib
import sys
import tempfile
from collections import Counter

sys.path.insert(0, str(pathlib.Path(__file__).parent))

#: ctest's SKIP_RETURN_CODE. Pillow is not a build dependency, and a machine
#: without it should report the gap rather than a red test that reads as a
#: product failure.
SKIP_EXIT = 77

try:
    import PIL  # noqa: F401
except ImportError:
    print("skipping: Pillow (PIL) is not installed; the colour audit needs it")
    raise SystemExit(SKIP_EXIT)

from design_colour_audit import (  # noqa: E402
    DELTA_CH_TOLERANCE,
    DELTA_E_TOLERANCE,
    align,
    build_palette,
    composite_over,
    declared_colours,
    delta_ch,
    delta_e,
    hexs,
    histogram,
    injected_mask,
    oklab_to_rgb,
    parse_colours,
    rgb_to_lab,
    run_audit,
)

FAILURES: list[str] = []


def check(condition: bool, label: str, detail: str = "") -> None:
    if condition:
        print(f"  ok   {label}")
    else:
        FAILURES.append(f"{label}: {detail}")
        print(f"  FAIL {label} — {detail}")


# ---------------------------------------------------------------------------
# Parsing what Chromium resolved
# ---------------------------------------------------------------------------


def test_parse_colours() -> None:
    print("parse_colours")
    check(parse_colours("rgb(196, 98, 42)") == [((196, 98, 42), 1.0)], "rgb()")
    check(
        parse_colours("rgba(60, 50, 30, 0.18)") == [((60, 50, 30), 0.18)],
        "rgba() carries alpha",
    )
    check(parse_colours("#C4622A") == [((196, 98, 42), 1.0)], "hex")
    check(parse_colours("#fff") == [((255, 255, 255), 1.0)], "short hex expands")
    got = parse_colours("linear-gradient(168deg, rgb(247, 240, 224), rgb(239, 230, 210))")
    check(len(got) == 2, "both gradient stops", f"got {got}")
    # The pack writes translucency as a slash form inside oklab().
    lab = parse_colours("oklab(0.244424 0.000586867 0.0135555 / 0.82)")
    check(len(lab) == 1 and abs(lab[0][1] - 0.82) < 1e-6, "oklab alpha", f"got {lab}")
    # Chromium emits oklab for every color-mix(), so a wrong conversion here
    # silently shifts a whole design's shadow and highlight colours. Both
    # values below are verbatim from a captured pack, and both must land on the
    # hex the pack declares for that token.
    for oklab, expected, token in (
        ((0.95629, 0.00115952, 0.0224435), (247, 240, 224), "--surface-raised"),
        ((0.244424, 0.000586867, 0.0135555), (35, 32, 25), "--surface-app"),
    ):
        got = oklab_to_rgb(*oklab)
        check(
            max(abs(a - b) for a, b in zip(got, expected)) <= 1,
            f"oklab resolves {token}",
            f"got {hexs(got)}, expected {hexs(expected)}",
        )


def test_stencil_colours_are_not_paint() -> None:
    print("stencil colours are excluded")
    snapshot = {
        "strings": [
            "radial-gradient(farthest-side, rgba(0, 0, 0, 0) calc(100% - 7px), rgb(0, 0, 0) calc(100% - 7px))",
            "rgb(196, 98, 42)",
        ],
        "computedStyleNames": ["mask-image", "background-color"],
        "documents": [{"layout": {"styles": [[0, 1]]}}],
    }
    found = declared_colours(snapshot)
    values = {rgb for rgb, _alpha, _src in found}
    check((196, 98, 42) in values, "a paint colour is admitted")
    check(
        (0, 0, 0) not in values,
        "a mask-image stencil colour is not admitted",
        f"got {sorted(values)}",
    )


# ---------------------------------------------------------------------------
# The palette rules
# ---------------------------------------------------------------------------


def test_alpha_enters_composited_not_at_full_strength() -> None:
    print("translucent colours enter composited")
    cream = (247, 240, 224)
    reference = Counter({cream: 10_000})
    declared = [((225, 235, 250), 0.1, "background-color: rgba(225,235,250,0.1)")]
    palette = build_palette(declared, reference)
    members = {m.rgb for m in palette.members}
    check(
        (225, 235, 250) not in members,
        "the raw near-white is NOT a design colour",
        f"members: {sorted(members)}",
    )
    expected = composite_over((225, 235, 250), 0.1, cream)
    check(expected in members, "its composite over the painted cream is", f"want {hexs(expected)}")


def test_one_witness_must_answer_both_distances() -> None:
    """The hole that let a stock grey pass.

    A design of warm creams and a warm near-black, which also declares white
    somewhere. White is at chroma zero, so it answers the chromatic distance
    for any neutral; the warm near-black answers the lightness one. If the two
    minima may come from different colours, ``#1E1E1E`` — a colour this design
    does not contain — scores clean.
    """
    print("one witness must answer both distances")
    reference = Counter({(247, 240, 224): 50_000, (33, 31, 24): 20_000})
    declared = [
        ((255, 255, 255), 1.0, "color: rgb(255,255,255)"),
        ((247, 240, 224), 1.0, "background-color: rgb(247,240,224)"),
        ((33, 31, 24), 1.0, "background-color: rgb(33,31,24)"),
    ]
    palette = build_palette(declared, reference)

    stock_grey = (30, 30, 30)
    e, ch, _why = palette.nearest(stock_grey, DELTA_E_TOLERANCE, DELTA_CH_TOLERANCE)
    check(
        e > DELTA_E_TOLERANCE or ch > DELTA_CH_TOLERANCE,
        "a stock neutral grey is foreign to a warm palette",
        f"dE {e:.2f} dCh {ch:.2f} — under both tolerances, so it would pass",
    )

    # Independently-minimised distances are what made it pass; assert the
    # separate minima really would have.
    lab = rgb_to_lab(stock_grey)
    loose_ch = min(delta_ch(lab, m.lab) for m in palette.members)
    loose_e = min(delta_e(lab, m.lab) for m in palette.members)
    check(
        loose_ch <= DELTA_CH_TOLERANCE,
        "independent minima would have passed it (why the rule exists)",
        f"loose dCh {loose_ch:.2f} > tolerance, so this regression case no longer bites",
    )
    check(loose_e > 0, "and lightness alone does not decide it", f"loose dE {loose_e:.2f}")

    # A colour the design does contain must still be accepted.
    e2, ch2, _ = palette.nearest((247, 240, 224), DELTA_E_TOLERANCE, DELTA_CH_TOLERANCE)
    check(
        e2 <= DELTA_E_TOLERANCE and ch2 <= DELTA_CH_TOLERANCE,
        "a colour the design paints is accepted",
        f"dE {e2:.2f} dCh {ch2:.2f}",
    )


def test_interpolation_is_allowed_but_identity_is_not_interpolated() -> None:
    print("interpolation vs identity")
    reference = Counter({(247, 240, 224): 50_000, (33, 31, 24): 20_000})
    palette = build_palette([], reference)
    midpoint = tuple(round(a + (b - a) * 0.5) for a, b in zip((247, 240, 224), (33, 31, 24)))
    e, ch, why = palette.nearest(midpoint, DELTA_E_TOLERANCE, DELTA_CH_TOLERANCE)
    check(
        e <= DELTA_E_TOLERANCE and ch <= DELTA_CH_TOLERANCE,
        "a blend of two design colours is a design colour",
        f"dE {e:.2f} dCh {ch:.2f} via {why}",
    )
    distance, witness = palette.exact_match(midpoint, 2.0)
    check(
        witness is None,
        "but identity refuses that same blend",
        f"matched {witness} at dE {distance:.2f}; a constant sitting inside the "
        "design's range would then read as derived",
    )
    distance, witness = palette.exact_match((247, 240, 224), 2.0)
    check(witness is not None, "identity accepts the colour itself", f"dE {distance:.2f}")


# ---------------------------------------------------------------------------
# The injected mask, end to end on a synthetic capture
# ---------------------------------------------------------------------------


def _write_capture(root: pathlib.Path, reference_rgb, render_pixels) -> pathlib.Path:
    """A minimal capture directory: a flat reference and a render that differs
    only where we say it does."""
    from PIL import Image

    capture = root / "ir-browser-capture"
    (capture / "validation-proof" / "render").mkdir(parents=True)
    w = h = 40
    reference = Image.new("RGB", (w, h), reference_rgb)
    reference.save(capture / "browser.png")
    render = reference.copy()
    px = render.load()
    for (x, y), colour in render_pixels.items():
        px[x, y] = colour
    render.save(capture / "validation-proof" / "render" / "render.png")
    (capture / "capture.json").write_text(
        json.dumps(
            {
                "provenance": {
                    "viewport": {
                        "device_scale_factor": 1,
                        "document": {
                            "primary_surface": {"left": 0, "top": 0, "width": w, "height": h}
                        },
                    }
                },
                "reference": {"path": "browser.png"},
            }
        )
    )
    (capture / "dom-snapshot.json").write_text(
        json.dumps(
            {
                "strings": [f"rgb({reference_rgb[0]}, {reference_rgb[1]}, {reference_rgb[2]})"],
                "computedStyleNames": ["background-color"],
                "documents": [{"layout": {"styles": [[0]]}}],
            }
        )
    )
    return capture


def test_injected_mask_and_end_to_end() -> None:
    print("injected mask + end-to-end audit")
    with tempfile.TemporaryDirectory() as tmp:
        root = pathlib.Path(tmp)
        cream = (247, 240, 224)
        # 100 pixels of a green that this design does not contain.
        marks = {(x, y): (80, 200, 80) for x in range(10, 20) for y in range(10, 20)}
        capture = _write_capture(root, cream, marks)

        pair = align(capture)
        mask = injected_mask(pair)
        check(sum(mask) == 100, "the mask finds exactly the painted pixels", f"got {sum(mask)}")

        hist = histogram(pair.render, mask)
        check(hist == Counter({(80, 200, 80): 100}), "and only their colour", f"got {hist}")

        report = run_audit(capture, None)
        foreign = [f["hex"] for f in report["global"]["foreign_colours"]]
        check(foreign == ["#50C850"], "the audit names the foreign green", f"got {foreign}")
        check(
            report["global"]["foreign_pixels"] == 100,
            "with its pixel count",
            f"got {report['global']['foreign_pixels']}",
        )


def test_a_mark_in_a_design_colour_is_clean() -> None:
    """The positive control. A test that can only go red proves nothing."""
    print("a mark painted in the design's own colour is clean")
    with tempfile.TemporaryDirectory() as tmp:
        root = pathlib.Path(tmp)
        cream = (247, 240, 224)
        rust = (196, 98, 42)
        marks = {(x, y): rust for x in range(10, 20) for y in range(10, 20)}
        capture = _write_capture(root, cream, marks)
        # The design declares the rust even though the flat reference does not
        # paint it — which is the case a value arc lives in.
        snapshot = json.loads((capture / "dom-snapshot.json").read_text())
        snapshot["strings"].append("rgb(196, 98, 42)")
        snapshot["computedStyleNames"].append("color")
        snapshot["documents"][0]["layout"]["styles"] = [[0, 1]]
        (capture / "dom-snapshot.json").write_text(json.dumps(snapshot))

        report = run_audit(capture, None)
        check(
            not report["global"]["foreign_colours"],
            "no foreign colour reported",
            f"got {[f['hex'] for f in report['global']['foreign_colours']]}",
        )


def main() -> int:
    for test in (
        test_parse_colours,
        test_stencil_colours_are_not_paint,
        test_alpha_enters_composited_not_at_full_strength,
        test_one_witness_must_answer_both_distances,
        test_interpolation_is_allowed_but_identity_is_not_interpolated,
        test_injected_mask_and_end_to_end,
        test_a_mark_in_a_design_colour_is_clean,
    ):
        test()
    print()
    if FAILURES:
        print(f"{len(FAILURES)} failure(s):")
        for f in FAILURES:
            print(f"  - {f}")
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
