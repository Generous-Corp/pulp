# Audio Quality Lab — Third-Party Notices

The Audio Quality Lab is an **optional, developer/CI-only** tool. It is never linked
into the Pulp MIT core or any shipped plugin/app binary, and is never required by
public CI.

## In-tree dependencies (Tier 1, permissive)

| Name | License | Use |
|------|---------|-----|
| NumPy | BSD-3-Clause | array math / FFT for the artifact detectors |
| soundfile (libsndfile) | BSD-3-Clause (lib: LGPL, used via the package, not linked into Pulp) | WAV I/O in the dev tool |

## Optional dev-only tools (NOT bundled, developer-supplied)

These are reached only across a process boundary via an explicit env-path; none is
committed, vendored, or required, and each degrades to `skipped` independently when its
env-path is unset (so a developer opts into any subset simply by which env-paths they
set):

| Name | License | Env-path | Role |
|------|---------|----------|------|
| ViSQOL | Apache-2.0 | `PULP_VISQOL_BIN` | full-reference perceptual MOS-LQO (Layer B) |
| PEAQ (GstPEAQ / PeaqB) | GPL | `PULP_PEAQ_BIN` | full-reference perceptual ODG (Layer B), copyleft-fenced |
| AQUA-Tk | GPL-3.0 | `PULP_AQUATK_BIN` | full-reference perceptual ODG (Layer B), copyleft-fenced |
| aubio | GPL-3.0 | `PULP_AUBIO_BIN` | MIR structural oracle (onset cross-check, advisory) — a feature extractor, NOT a perceptual metric; copyleft-fenced |
| Rubber Band R3 | GPL | — | benchmark reference only, developer-supplied |

No GPL code is compiled or linked into any Pulp artifact, and no committed baseline
depends on a copyleft tool. See the plan's §4 license architecture.
