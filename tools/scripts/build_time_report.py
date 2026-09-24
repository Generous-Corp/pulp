#!/usr/bin/env python3
"""Where a Ninja build spends its time, and what one edit costs to rebuild.

Three read-only views of a Ninja build directory, dependency-free python3:

  log           `.ninja_log` edge-seconds by category (compile, test-exe link,
                bundle link, codesign/stamps, archive, cargo, other), per build
                invocation. Multi-output edges appear once per OUTPUT in the log
                with identical start/end, so the log is deduplicated by
                (edge hash, start, end) before summing; summing raw lines counts a
                three-output edge three times.
  trace         clang `-ftime-trace` JSON aggregation: phase totals, compile CPU
                by source area, slowest TUs, headers by cumulative inclusive
                parse time, template instantiations.
  blast-radius  for each file: touch it, dry-run, restore its mtime, and count the
                compiles / executable links / bundle links ninja would run.

Blast radius needs a regen-free manifest. A plain `ninja -n` stops at
"Re-running CMake" whenever the tree uses CONFIGURE_DEPENDS globs (the glob
check cannot be evaluated in a dry run) and then reports nothing else, so every
file reads as zero. The dry run therefore uses a temporary copy of build.ninja
with the `build.ninja`, `CMakeFiles/VerifyGlobs.cmake_force` and
`CMakeFiles/cmake.verify_globs` edges removed. The copy lives in a temp dir,
never in the build dir.

The instrument carries its own control. With nothing touched it reports the
edges that are pending anyway (always-run custom commands, or an out-of-date
tree), and every per-file result is the set difference against that control. A
source file that is an input of the manifest MUST produce at least one compile
or link when touched; if it does not, the dry run is not seeing the graph and
the command exits 3 ("instrument broken") rather than printing zeros.

Usage:
  build_time_report.py log <build-dir|ninja_log> [--runs all|last|0-1] [--json]
  build_time_report.py trace <build-dir> [--src-root DIR] [--top N] [--json]
  build_time_report.py blast-radius <build-dir> FILE... [--src-root DIR] [--json]
"""

from __future__ import annotations

import argparse
import collections
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable

CATEGORIES = (
    "compile",
    "test-exe link",
    "bundle link",
    "codesign/stamps",
    "archive",
    "cargo",
    "other",
)

_BUNDLE_MARKERS = (".clap/", ".vst3/", ".component/", ".appex/", ".app/",
                   ".framework/", ".bundle/", ".lv2/", ".aaxplugin/")
_BUNDLE_SUFFIXES = (".dylib", ".so", ".clap", ".vst3", ".component", ".appex",
                    ".bundle", ".lv2", ".dll")
_COMPILE_SUFFIXES = (".o", ".obj", ".pch", ".gch", ".pcm")


# --------------------------------------------------------------------------
# .ninja_log
# --------------------------------------------------------------------------

@dataclass(frozen=True)
class LogEntry:
    start_ms: int
    end_ms: int
    output: str
    edge_hash: str

    @property
    def duration_ms(self) -> int:
        return max(0, self.end_ms - self.start_ms)


def categorize_output(output: str) -> str:
    """Category of a `.ninja_log` output path (build-dir relative or absolute)."""
    o = output.replace("\\", "/")
    low = o.lower()
    if "pulp-control-shipping-stamps/" in low or "codesign" in low \
            or low.endswith((".stamp", "-stamp", "_stamp")):
        return "codesign/stamps"
    if "cargo" in low or "/pulp-rs/" in low or low.startswith("pulp-rs/") \
            or "rust-cli" in low:
        return "cargo"
    if low.endswith(_COMPILE_SUFFIXES):
        return "compile"
    if low.endswith((".a", ".lib")):
        return "archive"
    if any(m in low for m in _BUNDLE_MARKERS) or low.endswith(_BUNDLE_SUFFIXES):
        return "bundle link"
    base = o.rsplit("/", 1)[-1]
    top = o.split("/", 1)[0]
    if top == "test" and "." not in base and "CMakeFiles" not in o:
        return "test-exe link"
    return "other"


