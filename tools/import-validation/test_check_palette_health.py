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
    for check in cph.ENFORCING_CHECKS:
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


class PackResolution(unittest.TestCase):
    """A derived pack's override block must reach the resolver.

    The bug this pins: the override a derived pack ships is scoped
    `:root, [data-theme="dark"], [data-theme="light"]`. A theme filter that
    rejects any selector MENTIONING the other theme drops that whole block, so
    the resolver reports the base pack's healthy palette for a pack whose
    palette has been flattened — a false clean, and the worst possible failure
    mode for a checker.
    """

    def write_pack(self, tmp: Path) -> Path:
        pack = tmp / "derived"
        (pack / "tokens").mkdir(parents=True)
        (pack / "tokens" / "a-base.css").write_text(
            ":root, [data-theme=\"dark\"] {\n"
            "  --accent: #16DAC2;\n"
            "  --accent-soft: rgba(22,218,194,0.15);\n"
            "  --accent-line: rgba(22,218,194,0.45);\n"
            "}\n"
            "[data-theme=\"light\"] { --accent: #10B6A3; }\n")
        (pack / "tokens" / "zz-pack-overrides.css").write_text(
            ":root, [data-theme=\"dark\"], [data-theme=\"light\"] {\n"
            "  --accent: #39FF6A !important;\n"
            "  --accent-soft: #39FF6A !important;\n"
            "  --accent-line: #39FF6A !important;\n"
            "}\n")
        return pack

    def test_the_override_block_wins_in_both_themes(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            pack = self.write_pack(Path(raw))
            for theme in ("dark", "light"):
                tokens = cph.tokens_from_pack(pack, theme)
                self.assertEqual(tokens["accent"], "#39FF6A", theme)
                self.assertEqual(tokens["accent-soft"], "#39FF6A", theme)
                found = cph.check_accent_ramp(tokens)[0]
                self.assertTrue(
                    any("--accent-soft is identical" in p for p in found),
                    f"{theme}: the override block was not resolved — {found}")

    def test_a_theme_exclusive_block_does_not_leak(self) -> None:
        # The other direction: a block scoped to light only must not colour the
        # dark resolution, or the resolver reports a palette nobody renders.
        with tempfile.TemporaryDirectory() as raw:
            pack = Path(raw) / "p"
            pack.mkdir()
            (pack / "t.css").write_text(
                ":root { --accent: #16DAC2; }\n"
                "[data-theme=\"light\"] { --accent: #10B6A3; }\n")
            self.assertEqual(cph.tokens_from_pack(pack, "dark")["accent"],
                             "#16DAC2")
            self.assertEqual(cph.tokens_from_pack(pack, "light")["accent"],
                             "#10B6A3")

    def test_var_indirection_is_resolved(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            pack = Path(raw) / "p"
            pack.mkdir()
            (pack / "t.css").write_text(
                ":root { --ink-signal: #16DAC2; --accent: var(--ink-signal); }\n")
            self.assertEqual(cph.tokens_from_pack(pack, "dark")["accent"],
                             "#16DAC2")


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


class ColourVision(unittest.TestCase):
    """The colour-vision lane, pinned in both directions.

    The bar sits in a narrow gap between two populations, so it is pinned
    against BOTH: palettes designed to survive colour blindness must pass,
    and the red/green pairs designers actually reach for must fail. A bar
    calibrated against only one side is how the first attempt at this check
    ended up rejecting the reference standard.
    """

    # Okabe-Ito, the canonical CVD-safe qualitative palette.
    OKABE_ITO = ("#E69F00", "#56B4E9", "#009E73", "#F0E442",
                 "#0072B2", "#D55E00", "#CC79A7")

    def worst(self, first: str, second: str) -> float:
        a, b = cph.parse_color(first), cph.parse_color(second)
        assert a is not None and b is not None
        return min(cph.delta_e(cph.simulate_cvd(a, kind),
                               cph.simulate_cvd(b, kind))
                   for kind in ("protan", "deutan"))

    def test_simulation_leaves_greys_alone(self) -> None:
        # Red-green deficiency is a hue confusion; it cannot move an
        # achromatic colour. If this drifts, the matrix is being applied in
        # the wrong space.
        grey = cph.parse_color("#808080")
        self.assertEqual(cph.simulate_cvd(grey, "protan"), grey)
        self.assertEqual(cph.simulate_cvd(grey, "deutan"), grey)

    def test_zero_severity_is_the_identity(self) -> None:
        red = cph.parse_color("#d32f2f")
        self.assertEqual(cph.simulate_cvd(red, "protan", 0.0), red)

    def test_simulation_preserves_alpha(self) -> None:
        self.assertEqual(cph.simulate_cvd((211, 47, 47, 0.5), "deutan")[3], 0.5)

    def test_protanopia_darkens_red(self) -> None:
        # The physical signature of a missing L cone: red loses luminance.
        red = cph.parse_color("#ff0000")
        self.assertLess(cph.simulate_cvd(red, "protan")[0], red[0])

    def test_delta_e_is_zero_for_identical_colours(self) -> None:
        c = cph.parse_color("#3FCF77")
        self.assertAlmostEqual(cph.delta_e(c, c), 0.0, places=6)

    def test_a_designed_cvd_safe_palette_clears_the_bar(self) -> None:
        # The positive control. A bar that rejects Okabe-Ito is not
        # calibrated, whatever else it catches.
        worst = min(self.worst(a, b)
                    for i, a in enumerate(self.OKABE_ITO)
                    for b in self.OKABE_ITO[i + 1:])
        self.assertGreaterEqual(worst, cph.CVD_DELTA_E_BAR,
                                f"Okabe-Ito worst pair {worst:.1f} is under "
                                f"the {cph.CVD_DELTA_E_BAR} bar")

    def test_traffic_light_pairs_are_caught(self) -> None:
        # The negative control, using the red/green pairs real products ship.
        for first, second, name in (("#d32f2f", "#2e7d32", "Material 700/800"),
                                    ("#e53935", "#43a047", "Material 600/600"),
                                    ("#ff3b30", "#34c759", "iOS system")):
            with self.subTest(name):
                self.assertLess(self.worst(first, second), cph.CVD_DELTA_E_BAR)

    def test_a_pair_separated_by_lightness_survives(self) -> None:
        # Pure red against pure green passes, and should: after simulation
        # they differ enormously in LIGHTNESS, which no red-green deficiency
        # removes. Pinning this stops someone "fixing" the check to fail
        # every red/green pair on principle.
        self.assertGreaterEqual(self.worst("#ff0000", "#00ff00"),
                                cph.CVD_DELTA_E_BAR)

    def test_a_hue_only_palette_is_reported(self) -> None:
        tokens = dict(HEALTHY)
        tokens.update({"success": "#2e7d32", "danger": "#d32f2f",
                       "warning": "#f9a825", "info": "#1565c0"})
        found, _ = cph.check_status_cvd(tokens)
        self.assertTrue(any("success" in p and "danger" in p for p in found),
                        found)

    def test_the_lane_is_advisory_unless_promoted(self) -> None:
        # The contract the CLI depends on: the colour-vision check is in
        # CHECKS but not in ENFORCING_CHECKS, so a finding reports rather
        # than blocking until --strict-cvd.
        self.assertIn(cph.check_status_cvd, cph.CHECKS)
        self.assertNotIn(cph.check_status_cvd, cph.ENFORCING_CHECKS)

    def test_every_check_is_routed(self) -> None:
        # The drift this file already warned about: a new check must land in
        # exactly one lane, never in neither.
        self.assertEqual(set(cph.CHECKS),
                         set(cph.ENFORCING_CHECKS) | set(cph.ADVISORY_CHECKS))


class ShippedPack(unittest.TestCase):
    """The pack in this repo must satisfy the bars this checker states.

    A checker whose own repo fails it is a checker nobody can turn on.
    """

    PACK = (Path(__file__).resolve().parents[2] / "assets" / "design-system"
            / "ink-signal" / "tokens" / "css")

    def test_ink_signal_passes_every_assertion_in_both_themes(self) -> None:
        # The whole checker, not just the text bars. Asserting one family here
        # leaves the others free to regress in the pack this repo ships: a
        # negative control that re-collapsed the accent ramp left this suite
        # green until the pack itself was put under the full check.
        for theme in ("dark", "light"):
            tokens = cph.tokens_from_pack(self.PACK, theme)
            self.assertIn("accent", tokens, theme)
            self.assertEqual(problems_of(tokens), [], f"{theme} theme")

    def test_ink_signal_dark_and_light_clear_the_bars(self) -> None:
        css = (self.PACK / "semantic.css").read_text()
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
