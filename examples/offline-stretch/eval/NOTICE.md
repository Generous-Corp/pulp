# Third-Party Notices — Stretch A/B Eval Toolkit

This toolkit is part of Pulp (MIT). It uses only permissively-licensed Python
packages, declared in `requirements.txt`:

| Package | License | Use |
|---------|---------|-----|
| numpy | BSD-3-Clause | numerics / FFT |
| soundfile | BSD-3-Clause | WAV I/O (wraps libsndfile) |

`soundfile` dynamically links **libsndfile** (LGPL-2.1). It is not bundled or
modified here; it is installed by the user via pip/their OS and loaded at runtime.
No LGPL code is included in this repository or in Pulp's shipped artifacts.

## Explicitly NOT bundled (copyleft — kept out of the MIT tree)

- **Rubber Band Library** (GPL-2.0) — a common time-stretch reference. It is NOT
  included, linked, or invoked by this toolkit. To compare against it, install
  your own `rubberband` binary under your own license, render a reference file,
  and pass it to `ab_compare.py --reference <file>`. The GPL tool stays entirely
  on your side of the boundary.

Keeping GPL/AGPL dependencies out of the MIT core is deliberate; this toolkit
only measures audio you provide and audio Pulp's own (MIT) engine produces.
