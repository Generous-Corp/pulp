#!/usr/bin/env python3
"""Codified HTML->Figma import: per-component standalone page generator.

Reads ds_dump.json (the design-system inline CSS + every .card's reconstructed
outerHTML, captured once from the bundled `Pulp Components.html`). Emits a clean,
self-contained standalone HTML page for one card in one theme, suitable for a
pixel-perfect `generate_figma_design` capture.

The design system is theme-adaptive via `data-theme="dark"|"light"` on <html>
(see the folder readme/SKILL), so the SAME card HTML + CSS yields both themes by
flipping that one attribute. Fonts are embedded as base64 @font-face in the CSS,
so the page needs no network.

Usage:
  gen_standalone.py <card_index> <dark|light> <out.html>
"""
import json, sys, html, pathlib

DUMP = pathlib.Path(__file__).with_name("ds_dump.json")

def slug(t):
    import re
    return re.sub(r'[^a-z0-9]+', '-', t.lower()).strip('-')[:48]

def main():
    idx = int(sys.argv[1]); theme = sys.argv[2]; out = sys.argv[3]
    d = json.load(open(DUMP))
    card = d["cards"][idx]
    knob_fix = (pathlib.Path(__file__).with_name("capture_prep.js")).read_text()
    page = f"""<!doctype html>
<html lang="en" data-theme="{theme}">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{html.escape(card['title'])} ({theme})</title>
<script src="https://mcp.figma.com/mcp/html-to-design/capture.js" async></script>
<style>{d['css']}</style>
<style>
  /* capture frame: pad the card in the app surface, hug its natural size */
  html,body {{ margin:0; }}
  body.pulp-root {{ background:var(--surface-app); padding:24px; display:inline-block; }}
  /* pin the card to the width it had in the master specimen page so its
     internal flex/grid layout reflows identically (1:1 with the source). */
  .capture-card {{ display:inline-block; width:{card['w']}px; }}
  .capture-card > .card {{ width:100%; box-sizing:border-box; }}
</style>
</head>
<body class="pulp-root">
<div class="capture-card">{card['html']}</div>
<script>{knob_fix}</script>
</body>
</html>
"""
    pathlib.Path(out).write_text(page, encoding="utf-8")
    print(f"wrote {out}  ({card['title'][:50]} / {theme}) {card['w']}x{card['h']}")

if __name__ == "__main__":
    main()
