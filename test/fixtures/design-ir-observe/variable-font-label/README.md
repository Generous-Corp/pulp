# Variable-font label fixture

`source.html` serves Funnel Display Variable from a Blob URL under the family
`Pulp Captured Variable`, which no host or bundle has. A loader script builds the
real page with `DOMParser`, the way a Claude standalone export does. It draws
"SPECTR" at weight 600 and "Sweep" at 400, at tops 13.5 and 64 CSS px.

- `design.ir.json` and `assets/` are the native import of that page. The import
  carries the captured font as `assets/b4151c9c...d6c1179d.ttf`.
- `chromium.png` is the Chromium capture of the page at DPR 2.

The font is byte-identical to `external/fonts/FunnelDisplay-VariableFont_wght.ttf`
(SIL Open Font License 1.1; see `external/fonts/README.md`). Regenerate
`source.html` from the repository root with
`python3 test/fixtures/design-ir-observe/variable-font-label/make_variable_font_fixture.py external/fonts/FunnelDisplay-VariableFont_wght.ttf test/fixtures/design-ir-observe/variable-font-label/source.html`,
then import it with the importer differential lab at a 420x120 viewport.
