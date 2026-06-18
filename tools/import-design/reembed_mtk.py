#!/usr/bin/env python3
"""Re-embed the Musical Typing Keyboard's two faithful Figma frames.

The MusicalTypingKeyboard is a bespoke TWO-frame catalog component (typing node
187:15, piano node 187:349) — make_catalog_component.py only does single-frame
ones, so this is its dedicated re-export → neutralize → re-embed lane.

Pipeline (faithful-vector):
  1. Fetch each frame as SVG via the Figma REST /images?format=svg endpoint.
  2. NEUTRALIZE the baked "selected keys shown" demo chord: the design bakes a
     few lit keys as #16DAC2 vertical gradients (0.26→1.0 opacity ramp) in the
     key-bed. The live momentary overlay owns all pressed-state lighting, so
     those baked gradients are restored to the resting key color — white keys
     (gradient y-extent ≥ 65) → #EBEEF1; black keys (< 65) → #3A3F47 (top) /
     #16191E (bottom). Toolbar/overview teal (solid #16DAC2, not a ramp) is
     left untouched. This is content-based (no byte offsets), so it survives
     Figma edits. Proven to reproduce the committed SVGs byte-exact.
  3. Emit core/view/src/musical_typing_keyboard_svg.cpp (chunked base64).

Usage:
  reembed_mtk.py [--token <T>] [--repo <path>] [--date YYYY-MM-DD]
  reembed_mtk.py --validate    # regenerate to a temp string, assert the decoded
                               # SVGs match the committed file; write nothing.

Token: --token, $FIGMA_TOKEN, or ~/.config/pulp/figma-token.
"""
import argparse, base64, json, os, re, sys, urllib.parse, urllib.request

FILE_KEY = "q9iDYZzg86YrOQKr6I3bY0"
TYPING_NODE = "187:15"
PIANO_NODE = "187:349"
CHUNK = 8000

_GRAD = re.compile(
    r'<linearGradient id="[^"]+"[^>]*y1="(?P<y1>[\d.]+)"[^>]*y2="(?P<y2>[\d.]+)"[^>]*>'
    r'(?P<body>.*?)</linearGradient>', re.S)


def neutralize(svg: str) -> str:
    """Restore baked lit-key gradients to resting colors (see module docstring)."""
    def repl(m):
        body = m.group("body")
        if body.count("#16DAC2") != 2 or "0.26" not in body:
            return m.group(0)             # toolbar/overview teal or other — keep
        white = abs(float(m.group("y2")) - float(m.group("y1"))) >= 65.0
        if white:
            body2 = body.replace("#16DAC2", "#EBEEF1")
        else:
            body2 = body.replace("#16DAC2", "#3A3F47", 1).replace("#16DAC2", "#16191E", 1)
        # Drop the lit gradient's 0.26-opacity top stop so the neutralized key is
        # SOLID — matching the resting keys (solid #EBEEF1 / #3A3F47→#16191E). The
        # leftover 0.26 made a was-lit key's top translucent over the dark bed, so
        # it read as gray (e.g. a gray E4) next to its solid neighbours.
        body2 = body2.replace(' stop-opacity="0.26"', '')
        s = m.start("body") - m.start()
        e = m.end("body") - m.start()
        return m.group(0)[:s] + body2 + m.group(0)[e:]
    return _GRAD.sub(repl, svg)


def fetch_svg(node: str, token: str) -> str:
    q = urllib.parse.quote(node)
    url = f"https://api.figma.com/v1/images/{FILE_KEY}?ids={q}&format=svg"
    req = urllib.request.Request(url, headers={"X-Figma-Token": token})
    img = (json.load(urllib.request.urlopen(req)).get("images", {}) or {}).get(node)
    if not img:
        sys.exit(f"no SVG render for node {node}")
    return urllib.request.urlopen(img).read().decode("utf-8", "replace")


