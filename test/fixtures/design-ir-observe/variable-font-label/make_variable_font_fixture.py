"""Write source.html for the variable-font label fixture.

The page is loader-shaped like a Claude standalone export: a script turns an
embedded font into a Blob URL, builds the real document with DOMParser and
swaps it in with replaceWith, so the browser capture packages the @font-face
rule and the font bytes in its materialized document.

  make_variable_font_fixture.py <font.ttf> <out source.html>
"""
import base64
import sys
from pathlib import Path

FAMILY = "Pulp Captured Variable"

INNER = """<!doctype html>
<html><head><meta charset="utf-8"><style>
@font-face{font-family:'%(family)s';src:url(%(url)s) format('truetype');font-weight:300 800;font-style:normal}
html,body{margin:0;background:#0d1117}
#panel{position:relative;width:420px;height:120px;background:#0d1117}
.label{position:absolute;font:600 15px '%(family)s',sans-serif;color:#e6edf3;line-height:20px}
.light{position:absolute;font:400 15px '%(family)s',sans-serif;color:#e6edf3;line-height:20px}
</style></head><body>
<div id="panel">
  <div class="label" id="label-half" style="left:60px;top:13.5px">SPECTR</div>
  <div class="light" id="light-half" style="left:220px;top:13.5px">Sweep</div>
  <div class="label" id="label-whole" style="left:60px;top:64px">SPECTR</div>
  <div class="light" id="light-whole" style="left:220px;top:64px">Sweep</div>
</div>
</body></html>"""


def main() -> None:
    font = Path(sys.argv[1]).read_bytes()
    encoded = base64.b64encode(font).decode("ascii")
    inner = INNER % {"family": FAMILY, "url": "__FONT_URL__"}
    # The inner document is a JS string; its only quote characters are single
    # quotes, so a backtick template literal carries it untouched.
    assert "`" not in inner and "${" not in inner
    page = f"""<!doctype html>
<html><head><meta charset="utf-8"><title>Variable font label</title></head>
<body>
<script>
(() => {{
  const bytes = Uint8Array.from(atob("{encoded}"), (c) => c.charCodeAt(0));
  const url = URL.createObjectURL(new Blob([bytes], {{ type: "font/ttf" }}));
  const html = `{inner}`.split("__FONT_URL__").join(url);
  const parsed = new DOMParser().parseFromString(html, "text/html");
  document.documentElement.replaceWith(parsed.documentElement);
}})();
</script>
</body></html>
"""
    Path(sys.argv[2]).write_text(page, encoding="utf-8")


if __name__ == "__main__":
    main()
