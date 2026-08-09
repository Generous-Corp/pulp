#!/usr/bin/env python3
"""Fail closed when Forge's semantic catalog and export registry drift.

This is deliberately a family-level gate, not a pack-level grep.  One pack can
contain many independently selectable nodes (the drum and lo-fi packs are the
obvious examples), so finding one descriptor function in each indexed header
proves almost nothing.

The manifest below is the independent, canonical list of semantic node keys.
The runtime audit proves that each registered descriptor/factory pair agrees
with its baked DSP contract.  This static gate proves that:

* the catalog index contains exactly the packs represented by the manifest;
* every canonical key is authored in its owning descriptor source;
* the export's independent expected-key list exactly matches this manifest; and
* the export registry has one explicit registration per canonical key.

No built binary is executed here, which keeps the gate usable in source-only
and cross-compilation checks.
"""

from __future__ import annotations

import argparse
import collections
import re
import sys
from pathlib import Path

HOST_INCLUDE = Path("core/host/include/pulp/host")
INDEX = HOST_INCLUDE / "forge_catalog_index.hpp"
EXPORT = Path("core/host/src/forge_catalog_export.cpp")

# Each tuple is (indexed pack, descriptor source, semantic node keys).  A detail
# header is allowed as the descriptor source, but ownership remains with its
# indexed public pack.
CATALOG = (
    ("forge_analog_vcf_catalog.hpp", "forge_analog_vcf_catalog.hpp",
     ("analog_vcf",)),
    ("forge_character_delay_catalog.hpp", "forge_character_delay_catalog.hpp",
     ("character_delay",)),
    ("forge_distortion_catalog.hpp", "forge_distortion_catalog.hpp",
     ("distortion",)),
    ("forge_drum_catalog.hpp", "detail/forge_drum_catalog_descriptor.hpp",
     ("drum_kick_oscillator", "drum_kick_resonant", "drum_kick_circuit",
      "drum_snare", "drum_hat", "drum_clap", "drum_tom_generic",
      "drum_tom_simmons", "drum_cymbal", "drum_membrane", "drum_string",
      "drum_zap", "drum_fm2", "drum_fm6", "drum_fm8")),
    ("forge_dynamics_catalog.hpp", "forge_dynamics_catalog.hpp",
     ("feedforward_compressor", "true_peak_limiter", "vca_compressor", "fet_compressor",
      "diode_bridge_compressor")),
    ("forge_effect_modulation_catalog.hpp",
     "detail/forge_effect_modulation_catalog_descriptor.hpp",
     ("frequency_shifter", "chorus", "phaser_stages", "delay_vibrato",
      "phase_vibrato", "univibe")),
    ("forge_effect_modulation_catalog.hpp",
     "detail/forge_effect_modulation_extended_catalog.hpp",
     ("flanger", "leslie", "scanner_vibrato")),
    ("forge_fdn_reverb_catalog.hpp", "forge_fdn_reverb_catalog.hpp",
     ("fdn_reverb",)),
    ("forge_fuzz_catalog.hpp", "forge_fuzz_catalog.hpp", ("fuzz",)),
    ("forge_lofi_catalog.hpp", "detail/forge_lofi_catalog_descriptor.hpp",
     ("lofi_delay", "lofi_filter", "lofi_waveshaper", "lofi_drywet",
      "lofi_noise", "lofi_bitcrush", "lofi_trim", "lofi_ping_pong",
      "lofi_reverb", "lofi_compressor", "lofi_gate", "lofi_lfo", "lofi_vca",
      "lofi_env_follower", "lofi_filter_cv", "lofi_delay_cv", "lofi_auto_pan",
      "lofi_width", "lofi_phaser")),
    ("forge_modulation_catalog.hpp", "forge_modulation_catalog.hpp",
     ("mod_lfo", "mod_lpg", "mod_slew", "mod_transient", "mod_env")),
    ("forge_multiband_catalog.hpp", "forge_multiband_catalog.hpp",
     ("multiband_compressor",)),
    ("forge_pitch_catalog.hpp", "forge_pitch_catalog.hpp",
     ("whammy", "harmony_engine")),
    ("forge_saturator_catalog.hpp", "forge_saturator_catalog.hpp",
     ("saturator",)),
    ("forge_sequencing_catalog.hpp", "forge_sequencing_catalog.hpp",
     ("stage_seq", "cartesian_walk", "rungler", "quantize_scale", "gate_logic",
      "prob_gate")),
    ("forge_sidechain_catalog.hpp", "forge_sidechain_catalog.hpp",
     ("sidechain_compressor",)),
    ("forge_space_catalog.hpp", "forge_space_catalog.hpp",
     ("convolution_reverb", "nonlin_ambience", "speaker_cabinet")),
    ("forge_synthesis_catalog.hpp", "forge_synthesis_catalog.hpp",
     ("additive_bank", "vocoder", "cyclic_stretch", "granular_live")),
    ("forge_tape_catalog.hpp", "forge_tape_catalog.hpp", ("tape_machine",)),
    ("forge_wavetable_catalog.hpp", "forge_wavetable_catalog.hpp",
     ("wavetable_oscillator",)),
)

