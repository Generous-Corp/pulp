# browser-capture-font-face-url

A plain HTML page whose `@font-face` rules load fonts from files beside it
with `src: url(...)`. The capture has to carry those faces into DesignIR, with
their family, weight, style and unicode-range, because nothing else names the
bytes Chromium drew the text with.

- `fonts/Jost-Regular.ttf` and `fonts/Jost-Bold.ttf` are byte copies of
  `external/fonts/` (Jost 3.710, SIL Open Font License 1.1, `OFL.txt`; SHA-256
  in `external/fonts/README.md`). The page names them `Pulp Page Face`, a
  family no host or Pulp bundle has, so native text can only resolve them from
  the imported assets.
- The bold rule lists a `local()` face no host has before its unquoted
  `url()`, so the capture has to skip non-url sources and read every url()
  spelling.
- `Pulp Page Unused` is declared and never used. Chromium never fetches it, so
  the capture binds nothing for it and its missing file is not an error.