def chunk_fn(name: str, svg: str) -> str:
    b64 = base64.b64encode(svg.encode()).decode()
    parts = [b64[i:i + CHUNK] for i in range(0, len(b64), CHUNK)]
    arr = ",\n".join('    "%s"' % p for p in parts)
    return (f"const char* {name}() {{\n"
            f"  static const char* const kParts[] = {{\n{arr}\n  }};\n"
            f"  static const std::string kJoined = [] {{\n"
            f"    std::string s;\n"
            f"    for (const char* p : kParts) s += p;\n"
            f"    return s;\n"
            f"  }}();\n"
            f"  return kJoined.c_str();\n"
            f"}}\n")


def render_cpp(typing_svg: str, piano_svg: str, date: str) -> str:
    header = (
        "// Generated — embedded faithful SVGs for the MusicalTyping catalog component.\n"
        "// TWO dedicated mode frames, exported standalone via\n"
        "// tools/import-design/figma_rest_export.py (faithful-vector lane) and rendered\n"
        "// 1:1 through DesignFrameView/SkSVGDOM. The piano⇄typing toggle swaps which\n"
        "// frame renders (and the view's intrinsic size).\n"
        "//   • typing mode: Figma node 187:15  (732x266)\n"
        "//   • piano  mode: Figma node 187:349 (732x176)\n"
        f"// Source file {FILE_KEY}, re-imported {date}. The combined\n"
        "// spec frame (187:2) stacks both modes to show their states at once; the live\n"
        "// component shows one at a time. The design's baked \"selected keys shown\" demo\n"
        "// chord (pure-#16DAC2 lit key gradients in the key-bed) is NEUTRALIZED to the\n"
        "// resting key color here — white→#EBEEF1, black→#3A3F47/#16191E — so the live\n"
        "// momentary overlay owns every pressed-state highlight (matching the combined\n"
        "// frame's invariant). Toolbar/overview teal is untouched. Stored as chunked\n"
        "// base64 (no single >64K literal) and joined once at first use.\n"
        "// Regenerate via tools/import-design/reembed_mtk.py.\n"
        "#include <string>\n\n"
        "namespace pulp::view::detail {\n\n")
    return (header
            + chunk_fn("musical_typing_typing_svg_b64", typing_svg) + "\n"
            + chunk_fn("musical_typing_piano_svg_b64", piano_svg)
            + "\n}  // namespace pulp::view::detail\n")


def decode_fn(cpp: str, name: str) -> str:
    block = cpp.split(f"const char* {name}()", 1)[1]
    parts = re.findall(r'"([A-Za-z0-9+/=]*)"', block.split("};", 1)[0])
    return base64.b64decode("".join(parts)).decode("utf-8", "replace")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--token")
    ap.add_argument("--repo", default=os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__)))))
    ap.add_argument("--date", default="2026-06-17")
    ap.add_argument("--validate", action="store_true",
                    help="regenerate from the live Figma and assert the decoded SVGs "
                         "match the committed file; write nothing")
    a = ap.parse_args()
    token = a.token or os.environ.get("FIGMA_TOKEN")
    if not token:
        p = os.path.expanduser("~/.config/pulp/figma-token")
        if os.path.exists(p):
            token = open(p).read().strip()
    if not token:
        sys.exit("no Figma token (--token / $FIGMA_TOKEN / ~/.config/pulp/figma-token)")

    typing = neutralize(fetch_svg(TYPING_NODE, token))
    piano = neutralize(fetch_svg(PIANO_NODE, token))
    cpp = render_cpp(typing, piano, a.date)
    out_path = os.path.join(a.repo, "core/view/src/musical_typing_keyboard_svg.cpp")

    if a.validate:
        cur = open(out_path).read()
        ok_t = decode_fn(cur, "musical_typing_typing_svg_b64") == typing
        ok_p = decode_fn(cur, "musical_typing_piano_svg_b64") == piano
        print(f"typing decoded SVG matches committed: {ok_t}")
        print(f"piano  decoded SVG matches committed: {ok_p}")
        for label, svg in (("typing", typing), ("piano", piano)):
            print(f"  {label}: #16DAC2={svg.count('16DAC2')} (lit demo chord must be 0; "
                  f"toolbar+overview teal ≤4 total)")
        sys.exit(0 if ok_t and ok_p else 1)

    open(out_path, "w").write(cpp)
    print(f"wrote {out_path} ({len(cpp)} bytes)")


if __name__ == "__main__":
    main()
