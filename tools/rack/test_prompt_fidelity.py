#!/usr/bin/env python3
"""Does what a person typed survive, and does what they are SHOWN say so?

    tools/rack/test_prompt_fidelity.py

Written to answer "long prompts truncate inconsistently", which sat as a
sentence with no reproduction behind it. The answer turned out to be that the
prompt is never truncated at all: it survives every boundary byte-for-byte, and
what gets cut is the ECHO of it on the chat rail. Three surfaces show the same
prompt at three lengths -- the title bar at 28 bytes, the rail at 200, the
explanation panel whole -- and the rail cut on a BYTE offset, so it split
characters and never said it had shortened anything.

The "inconsistently" was two effects at once, both content-dependent:

  * A byte cut lands at a different number of VISIBLE characters depending on
    the encoding width -- 200 ASCII, 100 accented, 66 CJK, 50 emoji.
  * Whether the last glyph survives depends on whether the cut offset divides
    that width. Pure runs are accidentally safe when it does; MIXED text has no
    such luck, and every ASCII-prefix length tested split a character at both
    200 and the 120 used elsewhere in the same file.

So this file measures three things, and NONE of them by reading source and
hoping. The two transit boundaries are driven end to end with real processes,
and the display helper is EXTRACTED FROM THE SHIPPING SOURCE at test time and
compiled -- there is no copy of it here to drift out of date.
"""

from __future__ import annotations

import hashlib
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
SHELL_SRC = os.path.join(ROOT, "forge-seam", "modular", "modular_shell.cpp")
ENGINE_SRC = os.path.join(ROOT, "forge-seam", "modular", "process_engine.cpp")

sys.path.insert(0, HERE)


def extract(path: str, signature: str) -> str:
    """One function's source, from the file that ships it.

    Copying the function here instead would make this test agree with a
    snapshot of the code rather than with the code, which is the failure mode
    it exists to catch. Brace-matched from the signature line, which is enough
    for the plain functions this reads and loud when it is not.
    """
    text = open(path, encoding="utf-8").read()
    at = text.find(signature)
    if at < 0:
        raise SystemExit(f"{os.path.basename(path)} no longer defines "
                         f"`{signature}` -- this test is measuring nothing")
    depth, i, started = 0, at, False
    while i < len(text):
        if text[i] == "{":
            depth += 1
            started = True
        elif text[i] == "}":
            depth -= 1
            if started and depth == 0:
                return text[at:i + 1]
        i += 1
    raise SystemExit(f"unbalanced braces reading `{signature}`")


