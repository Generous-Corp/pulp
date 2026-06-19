# Stretch A/B Eval Toolkit

A small, agent-runnable toolkit for measuring and comparing Pulp's offline
time-stretch / pitch renders — the loop used to tune the engine, packaged so you
(or your agent) can dial in a preset to taste.

## Install

```bash
pip install -r requirements.txt   # numpy + soundfile (both permissive)
```

You also need a built `stretchcli` (from this example):

```bash
cmake -S <pulp-root> -B build -DCMAKE_BUILD_TYPE=Release -DPULP_ENABLE_GPU=OFF
cmake --build build --target stretchcli -j
```

## Use

Quality metrics for any render(s):

```bash
python stretch_metrics.py out1.wav out2.wav
```

A/B configs/presets across ratios:

```bash
python ab_compare.py drum.wav \
  --cli ../../../build/examples/offline-stretch/stretchcli \
  --ratios 0.75,1.5,2.0 \
  --configs "clean:--character clean" "tape:--character varispeed"
```

Diff a render against a reference file you supply:

```bash
python ab_compare.py vocal.wav --ratios 2.0 \
  --configs "ours:--character clean" --reference my_reference_2x.wav
```

## Metrics (and how to read them)

| metric | what | how to read |
|--------|------|-------------|
| `centroid` | spectral brightness | the most reliable muddy-vs-clear signal |
| `onset` | attack punch | higher = sharper transients |
| `peak_hz` | dominant low partial | must EQUAL the source for a faithful stretch (no pitch shift) |
| `wobble` | low-freq pitch instability (std Hz) | lower = steadier; high = "wobbly when hit" |
| `spectral_l1` | LTAS-shape distance | engine-vs-reference; read with `band_balance`, not alone |
| `band_balance` | per-band energy | the trustworthy "EQ match" check |

**Metrics are necessary, not sufficient.** They repeatedly misled us on subtle
perceptual artifacts during development — always confirm by ear. Use them to
iterate fast and to catch regressions, not to declare victory.

## Workflow for dialing in a preset

1. A/B a few `--character`/`--fft`/`--transient-sens` configs with `ab_compare.py`.
2. Listen — pick the one that sounds best.
3. Save it: `stretchcli in.wav out.wav <your flags> --save-preset my.preset`.
4. Share `my.preset` (a tiny text file). Others load it with `--preset my.preset`.
