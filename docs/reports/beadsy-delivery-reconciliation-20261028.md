# Beadsy delivery reconciliation

This report records the durable engineering disposition for the independently
branded Beadsy instrument. It is intentionally free of private worktree,
credential, session, and queue details; transient delivery state belongs in the
planning handoff and authenticated Shipyard receipts.

The implementation work is incomplete and remains unapproved for release. The
current product evidence establishes a green local Release test suite (56 of 56
cases), a finite audible headless render with state restoration, and a direct
reference comparison covering 144 deterministic Beads DSP fixtures with
byte-identical output, zero maximum absolute error, and zero RMSE. The latter
is an internal source-versus-port proof and does not by itself prove adapter or
DAW behavior.

The declared formats currently have build artifacts for AU, AUv3, VST3, CLAP,
LV2, and standalone. AAX remains unavailable because its SDK, entry point, and
validator are not present. Format artifact presence is not host validation.

The remaining acceptance gates are:

- a source-authority visual comparison proving the straight-on faceplate,
  control geometry, proportional resizing, and source-picker presentation;
- a real host receipt proving independent left/right route selection from the
  host-provided project, bus, instrument, or physical-input catalog;
- current AU, VST3, CLAP, and LV2 host validation, plus an explicit AAX scope
  decision;
- an exact-head signed, notarized, stapled combined installer receipt;
- first-class Illustrator/SVG import and Vellum adoption evidence under the
  recorded ownership boundary; and
- an exact-head adversarial review and authenticated Shipyard authorization.

Pulp-owned audio, format, host, attribution, and proof changes may proceed from
fresh branches based on current `origin/main`. Generic transferred rendering,
canvas, screenshot, and design-schema work must follow the Pulp/Vellum routing
protocol and must not be inferred from historical local branches or old
receipts. No release or completion claim should be made until every declared
gate has current evidence.
