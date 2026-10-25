"""Write source.html for the font-subset label fixture: one family split into
two unicode-range subsets, the Latin Extended rule first and the basic Latin
rule second, served from Blob URLs by a DOMParser loader.

  make_subset_page.py <latinext.ttf> <latin.ttf> <out.html>
"""
import base64
import sys
from pathlib import Path

FAMILY = "Pulp Subset Probe"
INNER = """<!doctype html>
<html><head><meta charset="utf-8"><style>
@font-face{font-family:'%(f)s';src:url(__EXT__) format('truetype');font-weight:300 800;unicode-range:U+0100-024F}
@font-face{font-family:'%(f)s';src:url(__LATIN__) format('truetype');font-weight:300 800;unicode-range:U+0000-00FF}
html,body{margin:0;background:#0d1117}
#panel{position:relative;width:420px;height:120px;background:#0d1117}
.label{position:absolute;font:600 15px '%(f)s',sans-serif;color:#e6edf3;line-height:20px}
.light{position:absolute;font:400 15px '%(f)s',sans-serif;color:#e6edf3;line-height:20px}
</style></head><body>
<div id="panel">
  <div class="label" id="label-half" style="left:60px;top:13.5px">SPECTR</div>
  <div class="light" id="light-half" style="left:220px;top:13.5px">Sweep</div>
  <div class="label" id="label-whole" style="left:60px;top:64px">SPECTR</div>
  <div class="light" id="light-whole" style="left:220px;top:64px">Sweep</div>
</div>
</body></html>"""


def main() -> None:
    ext = base64.b64encode(Path(sys.argv[1]).read_bytes()).decode("ascii")
    latin = base64.b64encode(Path(sys.argv[2]).read_bytes()).decode("ascii")
    inner = INNER % {"f": FAMILY}
    page = f"""<!doctype html>
<html><head><meta charset="utf-8"><title>Subset probe</title></head><body>
<script>
(() => {{
  const blob = (b64) => URL.createObjectURL(new Blob(
      [Uint8Array.from(atob(b64), (c) => c.charCodeAt(0))], {{ type: "font/ttf" }}));
  const html = `{inner}`.split("__EXT__").join(blob("{ext}")).split("__LATIN__").join(blob("{latin}"));
  document.documentElement.replaceWith(new DOMParser().parseFromString(html, "text/html").documentElement);
}})();
</script>
</body></html>
"""
    Path(sys.argv[3]).write_text(page, encoding="utf-8")


if __name__ == "__main__":
    main()
