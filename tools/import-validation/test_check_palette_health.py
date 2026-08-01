#!/usr/bin/env python3
"""Unit tests for check_palette_health.py.

Every assertion in the checker is exercised in BOTH directions — a palette that
must fail it and a palette that must pass it. A one-directional test cannot tell
a working check from one that fails everything, and a checker that fails
everything is indistinguishable from a broken palette until someone runs it on
something healthy.
"""

from __future__ import annotations

import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).with_name("check_palette_health.py")
SPEC = importlib.util.spec_from_file_location("check_palette_health", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
cph = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = cph
SPEC.loader.exec_module(cph)


# A palette with real tonal structure, drawn from the shape the shipped
# ink-signal pack uses: an accent with a derived ramp, named hues that keep
# their own hue, and a text ramp with headroom.
HEALTHY = {
    "surface-app": "#161A21",
    "surface-panel": "#1E2530",
    "surface-raised": "#28303C",
    "surface-sunken": "#0E1116",
    "surface-inset": "#0C0F14",
    "surface-overlay": "#2F3743",
    "control": "#2A323D",
    "control-hover": "#323B47",
    "knob-base": "#2C333E",
    "text-strong": "#F3F6F9",
    "text": "#D6DCE4",
    "text-muted": "#B6BCC5",
    "text-faint": "#838A95",
    "accent": "#16DAC2",
    "accent-base": "#16DAC2",
    "accent-press": "#10B6A3",
    "accent-text": "#04211E",
    "accent-soft": "rgba(22,218,194,0.15)",
    "accent-soft-2": "rgba(22,218,194,0.26)",
    "accent-line": "rgba(22,218,194,0.45)",
    "accent-ring": "rgba(22,218,194,0.5)",
    "ink-signal": "#16DAC2",
    "ink-violet": "#8B6CF5",
    "ink-indigo": "#5E78FF",
    "ink-amber": "#F6B847",
    "ink-coral": "#FF5C4D",
    "ink-leaf": "#3FCF77",
    "ink-pink": "#FF7AA8",
    "info": "#5E78FF",
    "success": "#3FCF77",
    "warning": "#F6B847",
    "danger": "#FF5C4D",
}


def variant(**overrides: str) -> dict[str, str]:
    out = dict(HEALTHY)
    out.update(overrides)
    return out


def problems_of(tokens: dict[str, str]) -> list[str]:
    found: list[str] = []
    for check in (cph.check_accent_ramp, cph.check_hue_family,
                  cph.check_text_contrast):
        found += check(tokens)[0]
    return found


class ColourParsing(unittest.TestCase):
    def test_hex_forms(self) -> None:
        self.assertEqual(cph.parse_color("#39FF6A"), (0x39, 0xFF, 0x6A, 1.0))
        self.assertEqual(cph.parse_color("#abc"), (0xAA, 0xBB, 0xCC, 1.0))
        self.assertEqual(cph.parse_color("#00000080")[3], 128 / 255.0)

    def test_rgba_forms(self) -> None:
        self.assertEqual(cph.parse_color("rgb(1, 2, 3)"), (1, 2, 3, 1.0))
        self.assertEqual(cph.parse_color("rgba(57,255,106,0.18)"),
                         (57, 255, 106, 0.18))

    def test_unresolvable_values_are_none_not_guessed(self) -> None:
        # A guessed colour yields a plausible ratio for something nobody sees.
        for value in ("var(--accent)", "color-mix(in oklab, red 40%, transparent)",
                      "linear-gradient(#fff, #000)", "", "not-a-colour"):
            self.assertIsNone(cph.parse_color(value), value)

    def test_contrast_matches_known_wcag_pairs(self) -> None:
        black, white = (0, 0, 0, 1.0), (255, 255, 255, 1.0)
        self.assertAlmostEqual(cph.contrast(black, white), 21.0, places=2)
        self.assertAlmostEqual(cph.contrast(white, white), 1.0, places=6)

    def test_translucent_foreground_is_composited_before_scoring(self) -> None:
        # A ratio taken against a colour with alpha is a ratio against nothing.
        surface = (0, 0, 0, 1.0)
        half_white = (255, 255, 255, 0.5)
        self.assertEqual(cph.composite(half_white, surface)[:3], (128, 128, 128))


class AccentRamp(unittest.TestCase):
    def test_healthy_ramp_passes(self) -> None:
        self.assertEqual(cph.check_accent_ramp(HEALTHY)[0], [])

    def test_a_role_equal_to_the_accent_is_reported(self) -> None:
        found = cph.check_accent_ramp(variant(**{"accent-soft": "#16DAC2"}))[0]
        self.assertTrue(any("--accent-soft is identical" in p for p in found), found)

    def test_accent_base_may_alias_the_accent(self) -> None:
        # The packs define `--accent: var(--accent-base)`. Reporting the alias
        # would fail every correct pack.
        found = cph.check_accent_ramp(variant(**{"accent-base": "#16DAC2"}))[0]
        self.assertEqual(found, [])

    def test_the_shipped_collapse_is_caught(self) -> None:
        flat = variant(**{k: "#39FF6A" for k in
                          ("accent", "accent-base", "accent-press", "accent-line",
                           "accent-ring", "accent-soft", "accent-soft-2",
                           "accent-text")})
        found = cph.check_accent_ramp(flat)[0]
        self.assertTrue(any("only 1 distinct value" in p for p in found), found)
        self.assertTrue(any("1.00:1" in p for p in found), found)

    def test_accent_text_below_the_bar_is_reported_even_when_distinct(self) -> None:
        # Distinctness alone is not legibility: a slightly-off accent-text is a
        # different value and still unreadable on the fill.
        found = cph.check_accent_ramp(variant(**{"accent-text": "#15D8C0"}))[0]
        self.assertTrue(any("--accent-text" in p and "bar" in p for p in found),
                        found)

    def test_a_ramp_that_keeps_three_rungs_is_still_a_ramp(self) -> None:
        # Moves the boundary in the passing direction: tying ring to line is a
        # style choice, not a collapse.
        found = cph.check_accent_ramp(
            variant(**{"accent-ring": HEALTHY["accent-line"]}))[0]
        self.assertEqual(found, [])


class HueFamily(unittest.TestCase):
    def test_healthy_family_passes(self) -> None:
        self.assertEqual(cph.check_hue_family(HEALTHY)[0], [])

    def test_one_ink_may_equal_the_accent(self) -> None:
        # --accent IS var(--ink-signal) in the shipped pack.
        self.assertEqual(cph.check_hue_family(HEALTHY)[0], [])

    def test_two_inks_holding_the_accent_is_reported(self) -> None:
        found = cph.check_hue_family(
            variant(accent="#39FF6A", **{"ink-signal": "#39FF6A",
                                         "ink-amber": "#39FF6A"}))[0]
        self.assertTrue(any("named hues hold the accent" in p for p in found),
                        found)

    def test_status_roles_sharing_a_colour_are_reported(self) -> None:
        found = cph.check_hue_family(variant(warning="#3FCF77"))[0]
        self.assertTrue(any("--warning" in p and "--success" in p for p in found),
                        found)


class TextContrast(unittest.TestCase):
    def test_healthy_ramp_passes(self) -> None:
        self.assertEqual(cph.check_text_contrast(HEALTHY)[0], [])

    def test_the_shipped_muted_value_is_reported(self) -> None:
        found = cph.check_text_contrast(variant(**{"text-muted": "#4A7A50"}))[0]
        self.assertTrue(any("--text-muted" in p for p in found), found)

    def test_muted_at_plain_aa_is_still_reported(self) -> None:
        # The bar is AA PLUS headroom, so a value that only just clears AA on
        # the quietest surface must still fail. Without this the bar could be
        # lowered to 4.5 and no test would notice.
        surfaces = [HEALTHY[s] for s in cph.SURFACES if s in HEALTHY]
        aa_only = "#AAB1BB"
        worst = min(cph.contrast(cph.parse_color(aa_only), cph.parse_color(s))
                    for s in surfaces)
        self.assertGreaterEqual(worst, 4.5)
        self.assertLess(worst, 5.5)
        found = cph.check_text_contrast(variant(**{"text-muted": aa_only}))[0]
        self.assertTrue(any("--text-muted" in p for p in found), found)

    def test_faint_is_held_at_the_quiet_bar_not_the_body_bar(self) -> None:
        # Forcing faint to AA would erase the tier. A value between 3.0 and 4.5
        # must pass.
        found = cph.check_text_contrast(variant(**{"text-faint": "#838A95"}))[0]
        self.assertEqual(found, [])
        surfaces = [HEALTHY[s] for s in cph.SURFACES if s in HEALTHY]
        worst = min(cph.contrast(cph.parse_color("#838A95"), cph.parse_color(s))
                    for s in surfaces)
        self.assertLess(worst, 4.5)

    def test_every_surface_is_scored_not_just_the_app_background(self) -> None:
        # The tier ran out on --control-hover, not on the deck. A checker that
        # only scored surface-app would have passed the panel that shipped.
        on_app_only = variant(**{"text-muted": "#9EA6B0",
                                 "control-hover": "#5A6472"})
        found = cph.check_text_contrast(on_app_only)[0]
        self.assertTrue(any("control-hover" in p for p in found), found)

    def test_a_translucent_surface_is_composited_over_the_app(self) -> None:
        found = cph.check_text_contrast(
            variant(**{"surface-overlay": "rgba(255,255,255,0.92)"}))[0]
        self.assertTrue(any("surface-overlay" in p for p in found), found)


class CommandLine(unittest.TestCase):
    def run_cli(self, payload: dict, suffix: str = ".json",
                flag: str = "--tokens") -> subprocess.CompletedProcess:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / f"doc{suffix}"
            if suffix == ".json":
                path.write_text(json.dumps({"tokens": {"colors": payload}}))
            else:
                path.write_text("\n".join(
                    f'setColorToken("css/{k}", "{v}");' for k, v in payload.items()))
            return subprocess.run(
                [sys.executable, str(SCRIPT), flag, str(path)],
                capture_output=True, text=True)

    def test_healthy_palette_exits_zero(self) -> None:
        result = self.run_cli(HEALTHY)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_collapsed_palette_exits_assert(self) -> None:
        flat = variant(**{k: "#39FF6A" for k in
                          ("accent", "accent-soft", "accent-soft-2",
                           "accent-line", "accent-ring", "accent-press",
                           "accent-text")})
        result = self.run_cli(flat)
        self.assertEqual(result.returncode, cph.EX_ASSERT,
                         result.stdout + result.stderr)

    def test_the_emitted_artifact_is_judgeable_too(self) -> None:
        # The script is what a plugin loads; it can drift from the IR beside it.
        result = self.run_cli(HEALTHY, suffix=".js", flag="--artifact")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_a_token_set_with_nothing_to_judge_is_a_harness_fault(self) -> None:
        # Never a pass: an empty palette satisfies every assertion vacuously.
        result = self.run_cli({"surface-app": "#000000"})
        self.assertEqual(result.returncode, cph.EX_HARNESS,
                         result.stdout + result.stderr)

    def test_a_missing_input_is_an_input_fault(self) -> None:
        result = subprocess.run(
            [sys.executable, str(SCRIPT), "--tokens", "/nonexistent/tokens.json"],
            capture_output=True, text=True)
        self.assertEqual(result.returncode, cph.EX_INPUT, result.stderr)


class ShippedPack(unittest.TestCase):
    """The pack in this repo must satisfy the bars this checker states.

    A checker whose own repo fails it is a checker nobody can turn on.
    """

    def test_ink_signal_dark_and_light_clear_the_bars(self) -> None:
        css = (Path(__file__).resolve().parents[2] / "assets" / "design-system"
               / "ink-signal" / "tokens" / "css" / "semantic.css").read_text()
        surfaces_dark = ["#161A21", "#1E2530", "#28303C", "#0E1116", "#0C0F14",
                         "#2F3743", "#2A323D", "#323B47", "#2C333E"]
        surfaces_light = ["#EDEFF2", "#FAFBFC", "#FFFFFF", "#E0E4E9", "#E6E9EE",
                          "#F6F8FA", "#EDF0F4"]
        # Split on the SELECTOR, not the bare string: the file's header
        # comment names the light theme too, and splitting on that silently
        # scores the comment as if it were a theme block.
        blocks = css.split('[data-theme="light"] {')
        self.assertEqual(len(blocks), 2, "semantic.css lost its light theme")
        for text, surfaces in ((blocks[0], surfaces_dark),
                               (blocks[1], surfaces_light)):
            for tier, bar in (("text-muted", 5.5), ("text-faint", 3.0)):
                value = [line.split(":")[1].strip().rstrip(";")
                         for line in text.splitlines()
                         if line.strip().startswith(f"--{tier}:")]
                self.assertEqual(len(value), 1, f"--{tier} in {text[:40]!r}")
                colour = cph.parse_color(value[0])
                self.assertIsNotNone(colour, value[0])
                worst = min(cph.contrast(colour, cph.parse_color(s))
                            for s in surfaces)
                self.assertGreaterEqual(
                    worst, bar,
                    f"--{tier} {value[0]} is {worst:.2f}:1 on its own pack's "
                    f"quietest surface, under the {bar}:1 bar")


if __name__ == "__main__":
    unittest.main()
