# Browser capture fixture: font smoothing

A real Chrome capture of `panel.html`. It pins what the capture records for
CSS `-webkit-font-smoothing` and what the lowering keeps of it.

| element | smoothing | lowered `font_smoothing` |
|---|---|---|
| `#smooth` "Antialiased" | `antialiased` | `antialiased` |
| `#default` "Default" | `auto` (unset) | absent |
| `#subpixel` "Subpixel" | `subpixel-antialiased` | absent (rasterizes as `auto` on macOS) |

## Regenerating

```sh
STAGE=$(mktemp -d)
cp test/fixtures/browser-capture-font-smoothing/panel.html "$STAGE"/
node tools/import-design/browser_capture/capture.mjs capture \
  --browser "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" \
  --input "$STAGE/panel.html" --root "$STAGE" \
  --output test/fixtures/browser-capture-font-smoothing \
  --initial-width 320 --initial-height 80 --dpr 2 --timeout-ms 120000
```

Captured with Chrome 155.0.8059.85 on macOS.
