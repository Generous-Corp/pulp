#!/usr/bin/env python3
"""Standalone page generator for the EYEBROW specimen family.

Some design-system components are not `.card` blocks but `<p class="lbl">Name —
desc</p>` eyebrows followed by a `.themepane` / `.grid` content block. This
generator reads ds_specimens.json (eyebrowSpecimens[]) and emits a self-contained
standalone page for one specimen + theme, same contract as gen_standalone.py
(inline CSS, data-theme, pinned width, capture_prep.js + capture.js).

Usage:
  gen_eyebrow.py <specimen_index> <dark|light> <out.html>
"""
import json, sys, html, pathlib

DUMP = pathlib.Path(__file__).with_name("ds_specimens.json")

def main():
    idx = int(sys.argv[1]); theme = sys.argv[2]; out = sys.argv[3]
    d = json.load(open(DUMP))
    spec = d["eyebrowSpecimens"][idx]
    knob_fix = (pathlib.Path(__file__).with_name("capture_prep.js")).read_text()
    page = f"""<!doctype html>
<html lang="en" data-theme="{theme}">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{html.escape(spec['name'])} ({theme})</title>
<script src="https://mcp.figma.com/mcp/html-to-design/capture.js" async></script>
<style>{d['css']}</style>
<style>
  html,body {{ margin:0; }}
  body.pulp-root {{ background:var(--surface-app); padding:24px; display:inline-block; }}
  .capture-card {{ display:inline-block; width:{spec['w']}px; }}
</style>
</head>
<body class="pulp-root">
<div class="capture-card">{spec['html']}</div>
<script>{knob_fix}</script>
</body>
</html>
"""
    pathlib.Path(out).write_text(page, encoding="utf-8")
    print(f"wrote {out}  ({spec['name']} / {theme}) {spec['w']}x{spec['h']}")

if __name__ == "__main__":
    main()