def parse_ninja_log(text: str) -> list[list[LogEntry]]:
    """Split a `.ninja_log` into build invocations.

    Ninja appends one line per output as each edge FINISHES, so within one
    invocation end times never decrease. A new invocation restarts its clock,
    which shows up as an end time lower than the previous line's.
    """
    runs: list[list[LogEntry]] = []
    cur: list[LogEntry] = []
    prev_end = -1
    for line in text.splitlines():
        if not line.strip():
            continue
        if line.startswith("#"):
            # A version header begins a fresh log (recompaction or a new file).
            if cur:
                runs.append(cur)
                cur = []
            prev_end = -1
            continue
        parts = line.split("\t")
        if len(parts) < 5:
            continue
        try:
            start, end = int(parts[0]), int(parts[1])
        except ValueError:
            continue
        if end < prev_end and cur:
            runs.append(cur)
            cur = []
        prev_end = end
        cur.append(LogEntry(start, end, parts[3], parts[4]))
    if cur:
        runs.append(cur)
    return runs


def dedupe_edges(entries: Iterable[LogEntry]) -> list[tuple[LogEntry, list[str]]]:
    """One record per edge execution: (first entry, all its outputs)."""
    seen: dict[tuple[str, int, int], tuple[LogEntry, list[str]]] = {}
    for e in entries:
        key = (e.edge_hash, e.start_ms, e.end_ms)
        if key in seen:
            seen[key][1].append(e.output)
        else:
            seen[key] = (e, [e.output])
    return list(seen.values())


def edge_category(outputs: list[str]) -> str:
    """An edge's category: the most specific category among its outputs."""
    cats = {categorize_output(o) for o in outputs}
    for c in CATEGORIES:
        if c in cats and c != "other":
            return c
    return "other"


def select_runs(runs: list[list[LogEntry]], spec: str) -> list[int]:
    if not runs:
        return []
    if spec == "all":
        return list(range(len(runs)))
    if spec == "last":
        return [len(runs) - 1]
    out: list[int] = []
    for part in spec.split(","):
        part = part.strip()
        if "-" in part:
            a, b = part.split("-", 1)
            out.extend(range(int(a), int(b) + 1))
        elif part:
            out.append(int(part))
    bad = [i for i in out if i < 0 or i >= len(runs)]
    if bad:
        raise ValueError(f"run index out of range {bad}; log has {len(runs)} runs")
    return out


def summarize_edges(entries: Iterable[LogEntry]) -> dict:
    edges = dedupe_edges(entries)
    by_cat = {c: {"edge_seconds": 0.0, "count": 0} for c in CATEGORIES}
    raw_lines = 0
    wall_ms = 0
    for e, outs in edges:
        raw_lines += len(outs)
        cat = edge_category(outs)
        by_cat[cat]["edge_seconds"] += e.duration_ms / 1000.0
        by_cat[cat]["count"] += 1
        wall_ms = max(wall_ms, e.end_ms)
    total = sum(v["edge_seconds"] for v in by_cat.values())
    for v in by_cat.values():
        v["edge_seconds"] = round(v["edge_seconds"], 1)
        v["share"] = round(v["edge_seconds"] / total, 4) if total else 0.0
    return {
        "edges": len(edges),
        "log_lines": raw_lines,
        "edge_seconds": round(total, 1),
        "max_end_seconds": round(wall_ms / 1000.0, 1),
        "categories": by_cat,
    }


def ninja_log_report(path: Path, runs_spec: str = "all") -> dict:
    log_path = path / ".ninja_log" if path.is_dir() else path
    if not log_path.is_file():
        raise FileNotFoundError(f"no .ninja_log at {log_path}")
    runs = parse_ninja_log(log_path.read_text(errors="replace"))
    chosen = select_runs(runs, runs_spec)
    per_run = []
    for i, run in enumerate(runs):
        s = summarize_edges(run)
        per_run.append({"run": i, "edges": s["edges"], "edge_seconds": s["edge_seconds"],
                        "max_end_seconds": s["max_end_seconds"]})
    selected: list[LogEntry] = [e for i in chosen for e in runs[i]]
    return {
        "ninja_log": str(log_path),
        "runs_total": len(runs),
        "runs_selected": chosen,
        "per_run": per_run,
        "selected": summarize_edges(selected),
    }


