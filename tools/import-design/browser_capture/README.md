# Browser capture helper

This helper evaluates an authorized HTML document in an isolated Chromium
process and writes a replayable `pulp-browser-capture-v1` envelope. It is an
import-time tool; generated Pulp plugins do not embed or require Chromium.

The C++ launcher in `browser_capture_backend.{hpp,cpp}` is the supported entry
point. It discovers and probes a compatible browser, creates a fresh temporary
profile, and invokes `capture.mjs` through Pulp's argv-safe `ChildProcess`.
`capture.mjs` uses only Node.js built-ins and requires Node.js 22 or newer.

The caller supplies an `input_file` inside an authorized `staged_root`. The
helper serves that root from a random tokenized loopback URL, so relative
scripts, styles, fonts, and images continue to work. Each served path is
realpath-checked against the root, including symlinks. External requests are
blocked through CDP unless the caller explicitly enables them.

Successful output is:

```text
<output>/
  capture.json
  browser.png
  dom-snapshot.json
  semantic-report.json
  tokens.json
```

`capture.json` conforms to `capture_protocol.json`. The DOM snapshot is kept as
a sidecar because it can be large; the envelope references it by relative path.
`tokens.json` preserves active light / no-preference computed CSS custom
properties as colors, true
pixel dimensions, or strings. Relative units and expressions are never coerced
to pixels. Selector-specific theme overrides are not promoted into the current
single-mode DesignIR token map; the authored HTML/CSS remains the source for
that follow-up rather than pretending a computed default is multi-theme.
Loopback ports, random tokens, browser executable paths, and host filesystem
paths are not recorded in the envelope.

Failures are nonzero and write `capture-error.json` when an output directory is
available. The helper never selects a lower-fidelity importer.
