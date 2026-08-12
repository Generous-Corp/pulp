# Browser capture helper

This helper evaluates an authorized HTML document in an isolated Chromium
process and writes a replayable `pulp-browser-capture-v1` envelope. It is an
import-time tool; generated Pulp plugins do not embed or require Chromium.

The C++ launcher in `browser_capture_backend.{hpp,cpp}` is the supported entry
point. It discovers and probes a compatible browser, creates a fresh temporary
profile, and invokes `capture.mjs` through Pulp's argv-safe `ChildProcess`.
`capture.mjs` uses only Node.js built-ins and requires Node.js 22 or newer.
The public CLI opt-in is `--allow-browser-network`; the helper also accepts that
spelling when invoked directly by importer developers, while the launcher uses
its internal `--allow-network` spelling.

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

`--interactions <plan.json>` optionally reaches one deterministic secondary
state before the same-frame evidence capture. Plans use
`pulp-browser-interactions-v1` and contain only bounded `click`,
`context-click`, `type`, `wait-for`, and `wait-ms` actions. `context-click`
uses a real secondary-button press/release. The helper records selectors and typed-text
length in `interaction-report.json`; it persists neither typed plaintext nor a
per-action text hash. The published plan identity hashes a canonical redacted
plan in which typed text is replaced by its length, so short private values
cannot be recovered by hashing candidate plans. Same-document history and
fragment routing are allowed; loading another document or opening a popup
remains forbidden. Typed text still becomes live rendered page state and may
therefore appear in screenshots, DOM/semantic evidence, or tokens. Never put a
password, credential, private draft, or other secret in an interaction plan.
Sources with a distinct asynchronous boundary after the last action may expose
`globalThis.__pulpInteractionReady` as a Promise or one-shot function; the
initial `__pulpCaptureReady` contract is never invoked twice.
Action timeouts remain inside the capture-wide
`--timeout-ms` deadline and cannot extend it. See
`interaction_plan_protocol.json` for the exact schema. With no plan, capture
retains its initial-state behavior and output set.

For a reproducible Forge Modular secondary-state proof, use
`test/fixtures/browser_capture_forge_modular_mentions.json` with the source
export and pixel validation:

```bash
pulp import-design \
  --file /path/to/ForgeModular.dc.html \
  --browser-interactions test/fixtures/browser_capture_forge_modular_mentions.json \
  --allow-browser-network \
  --emit ir-json \
  --output /tmp/forge-modular-proof.json \
  --validate \
  --screenshot-backend skia
```

The captured and Skia-rendered frames must both show the module mention picker,
and validation must report zero differing pixels.

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

With explicit network access, the launcher admits only public HTTPS origins
declared by the bounded staged source graph. Provider-owned secondary origins
must also be present in the audited dependency registry; currently this covers
Google Fonts stylesheet responses loading font bytes from `fonts.gstatic.com`.
All admitted hosts are resolved to public addresses and pinned before Chromium
launches, and every fetched response is content-hashed in the capture envelope.

Failures are nonzero and write `capture-error.json` when an output directory is
available. The helper never selects a lower-fidelity importer.

The resolved browser build is written to stderr as a `[browser-capture]` line
before any page work, so a capture that never produces an envelope still says
which Chromium ran. Chromium's screenshot and virtual-time behaviour differs
between releases, and a failure report that does not name the browser cannot be
triaged. A capture that reaches its deadline reports the phase it died in, the
last browser call that completed, and the calls still in flight; the same
detail is written to `capture-error.json`.

Arbitrary JavaScript evaluation is intentionally not a CLI escape hatch: it
would weaken the source/evidence boundary and make captures difficult to
reproduce or audit.