def render_log_markdown(rep: dict) -> str:
    sel = rep["selected"]
    lines = [f"### `.ninja_log` edge-seconds (runs {rep['runs_selected']} of {rep['runs_total']})", "",
             f"{sel['edges']:,} edges from {sel['log_lines']:,} log lines; "
             f"{sel['edge_seconds']:,.1f} edge-s.", "",
             "| Category | Edge-s | Share | Count | Mean |", "|---|---:|---:|---:|---:|"]
    for cat in CATEGORIES:
        v = sel["categories"][cat]
        mean = v["edge_seconds"] / v["count"] if v["count"] else 0.0
        lines.append(f"| {cat} | {v['edge_seconds']:,.1f} | {v['share'] * 100:.1f}% | "
                     f"{v['count']:,} | {mean:.2f} s |")
    lines += ["", "| Run | Edges | Edge-s | Last edge ended at |", "|---:|---:|---:|---:|"]
    for r in rep["per_run"]:
        lines.append(f"| {r['run']} | {r['edges']:,} | {r['edge_seconds']:,.1f} | "
                     f"{r['max_end_seconds']:,.1f} s |")
    return "\n".join(lines)


# --------------------------------------------------------------------------
# -ftime-trace
# --------------------------------------------------------------------------

_PHASES = ("ExecuteCompiler", "Frontend", "Backend", "Source", "ParseClass",
           "InstantiateFunction", "InstantiateClass", "PerformPendingInstantiations",
           "CodeGen Function", "OptModule", "CodeGenPasses", "OptFunction")


def source_intervals(events: list[dict]) -> list[tuple[str, int]]:
    """(header, inclusive microseconds) for every `Source` span in one trace.

    clang emits `Source` as async begin/end pairs ('b'/'e') that nest by
    include depth, so they are matched with a stack. An unmatched 'e' is
    ignored rather than paired with the wrong begin.
    """
    out: list[tuple[str, int]] = []
    stack: list[tuple[int, str]] = []
    for e in events:
        if e.get("name") != "Source":
            continue
        ph = e.get("ph")
        if ph == "b":
            stack.append((int(e.get("ts", 0)), (e.get("args") or {}).get("detail", "?")))
        elif ph == "e" and stack:
            t0, hdr = stack.pop()
            out.append((hdr, int(e.get("ts", 0)) - t0))
    return out


def phase_totals(events: list[dict]) -> dict[str, int]:
    """`Total <phase>` complete ('X') events → {phase: microseconds}."""
    tot: dict[str, int] = {}
    for e in events:
        name = e.get("name", "")
        if e.get("ph") == "X" and name.startswith("Total "):
            tot[name[6:]] = int(e.get("dur", 0))
    return tot


def area_of(rel_obj: str) -> str:
    o = rel_obj.replace("\\", "/")
    if "_deps/" in o:
        return "_deps"
    rel = o.split("/CMakeFiles/")[0] if "/CMakeFiles/" in o else o
    parts = [p for p in rel.split("/") if p not in ("", ".")]
    if not parts:
        return "."
    if parts[0] == "core" and len(parts) > 1:
        return "core/" + parts[1]
    return parts[0]


def _norm_header(path: str, src_root: str) -> str:
    p = os.path.realpath(path) if path.startswith("/") else path
    if src_root and p.startswith(src_root + "/"):
        return p[len(src_root) + 1:]
    p = re.sub(r".*/SDKs/[^/]+\.sdk", "<SDK>", p)
    p = re.sub(r".*/usr/include/c\+\+/v1", "<libc++>", p)
    p = re.sub(r".*/\.cache/pulp/skia/[^/]+", "<skia-cache>", p)
    return p


