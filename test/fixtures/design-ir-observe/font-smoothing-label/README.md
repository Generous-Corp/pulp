# Font-smoothing label fixture

`source.html` sets `-webkit-font-smoothing: antialiased` on `html, body` and
draws Helvetica Neue "SPECTR" at 700 and "Sweep" at 400, light on dark, at tops
13.5 and 64 CSS px. Both weights are faces Helvetica Neue ships, so the face
never depends on weight matching.

- `design.ir.json` and `assets/` are the native import of that page. Every text
  node carries `font_smoothing: antialiased`.
- `chromium.png` is the Chromium capture of the page at DPR 2.

Regenerate by importing `source.html` with the importer differential lab at a
420x120 viewport. Captured with Chrome 155.0.8059.85 on macOS.