INCLUDE_RE = re.compile(
    r'^\s*#include\s+<pulp/host/(forge_[a-z0-9_]+_catalog\.hpp)>', re.M)
EXPECTED_KEYS_RE = re.compile(
    r"expected_node_keys\s*\(\s*\)\s*\{(?P<body>.*?)return\s+keys\s*;",
    re.S,
)
STRING_RE = re.compile(r'"([^"]+)"')
ADD_RE = re.compile(r"(?m)^\s*add\s*\(")
ADD_DRUM_RE = re.compile(r"(?m)^\s*add_drum\s*\(")


def indexed_packs(root: Path) -> list[str]:
    return INCLUDE_RE.findall((root / INDEX).read_text(encoding="utf-8"))


def canonical_keys() -> list[str]:
    return [key for _, _, keys in CATALOG for key in keys]


def report_set_diff(label: str, actual: set[str], expected: set[str]) -> bool:
    ok = True
    missing = sorted(expected - actual)
    unexpected = sorted(actual - expected)
    if missing:
        ok = False
        print(f"\n  {label} missing canonical entries:", file=sys.stderr)
        for item in missing:
            print(f"    {item}", file=sys.stderr)
    if unexpected:
        ok = False
        print(f"\n  {label} has unexpected entries:", file=sys.stderr)
        for item in unexpected:
            print(f"    {item}", file=sys.stderr)
    return ok


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=".", help="repository root")
    args = ap.parse_args()
    root = Path(args.root).resolve()

    required = (INDEX, EXPORT)
    absent = [str(path) for path in required if not (root / path).is_file()]
    if absent:
        for path in absent:
            print(f"forge-descriptor-coverage: cannot read {path}", file=sys.stderr)
        return 2

    keys = canonical_keys()
    key_counts = collections.Counter(keys)
    duplicate_manifest_keys = sorted(key for key, count in key_counts.items() if count != 1)
    if duplicate_manifest_keys:
        print("forge-descriptor-coverage: duplicate canonical keys:", file=sys.stderr)
        for key in duplicate_manifest_keys:
            print(f"  {key}", file=sys.stderr)
        return 2

    manifest_packs = {pack for pack, _, _ in CATALOG}
    packs = indexed_packs(root)
    ok = report_set_diff("catalog index", set(packs), manifest_packs)
    if len(packs) != len(set(packs)):
        ok = False
        print("\n  catalog index contains duplicate includes", file=sys.stderr)

    for pack, descriptor_source, owned_keys in CATALOG:
        path = root / HOST_INCLUDE / descriptor_source
        if not path.is_file():
            ok = False
            print(f"\n  descriptor source does not exist: {descriptor_source}",
                  file=sys.stderr)
            continue
        text = path.read_text(encoding="utf-8")
        for key in owned_keys:
            if not re.search(rf'"{re.escape(key)}"', text):
                ok = False
                print(f"\n  {pack} does not author canonical key {key!r} "
                      f"in {descriptor_source}", file=sys.stderr)

    export_text = (root / EXPORT).read_text(encoding="utf-8")
    expected_match = EXPECTED_KEYS_RE.search(export_text)
    if expected_match is None:
        ok = False
        export_keys: list[str] = []
        print("\n  export has no parseable independent expected_node_keys() list",
              file=sys.stderr)
    else:
        export_keys = STRING_RE.findall(expected_match.group("body"))
        ok = report_set_diff("export expected-node list", set(export_keys), set(keys)) and ok
        if len(export_keys) != len(set(export_keys)):
            ok = False
            print("\n  export expected-node list contains duplicates", file=sys.stderr)

    direct_registrations = len(ADD_RE.findall(export_text))
    drum_registrations = len(ADD_DRUM_RE.findall(export_text))
    # The parameterized drum family uses one small helper so every EngineId is
    # still an explicit source registration without repeating the descriptor /
    # factory join. Its helper body contains one `add(` call, which is a
    # template for the calls rather than an additional node.
    if "const auto add_drum" in export_text:
        direct_registrations -= 1
    registrations = direct_registrations + drum_registrations
    if registrations != len(keys):
        ok = False
        print(f"\n  export registry has {registrations} add(...) registrations; "
              f"canonical manifest requires exactly {len(keys)}", file=sys.stderr)

    print(f"forge-descriptor-coverage: {len(keys)} canonical families, "
          f"{len(manifest_packs)} indexed packs, {registrations} export registrations")
    if not ok:
        print("\nforge-descriptor-coverage: FAILED", file=sys.stderr)
        return 1

    print("forge-descriptor-coverage: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