def iter_trace_files(build_dir: Path) -> Iterable[Path]:
    for dp, _dn, fn in os.walk(build_dir):
        if "CMakeFiles" not in dp:
            continue
        for f in fn:
            if f.endswith(".json") and (".o" in f or f.endswith((".cpp.json", ".mm.json", ".c.json"))):
                yield Path(dp) / f


def aggregate_traces(build_dir: Path, src_root: Path | None = None, top: int = 25) -> dict:
    root = os.path.realpath(str(src_root)) if src_root else ""
    hdr_time: collections.Counter = collections.Counter()
    hdr_count: collections.Counter = collections.Counter()
    inst_time: collections.Counter = collections.Counter()
    inst_count: collections.Counter = collections.Counter()
    area_t: collections.Counter = collections.Counter()
    area_n: collections.Counter = collections.Counter()
    phases: collections.Counter = collections.Counter()
    tus = []
    for path in iter_trace_files(build_dir):
        try:
            events = json.loads(path.read_text()).get("traceEvents", [])
        except (OSError, ValueError):
            continue
        tot = phase_totals(events)
        if "ExecuteCompiler" not in tot:
            continue
        for hdr, dur in source_intervals(events):
            h = _norm_header(hdr, root)
            hdr_time[h] += dur
            hdr_count[h] += 1
        for e in events:
            if e.get("ph") == "X" and e.get("name") in ("InstantiateClass", "InstantiateFunction"):
                det = re.sub(r"<.*", "<…>", (e.get("args") or {}).get("detail", "?"))
                inst_time[det] += int(e.get("dur", 0))
                inst_count[det] += 1
        rel = os.path.relpath(path, build_dir)
        a = area_of(rel)
        area_t[a] += tot["ExecuteCompiler"]
        area_n[a] += 1
        for k in _PHASES:
            phases[k] += tot.get(k, 0)
        tus.append({"tu": rel, "total_s": tot["ExecuteCompiler"] / 1e6,
                    "frontend_s": tot.get("Frontend", 0) / 1e6,
                    "backend_s": tot.get("Backend", 0) / 1e6})
    tus.sort(key=lambda t: -t["total_s"])
    s = 1e6
    return {
        "tus": len(tus),
        "phases_cpu_s": {k: round(v / s, 1) for k, v in phases.most_common()},
        "areas": [{"area": a, "cpu_s": round(v / s, 1), "tus": area_n[a]}
                  for a, v in area_t.most_common()],
        "slowest_tus": [{k: (round(v, 2) if isinstance(v, float) else v) for k, v in t.items()}
                        for t in tus[:top]],
        "headers": [{"header": h, "cpu_s": round(v / s, 1), "includes": hdr_count[h]}
                    for h, v in hdr_time.most_common(top)],
        "templates": [{"template": h[:160], "cpu_s": round(v / s, 1), "count": inst_count[h]}
                      for h, v in inst_time.most_common(top)],
    }


def render_trace_markdown(rep: dict) -> str:
    lines = [f"### `-ftime-trace`: {rep['tus']:,} TUs", "", "Phase totals (CPU-s): " +
             ", ".join(f"{k} {v:,.0f}" for k, v in rep["phases_cpu_s"].items())]
    lines += ["", "| Area | CPU-s | TUs |", "|---|---:|---:|"]
    lines += [f"| {a['area']} | {a['cpu_s']:,.0f} | {a['tus']:,} |" for a in rep["areas"][:15]]
    lines += ["", "| Header | CPU-s | Includes |", "|---|---:|---:|"]
    lines += [f"| `{h['header']}` | {h['cpu_s']:,.0f} | {h['includes']:,} |" for h in rep["headers"]]
    lines += ["", "| Slowest TU | Total | Frontend | Backend |", "|---|---:|---:|---:|"]
    lines += [f"| `{t['tu']}` | {t['total_s']:.1f} | {t['frontend_s']:.1f} | {t['backend_s']:.1f} |"
              for t in rep["slowest_tus"][:10]]
    return "\n".join(lines)