def compile_probe(body: str, main: str, out: str) -> str | None:
    """Compile extracted source plus a driver. -> the binary, or None."""
    src = out + ".cpp"
    with open(src, "w", encoding="utf-8") as f:
        f.write("#include <cstdio>\n#include <cstdlib>\n#include <cstddef>\n"
                "#include <fstream>\n#include <sstream>\n#include <string>\n\n")
        f.write(body + "\n\n" + main + "\n")
    r = subprocess.run(["clang++", "-std=c++20", "-O1", "-o", out, src],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print(f"  WRONG  the extracted source does not compile:\n{r.stderr[:1500]}")
        return None
    return out


# ── boundary 1: what a person typed -> the generator's argv ─────────────────

CASES = {
    "plain ascii":      lambda n: b"a" * n,
    "words + newlines": lambda n: (b"make a patch\n" * (n // 13 + 1))[:n],
    "single quotes":    lambda n: (b"don't stop it's fine " * (n // 21 + 1))[:n],
    "shell metachars":  lambda n: (b"$X `id` \\ ! ~ * ? & | ; < > " * (n // 29 + 1))[:n],
    "utf8 accented":    lambda n: "\u00e9".encode() * n,
    "utf8 CJK":         lambda n: "\u97f3".encode() * n,
    "utf8 emoji":       lambda n: "\U0001f3b9".encode() * n,
    "mixed ascii+emoji": lambda n: (b"a" * 7 + "\U0001f3b9".encode()) * (n // 11 + 1),
}
LENGTHS = [100, 4000, 64000, 200000]


def check_shell_boundary() -> tuple:
    """The prompt must reach the generator's argv byte for byte.

    Drives the EXACT command ProcessEngine::start builds -- shell_quote, `sh
    -c`, nohup, the background `&` and the stdout redirect all in place -- with
    the shipping `shell_quote` extracted and compiled, not re-implemented.

    A re-implementation would pass this test while the shipped function was
    broken, which is the whole class of defect being ruled out.
    """
    bad, ran = 0, 1
    work = tempfile.mkdtemp(prefix="prompt-fidelity-")
    quote_main = """
int main(int argc, char** argv) {
    std::ifstream f(argv[1], std::ios::binary);
    std::stringstream ss; ss << f.rdbuf();
    const std::string q = shell_quote(ss.str());
    std::fwrite(q.data(), 1, q.size(), stdout);
    return 0;
}
"""
    probe = compile_probe(extract(ENGINE_SRC, "std::string shell_quote("),
                          quote_main, os.path.join(work, "quote"))
    if not probe:
        return 1, ran

    recorder = os.path.join(work, "recorder.py")
    with open(recorder, "w") as f:
        f.write("import hashlib, os, sys\n"
                "b = (sys.argv[2] if len(sys.argv) > 2 else '')"
                ".encode('utf-8', 'surrogatepass')\n"
                "open(os.environ['RECORD_TO'], 'w').write("
                "f'{len(b)} {hashlib.sha256(b).hexdigest()}')\n")

    def quoted(raw: bytes) -> str:
        p = os.path.join(work, "in.bin")
        open(p, "wb").write(raw)
        return subprocess.run([probe, p], capture_output=True).stdout.decode(
            "utf-8", "surrogatepass")

    import time
    losses = []
    for name, make in CASES.items():
        for n in LENGTHS:
            want = make(n)
            rec = os.path.join(work, "rec.txt")
            if os.path.exists(rec):
                os.remove(rec)
            log = os.path.join(work, "log.txt")
            cmd = (f"cd {quoted(work.encode())} && nohup python3 -u recorder.py "
                   f"build {quoted(want)} > {quoted(log.encode())} 2>&1 &")
            subprocess.run(["sh", "-c", cmd], capture_output=True,
                           env=dict(os.environ, RECORD_TO=rec))
            got = None
            for _ in range(400):
                if os.path.exists(rec):
                    got = open(rec).read().split()
                    break
                time.sleep(0.05)
            if not got or int(got[0]) != len(want) or \
                    got[1] != hashlib.sha256(want).hexdigest():
                losses.append(f"{name}@{len(want)}B -> "
                              f"{got[0] if got else 'never arrived'}")
    if losses:
        bad += 1
        print(f"  WRONG  the prompt does not survive the shell: {losses[:6]}")
    else:
        print(f"  ok     {len(CASES) * len(LENGTHS)} prompts up to "
              f"{max(LENGTHS):,} bytes reach the generator byte for byte")
    return bad, ran


# ── boundary 2: the generator's argv -> the model's argv ────────────────────

def check_model_boundary() -> tuple:
    """The request must reach the model's prompt verbatim.

    Needs an inventory, so it needs Rack. Reported as a SKIP that is NOT
    counted as a check that ran -- a skip scoring as a pass is the same defect
    as a gate that measures presence.
    """
    import patch as P
    inv = P.inventory()
    if "Fundamental" not in inv or "Core" not in inv:
        print("  --     no Rack installed, so the model boundary is UNTESTED "
              "here (this is a skip, not a pass)")
        return 0, 0

    work = tempfile.mkdtemp(prefix="prompt-fidelity-model-")
    cap = os.path.join(work, "prompt.txt")
    stub = os.path.join(work, "claude")
    with open(stub, "w") as f:
        f.write("#!/usr/bin/env python3\n"
                "import json, os, sys\n"
                "open(os.environ['CAPTURE_TO'], 'w', encoding='utf-8')"
                ".write(sys.argv[-1])\n"
                "print('```json patch\\n' + json.dumps({'modules': ["
                "{'id': 1, 'plugin': 'Fundamental', 'model': 'VCO', 'pos': [0, 0]},"
                "{'id': 2, 'plugin': 'Core', 'model': 'AudioInterface2', 'pos': [8, 0]}],"
                "'cables': [{'id': 1, 'outputModuleId': 1, 'outputId': 0,"
                "'inputModuleId': 2, 'inputId': 0}]}) + '\\n```')\n")
    os.chmod(stub, 0o755)
    os.environ["CAPTURE_TO"] = cap

    saved = (P.find_claude, P.audibility, P.lint, P.reflow, P.configure_audio)
    P.find_claude = lambda: stub
    P.audibility = lambda pch: (P.UNMEASURED, "not run by this test")
    P.lint = lambda pch, inv_: []
    P.reflow = lambda pch, inv_: pch
    P.configure_audio = lambda pch: None
    losses = []
    try:
        for name, make in (("plain ascii", lambda n: "a" * n),
                           ("single quotes", lambda n: ("don't " * n)[:n]),
                           ("utf8 emoji", lambda n: "\U0001f3b9" * n),
                           ("utf8 CJK", lambda n: "\u97f3" * n)):
            for n in (50, 4000, 100000):
                req = make(n)
                if os.path.exists(cap):
                    os.remove(cap)
                try:
                    P.generate(req, inv, None, retries=0)
                except SystemExit:
                    pass
                sent = open(cap, encoding="utf-8").read() if os.path.exists(cap) else ""
                if req not in sent:
                    losses.append(f"{name}@{n}")
    finally:
        (P.find_claude, P.audibility, P.lint, P.reflow,
         P.configure_audio) = saved

    if losses:
        print(f"  WRONG  the request does not reach the model verbatim: {losses}")
        return 1, 1
    print("  ok     requests up to 100,000 characters reach the model verbatim")
    return 0, 1


# ── the display: what the person is SHOWN ───────────────────────────────────

def check_echo_is_character_safe() -> tuple:
    """A shortened echo must never split a character, and must say it cut.

    `shortened()` is extracted from the shipping shell and compiled, so this
    asserts the behaviour of the code that runs rather than of a copy.

    The mixed case is the one that matters. Pure runs of one character width
    are accidentally safe whenever the cut offset divides that width, which is
    exactly why a byte cut at 120 and at 200 looked fine for years: 120 and 200
    both divide by 2 and by 4. Seven ASCII characters followed by emoji does
    not divide by anything, and every prefix length split a character.
    """
    bad, ran = 0, 1
    work = tempfile.mkdtemp(prefix="prompt-fidelity-echo-")
    main = r"""
/// Is the WHOLE string well-formed UTF-8?
///
/// Checking only the tail is not enough and the difference is not academic: a
/// byte cut that appends an ellipsis leaves the broken glyph in the MIDDLE,
/// with three valid bytes after it, so a tail-only check calls it clean. That
/// exact hole let a deliberately reverted fix pass this test once.
bool valid_utf8(const std::string& s) {
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::size_t want = 0;
        if (c < 0x80) want = 1;
        else if ((c & 0xE0) == 0xC0) want = 2;
        else if ((c & 0xF0) == 0xE0) want = 3;
        else if ((c & 0xF8) == 0xF0) want = 4;
        else return false;                       // a stray continuation byte
        if (i + want > s.size()) return false;   // truncated at the end
        for (std::size_t k = 1; k < want; ++k)
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80)
                return false;                    // truncated in the middle
        i += want;
    }
    return true;
}

int main() {
    const std::string ell = "\xE2\x80\xA6";       // the ellipsis it appends
    const char* units[] = {"a", "\xC3\xA9", "\xE9\x9F\xB3", "\xF0\x9F\x8E\xB9"};
    int split = 0, unmarked = 0, overlong = 0;

    for (std::size_t limit : {size_t(120), size_t(200)}) {
        for (const char* unit : units) {
            for (int pad = 0; pad < 8; ++pad) {          // the mixed cases
                std::string p(pad, 'a');
                while (p.size() < limit * 5) p += unit;
                const std::string shown = shortened(p, limit);
                if (!valid_utf8(shown)) ++split;
                // It cut, so it must say so.
                if (shown.size() < p.size() &&
                    shown.compare(shown.size() >= ell.size()
                                      ? shown.size() - ell.size() : 0,
                                  ell.size(), ell) != 0) ++unmarked;
                // And it must not keep more characters than it promised.
                std::size_t chars = 0;
                for (char c : shown)
                    if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++chars;
                if (chars > limit + 1) ++overlong;       // +1 for the ellipsis
            }
        }
    }
    // A prompt shorter than the limit must come back untouched.
    const std::string little = "a short prompt";
    const bool kept = shortened(little, 200) == little;
    std::printf("%d %d %d %d\n", split, unmarked, overlong, kept ? 1 : 0);
    return 0;
}
"""
    probe = compile_probe(extract(SHELL_SRC, "std::string shortened("),
                          main, os.path.join(work, "echo"))
    if not probe:
        return 1, ran
    out = subprocess.run([probe], capture_output=True, text=True).stdout.split()
    split, unmarked, overlong, kept = (int(x) for x in out)
    if split:
        bad += 1
        print(f"  WRONG  the echo splits a UTF-8 character in {split} case(s); a "
              f"byte cut shows the reader a broken glyph")
    elif unmarked:
        bad += 1
        print(f"  WRONG  the echo shortened the text without saying so in "
              f"{unmarked} case(s); a cut that does not look cut reads as data "
              f"the app lost")
    elif overlong:
        bad += 1
        print(f"  WRONG  the echo kept more characters than its limit in "
              f"{overlong} case(s)")
    elif not kept:
        bad += 1
        print("  WRONG  a prompt shorter than the limit was altered anyway")
    else:
        print("  ok     the echo cuts on character boundaries, marks what it "
              "cut, and leaves a short prompt alone")

    # AND THE CALLERS. A correct helper nothing calls is the defect that was
    # there: `trimmed()` sat 1,100 lines above two raw byte cuts of the prompt.
    ran += 1
    text = open(SHELL_SRC, encoding="utf-8").read()
    raw = [ln.strip() for ln in text.splitlines()
           if "prompt.substr(0," in ln or "text.substr(0, kMax)" in ln]
    if raw:
        bad += 1
        print(f"  WRONG  a prompt is still cut by byte offset: {raw}")
    else:
        print("  ok     nothing cuts the prompt by byte offset any more")
    return bad, ran


def main() -> int:
    bad, ran = 0, 0
    for check in (check_shell_boundary, check_model_boundary,
                  check_echo_is_character_safe):
        b, r = check()
        bad += b
        ran += r
    print(f"\n{ran - bad}/{ran} correct")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
