# Font-subset label fixture

One family, `Pulp Subset Probe`, is split into two `@font-face` rules with
`unicode-range`, as a web font service serves a family. The first rule is a
Latin Extended subset (U+0100-024F) with no basic Latin glyphs. The second is
the basic Latin subset (U+0000-00FF). Both are cut from Funnel Display Variable
and keep its `wght` axis. The page draws "SPECTR" at 600 and "Sweep" at 400 at
tops 13.5 and 64 CSS px, so every glyph comes from the second rule.

A renderer that registers both files under the plain family lets the first one
stand in for the family, and the text falls back to a host face. Registering
each subset under its own alias, last-defined first, gives Chromium's result.

- `design.ir.json` and `assets/` are the native import of `source.html`.
- `ui.js` is the generated script for the same import (`--emit js`); it reads
  the same `assets/`.
- `chromium.png` is the Chromium capture of the page at DPR 2.

The subsets are byte-identical to what fontTools produces from
`external/fonts/FunnelDisplay-VariableFont_wght.ttf` (SIL Open Font License 1.1;
see `external/fonts/README.md`):

```sh
python3 -m fontTools.subset FunnelDisplay-VariableFont_wght.ttf --unicodes="U+0000-00FF" --output-file=latin.ttf
python3 -m fontTools.subset FunnelDisplay-VariableFont_wght.ttf --unicodes="U+0100-024F" --output-file=latinext.ttf
python3 make_subset_page.py latinext.ttf latin.ttf source.html
```

Then import `source.html` with the importer differential lab at a 420x120
viewport (`design.ir.json`, `assets/`, `chromium.png`) and with
`pulp-import-design --from claude --file source.html --emit js
--native-panel-lowering --render-size 420x120` (`ui.js`). Captured with Chrome
155.0.8059.85 on macOS.