# --------------------------------------------------------------------------
# blast radius
# --------------------------------------------------------------------------

_REGEN_OUTPUTS = ("build.ninja", "CMakeFiles/VerifyGlobs.cmake_force",
                  "CMakeFiles/cmake.verify_globs")


class InstrumentBroken(RuntimeError):
    """The dry run cannot see the dependency graph; any count would be a lie."""


_TOKEN = re.compile(r"(?:\$.|[^\s:$|])+")


def _unescape(tok: str) -> str:
    return re.sub(r"\$(.)", r"\1", tok)


def parse_build_statement(stmt: str) -> tuple[list[str], str, list[str]]:
    """`build <outs> [| <implicit>] : <rule> <ins> [| <implicit>] [|| <order-only>]`
    → (outputs, rule, explicit+implicit inputs).

    ``stmt`` is one logical line (continuations already joined). Ninja escapes
    a literal space, colon or dollar in a path as ``$ ``, ``$:``, ``$$``.
    """
    body = stmt[len("build "):]
    outs: list[str] = []
    i = 0
    while i < len(body) and body[i] != ":":
        if body[i] in " \t|":
            i += 1
            continue
        m = _TOKEN.match(body, i)
        if not m:
            i += 1
            continue
        outs.append(_unescape(m.group(0)))
        i = m.end()
    toks = [_unescape(t) for t in re.findall(r"(?:\$.|[^\s$])+", body[i + 1:])]
    rule = toks[0] if toks else ""
    ins: list[str] = []
    for t in toks[1:]:
        if t == "||":
            break
        if t != "|":
            ins.append(t)
    return outs, rule, ins


def iter_build_statements(manifest: str) -> Iterable[tuple[int, int, str]]:
    """(first line index, last line index, joined statement) per `build` line."""
    lines = manifest.splitlines()
    i = 0
    while i < len(lines):
        if lines[i].startswith("build "):
            start = i
            joined = lines[i]
            while joined.endswith("$") and not joined.endswith("$$") and i + 1 < len(lines):
                i += 1
                joined = joined[:-1] + " " + lines[i].lstrip()
            yield start, i, joined
        i += 1


def strip_regeneration_edges(manifest: str, build_dir: str = "") -> tuple[str, int]:
    """Remove the CMake regeneration/glob-verification edge blocks.

    An edge block is its `build` line, any `$`-continued lines, and the
    indented variable bindings that follow. Returns (text, blocks removed).
    """
    targets = set(_REGEN_OUTPUTS)
    if build_dir:
        bd = build_dir.rstrip("/")
        targets |= {f"{bd}/{t}" for t in _REGEN_OUTPUTS}
    lines = manifest.splitlines(keepends=True)
    drop: set[int] = set()
    removed = 0
    for start, end, stmt in iter_build_statements(manifest):
        outs, _rule, _ins = parse_build_statement(stmt)
        if outs and outs[0] in targets:
            removed += 1
            j = end + 1
            while j < len(lines) and lines[j][:1] in (" ", "\t") and lines[j].strip():
                j += 1
            drop.update(range(start, j))
    return "".join(l for k, l in enumerate(lines) if k not in drop), removed


def compile_sources(manifest: str, build_dir: str = "") -> dict[str, str]:
    """Object output (build-relative) → absolute path of the source it compiles."""
    prefix = build_dir.rstrip("/") + "/" if build_dir else ""
    out: dict[str, str] = {}
    for _s, _e, stmt in iter_build_statements(manifest):
        outs, rule, ins = parse_build_statement(stmt)
        if "_COMPILER" not in rule.upper() or not outs or not ins:
            continue
        obj = outs[0][len(prefix):] if prefix and outs[0].startswith(prefix) else outs[0]
        src = ins[0] if os.path.isabs(ins[0]) or not build_dir else os.path.join(build_dir, ins[0])
        # realpath: a worktree reached through a symlink (/tmp → /private/tmp)
        # must compare equal to the resolved path of the file being touched.
        out[obj] = os.path.realpath(src)
    return out


