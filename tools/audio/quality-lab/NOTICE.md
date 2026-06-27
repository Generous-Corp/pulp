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

These are reached only across a process boundary via an explicit env-path
(`PULP_VISQOL_BIN`, `PULP_PEAQ_BIN`, …); none is committed, vendored, or required:

| Name | License | Tier |
|------|---------|------|
| ViSQOL | Apache-2.0 | opt-in permissive (Layer B) |
| PEAQ (GstPEAQ / PeaqB) | GPL | copyleft-fenced, developer-supplied |
| AQUA-Tk | GPL-3.0 | copyleft-fenced, developer-supplied |
| Rubber Band R3 | GPL | benchmark reference only, developer-supplied |

No GPL code is compiled or linked into any Pulp artifact, and no committed baseline
depends on a copyleft tool. See the plan's §4 license architecture.