_STATUS = re.compile(r"^\[(\d+)/(\d+)\]\s?(.*)$")
_LINK = re.compile(r"^Linking \S+ (?:CFBundle )?(executable|shared module|shared library|"
                   r"static library) (.+)$")


def classify_description(desc: str) -> str:
    """Category of one planned edge from its ninja description."""
    d = desc.strip()
    low = d.lower()
    m = _LINK.match(d)
    if m:
        kind, target = m.group(1), m.group(2)
        if kind == "static library":
            return "archive"
        if kind in ("shared module", "shared library") \
                or any(b in target.lower() + "/" for b in _BUNDLE_MARKERS):
            return "bundle link"
        return "test-exe link" if target.replace("\\", "/").startswith("test/") else "exe link"
    if low.startswith(("building ", "precompiling ")) and " object " in low:
        return "compile"
    if "shipping-stamps" in low or "codesign" in low or re.search(r"\bsign(ing)?\b", low) \
            or "stamp" in low:
        return "codesign/stamps"
    if "cargo" in low or "rust" in low:
        return "cargo"
    return "other"


def count_categories(descs: Iterable[str]) -> dict[str, int]:
    c = collections.Counter(classify_description(d) for d in descs)
    return {
        "compiles": c["compile"],
        "exe_links": c["test-exe link"] + c["exe link"],
        "test_exe_links": c["test-exe link"],
        "bundle_links": c["bundle link"],
        "stamps": c["codesign/stamps"],
        "archives": c["archive"],
        "other": c["other"] + c["cargo"],
        "total": sum(c.values()),
    }


def parse_plan(stdout: str) -> collections.Counter:
    """Planned edge descriptions from a dry run, checked for completeness.

    Ninja numbers every edge it plans as ``[i/N]``. On a non-terminal it does
    not always print a line for each one (a plain ``ninja -n`` of a 3,715-edge
    plan printed 2,591 lines, while the same run under ``-d explain`` printed
    all of them), so a plan is accepted only when the number of status lines
    equals ``N``. A short plan raises rather than undercounting.
    """
    descs: collections.Counter = collections.Counter()
    total = None
    n = 0
    for line in stdout.splitlines():
        m = _STATUS.match(line)
        if not m:
            continue
        total = int(m.group(2))
        descs[m.group(3).strip()] += 1
        n += 1
    if total is None:
        if "no work to do" in stdout or not stdout.strip() or "Entering directory" in stdout:
            return descs
        raise InstrumentBroken("dry run printed no plan and did not say 'no work to do'")
    if n != total:
        raise InstrumentBroken(f"ninja planned {total} edges but printed {n}; "
                               f"the plan cannot be counted")
    return descs


@dataclass
class DryRunner:
    build_dir: Path
    ninja: str = "ninja"
    removed_blocks: int = field(default=0, init=False)
    manifest_text: str = field(default="", init=False)
    _manifest: Path | None = field(default=None, init=False)
    _tmp: str | None = field(default=None, init=False)

    def __enter__(self) -> "DryRunner":
        src = self.build_dir / "build.ninja"
        if not src.is_file():
            raise FileNotFoundError(f"{self.build_dir} is not a Ninja build dir (no build.ninja)")
        self.manifest_text = src.read_text(errors="replace")
        stripped, self.removed_blocks = strip_regeneration_edges(self.manifest_text,
                                                                 str(self.build_dir))
        self._tmp = tempfile.mkdtemp(prefix="pulp-blast-")
        self._manifest = Path(self._tmp) / "build.ninja"
        self._manifest.write_text(stripped)
        return self

    def __exit__(self, *exc) -> None:
        if self._tmp:
            shutil.rmtree(self._tmp, ignore_errors=True)

    def run(self) -> collections.Counter:
        # `-d explain` is load-bearing: it is what makes ninja print every planned
        # edge on a non-terminal (see parse_plan). A dry run writes neither the
        # build log nor the deps log.
        proc = subprocess.run(
            [self.ninja, "-C", str(self.build_dir), "-f", str(self._manifest),
             "-n", "-k", "0", "-d", "explain"],
            capture_output=True, text=True)
        if "Re-running CMake" in proc.stdout or "Re-running CMake" in proc.stderr:
            raise InstrumentBroken("dry run still re-runs CMake: the regeneration edge was not removed")
        if proc.returncode != 0:
            raise InstrumentBroken(f"ninja -n failed (exit {proc.returncode}): "
                                   f"{(proc.stderr or proc.stdout).strip()[-400:]}")
        return parse_plan(proc.stdout)


def resolve_source(path: str, src_root: Path) -> Path | None:
    p = Path(path)
    if p.is_absolute():
        return p if p.exists() else None
    cand = src_root / p
    if cand.exists():
        return cand
    hits = sorted(src_root.glob(f"core/*/include/{path}")) + sorted(src_root.glob(f"core/*/{path}"))
    return hits[0] if hits else None


def _touch_and_measure(runner: DryRunner, path: Path,
                       control: collections.Counter) -> tuple[collections.Counter, str]:
    """Touch ``path``, dry-run, restore its mtime; return newly planned edges."""
    st = os.stat(path)
    saved = (st.st_atime_ns, st.st_mtime_ns)
    now_ns = max(saved[1] + 1_000_000_000, time.time_ns())
    note = ""
    os.utime(path, ns=(saved[0], now_ns))
    try:
        planned = runner.run()
    finally:
        cur = os.stat(path)
        if cur.st_mtime_ns == now_ns and cur.st_size == st.st_size:
            os.utime(path, ns=saved)
        else:
            # Someone wrote the file while it was touched; restoring the old
            # mtime would hide their edit from their next build.
            note = "file changed during measurement; mtime NOT restored"
    return planned - control, note


def _object_of(desc: str) -> str | None:
    m = re.match(r"^Building \S+ object (.+)$", desc)
    return m.group(1) if m else None


def blast_radius(build_dir: Path, files: list[str], src_root: Path,
                 ninja: str = "ninja") -> dict:
    build_dir = build_dir.resolve()
    src_root = src_root.resolve()
    results = []
    with DryRunner(build_dir, ninja) as runner:
        sources = compile_sources(runner.manifest_text, str(build_dir))
        compiled = set(sources.values())
        control = runner.run()
        control_counts = count_categories(control.elements())
        pending_objs = {o for o in (_object_of(d) for d in control) if o}
        pending_sources = {sources[o] for o in pending_objs if o in sources}

        # Built-in positive control: a compiled .cpp whose object is up to date.
        probe_result = None
        for obj, s in sources.items():
            if s.endswith(".cpp") and s.startswith(str(src_root) + "/") \
                    and obj not in pending_objs and os.path.exists(s):
                new, _ = _touch_and_measure(runner, Path(s), control)
                probe_result = {"file": os.path.relpath(s, src_root),
                                **count_categories(new.elements())}
                if probe_result["compiles"] == 0:
                    raise InstrumentBroken(
                        f"positive control {probe_result['file']} (compiled by this manifest, "
                        f"object up to date) produced no compile when touched")
                break

        for f in files:
            src = resolve_source(f, src_root)
            if src is None:
                results.append({"file": f, "status": "missing"})
                continue
            new, note = _touch_and_measure(runner, src, control)
            counts = count_categories(new.elements())
            rel = os.path.relpath(src, src_root) if str(src).startswith(str(src_root)) else str(src)
            row = {"file": rel, "status": "ok", **counts}
            if note:
                row["note"] = note
            touched_any = counts["compiles"] + counts["exe_links"] + counts["bundle_links"] \
                + counts["archives"]
            key = os.path.realpath(str(src))
            if touched_any == 0:
                if key in pending_sources:
                    row["status"] = "already-pending"
                elif key in compiled:
                    raise InstrumentBroken(
                        f"{rel} is compiled by this manifest but produced no compile or link")
                elif src.suffix in (".cpp", ".cc", ".c", ".mm", ".m"):
                    row["status"] = "not-in-build"
                else:
                    row["status"] = "no-dependents"
            results.append(row)
    return {
        "build_dir": str(build_dir),
        "regen_edges_removed": runner.removed_blocks,
        "control": control_counts,
        "positive_control": probe_result,
        "files": results,
    }


def render_blast_markdown(rep: dict) -> str:
    c = rep["control"]
    lines = [f"### Blast radius (`{rep['build_dir']}`)", "",
             f"Control (nothing touched): {c['total']} edges pending "
             f"({c['compiles']} compiles, {c['exe_links']} exe links); per-file counts exclude them."]
    if c["total"] > 10:
        lines.append(f"**The tree is not up to date** ({c['total']} pending edges): per-file counts "
                     "are a LOWER BOUND, since anything already pending is not counted.")
    pc = rep.get("positive_control")
    if pc:
        lines.append(f"Positive control `{pc['file']}`: {pc['compiles']} compiles, "
                     f"{pc['exe_links']} exe links.")
    lines += ["", "| Touched file | Compiles | Exe relinks | Bundle relinks | Stamps | Status |",
              "|---|---:|---:|---:|---:|---|"]
    for r in rep["files"]:
        if r["status"] == "missing":
            lines.append(f"| `{r['file']}` | | | | | missing |")
            continue
        lines.append(f"| `{r['file']}` | {r['compiles']:,} | {r['exe_links']:,} | "
                     f"{r['bundle_links']:,} | {r['stamps']:,} | {r['status']}"
                     f"{' — ' + r['note'] if r.get('note') else ''} |")
    return "\n".join(lines)

# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------

def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p_log = sub.add_parser("log", help="edge-seconds by category from .ninja_log")
    p_log.add_argument("path", type=Path)
    p_log.add_argument("--runs", default="all", help="all | last | comma list / ranges (0-1,3)")
    p_log.add_argument("--json", action="store_true")
    p_tr = sub.add_parser("trace", help="aggregate -ftime-trace JSON")
    p_tr.add_argument("build_dir", type=Path)
    p_tr.add_argument("--src-root", type=Path)
    p_tr.add_argument("--top", type=int, default=25)
    p_tr.add_argument("--json", action="store_true")
    p_br = sub.add_parser("blast-radius", help="touch → dry-run → restore, per file")
    p_br.add_argument("build_dir", type=Path)
    p_br.add_argument("files", nargs="+")
    p_br.add_argument("--src-root", type=Path)
    p_br.add_argument("--ninja", default=os.environ.get("NINJA", "ninja"))
    p_br.add_argument("--json", action="store_true")
    args = ap.parse_args(argv)

    try:
        if args.cmd == "log":
            rep = ninja_log_report(args.path, args.runs)
            print(json.dumps(rep, indent=2) if args.json else render_log_markdown(rep))
        elif args.cmd == "trace":
            rep = aggregate_traces(args.build_dir, args.src_root, args.top)
            if rep["tus"] == 0:
                print(f"no -ftime-trace JSON under {args.build_dir} "
                      "(configure with -DCMAKE_CXX_FLAGS=-ftime-trace)", file=sys.stderr)
                return 2
            print(json.dumps(rep, indent=2) if args.json else render_trace_markdown(rep))
        else:
            src_root = args.src_root or _guess_src_root(args.build_dir)
            rep = blast_radius(args.build_dir, args.files, src_root, args.ninja)
            print(json.dumps(rep, indent=2) if args.json else render_blast_markdown(rep))
    except InstrumentBroken as exc:
        print(f"instrument broken: {exc}", file=sys.stderr)
        return 3
    except (FileNotFoundError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    return 0


def _guess_src_root(build_dir: Path) -> Path:
    cache = build_dir / "CMakeCache.txt"
    try:
        for line in cache.read_text(errors="replace").splitlines():
            if line.startswith("CMAKE_HOME_DIRECTORY:INTERNAL="):
                return Path(line.split("=", 1)[1])
    except OSError:
        pass
    return build_dir.resolve().parent


if __name__ == "__main__":
    sys.exit(main())
