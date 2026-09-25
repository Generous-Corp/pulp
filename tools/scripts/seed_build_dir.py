#!/usr/bin/env python3
"""Seed a worktree's build directory from a warm donor build directory.

A fresh Pulp worktree pays a full configure plus every compile and link
before its first useful build, even when a sibling worktree a few commits
away already holds all of that work. On APFS the donor's Ninja build
directory can be cloned in one ``clonefile`` call (shared blocks, no data
copied) and then retargeted so Ninja and CMake see it as the target
worktree's own:

1. every text file that names the donor source or build path is rewritten
   to name the target's (CMakeCache.txt, build.ninja, compile_commands.json,
   CTest files, generated cmake, response files, ...);
2. ``.ninja_deps`` is re-emitted with the same substitution, so the header
   dependencies Ninja consults live in the target tree, not the donor's;
3. ``.ninja_log`` command hashes are recomputed from the retargeted
   ``build.ninja``, so an edge whose command differs only by the path is not
   reported as "command line changed";
4. binary outputs that embed the donor path (a ``-D`` define carrying the
   source dir, ``__FILE__`` without ccache's base_dir, a build-tree rpath)
   are deleted, so Ninja rebuilds them rather than leaving a binary that
   still points at the donor;
5. tracked sources that are the same blob in both worktrees and clean in
   both get the donor's mtime, so a fresh checkout is not newer than every
   object.

The retarget only ever removes gratuitous work: anything Ninja cannot prove
up to date afterwards (a source that differs, a command that changed, an
output we could not vouch for) is rebuilt as usual. The donor is only read.

Exit codes: 0 seeded; 3 unsupported (not macOS/APFS, cross-volume, donor not
a warm Ninja dir, build type mismatch, donor busy, no candidate) with nothing
left at the target; 1 error.
"""

from __future__ import annotations

import argparse
import ctypes
import errno
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

UNSUPPORTED = 3
RECEIPT_NAME = ".pulp-seed-receipt.json"

# Never worth sniffing for text: compiler and link outputs plus media.
BINARY_SUFFIXES = {
    ".o", ".a", ".dylib", ".so", ".pch", ".gch", ".pcm", ".png", ".jpg",
    ".jpeg", ".wav", ".flac", ".zip", ".gz", ".tar", ".bin", ".dat", ".ttf",
    ".otf", ".woff", ".woff2", ".rlib", ".rmeta",
}


# ---------------------------------------------------------------------------
# The hash Ninja stores in .ninja_log for each command.
# ---------------------------------------------------------------------------

M64 = (1 << 64) - 1


def murmur_hash64a(data: bytes, seed: int = 0xDECAFBADDECAFBAD) -> int:
    """Ninja's command hash through log v6."""
    m = 0xC6A4A7935BD1E995
    r = 47
    n = len(data)
    h = (seed ^ ((n * m) & M64)) & M64
    nblocks = n // 8
    for i in range(nblocks):
        k = struct.unpack_from("<Q", data, i * 8)[0]
        k = (k * m) & M64
        k ^= k >> r
        k = (k * m) & M64
        h ^= k
        h = (h * m) & M64
    tail = data[nblocks * 8:]
    if tail:
        for i in range(len(tail) - 1, -1, -1):
            h ^= tail[i] << (8 * i)
        h = (h * m) & M64
    h ^= h >> r
    h = (h * m) & M64
    h ^= h >> r
    return h


_RAPID_SECRET = (0x2D358DCCAA6C78A5, 0x8BB84B93962EACC9, 0x4B33A62ED433D4A3)
_RAPID_SEED = 0xBDD89AA982704029


def _rapid_mum(a: int, b: int) -> tuple[int, int]:
    r = a * b
    return r & M64, (r >> 64) & M64


def _rapid_mix(a: int, b: int) -> int:
    a, b = _rapid_mum(a, b)
    return a ^ b


def rapidhash(data: bytes, seed: int = _RAPID_SEED) -> int:
    """Ninja's command hash from log v7 (ninja 1.13)."""
    s = _RAPID_SECRET
    n = len(data)

    def r64(i: int) -> int:
        return struct.unpack_from("<Q", data, i)[0]

    def r32(i: int) -> int:
        return struct.unpack_from("<I", data, i)[0]

    seed ^= _rapid_mix(seed ^ s[0], s[1]) ^ n
    if n <= 16:
        if n >= 4:
            plast = n - 4
            a = (r32(0) << 32) | r32(plast)
            delta = (n & 24) >> (n >> 3)
            b = (r32(delta) << 32) | r32(plast - delta)
        elif n > 0:
            a = (data[0] << 56) | (data[n >> 1] << 32) | data[n - 1]
            b = 0
        else:
            a = b = 0
    else:
        p = 0
        i = n
        if i > 48:
            see1 = see2 = seed
            while True:
                seed = _rapid_mix(r64(p) ^ s[0], r64(p + 8) ^ seed)
                see1 = _rapid_mix(r64(p + 16) ^ s[1], r64(p + 24) ^ see1)
                see2 = _rapid_mix(r64(p + 32) ^ s[2], r64(p + 40) ^ see2)
                p += 48
                i -= 48
                if i < 48:
                    break
            seed ^= see1 ^ see2
        if i > 16:
            seed = _rapid_mix(r64(p) ^ s[2], r64(p + 8) ^ seed ^ s[1])
            if i > 32:
                seed = _rapid_mix(r64(p + 16) ^ s[2], r64(p + 24) ^ seed)
        a = r64(p + i - 16)
        b = r64(p + i - 8)
    a ^= s[1]
    b ^= seed
    a, b = _rapid_mum(a, b)
    return _rapid_mix(a ^ s[0] ^ n, b ^ s[1])


HASHERS = {"rapidhash": rapidhash, "murmur64a": murmur_hash64a}


def log_hash_equal(field: str, value: int) -> bool:
    try:
        return int(field, 16) == value
    except ValueError:
        return False


def hasher_for_log(header: str) -> str:
    m = re.match(r"# ninja log v(\d+)", header)
    version = int(m.group(1)) if m else 0
    return "rapidhash" if version >= 7 else "murmur64a"


# ---------------------------------------------------------------------------
# Ninja file formats.
# ---------------------------------------------------------------------------

def parse_ninja_log(path: Path) -> tuple[str, list[list[str]]]:
    lines = path.read_text(errors="surrogateescape").splitlines()
    header = lines[0] if lines else ""
    entries = [ln.split("\t") for ln in lines[1:] if ln and not ln.startswith("#")]
    return header, [e for e in entries if len(e) == 5]


def write_ninja_log(path: Path, header: str, entries: list[list[str]]) -> None:
    body = "\n".join("\t".join(e) for e in entries)
    path.write_text(header + "\n" + body + ("\n" if body else ""), errors="surrogateescape")


def rewrite_ninja_deps(path: Path, subst) -> dict:
    """Re-emit a ``.ninja_deps`` (format v3/v4) with its path records rewritten.

    Records are ``uint32 header`` + payload. A deps record has the high bit
    set; a path record is the NUL-padded path followed by ``~id``.
    """
    raw = path.read_bytes()
    magic = b"# ninjadeps\n"
    if not raw.startswith(magic):
        raise ValueError("not a ninja deps log")
    version = struct.unpack_from("<i", raw, len(magic))[0]
    if version not in (3, 4):
        raise ValueError(f"unsupported ninja deps version {version}")
    out = bytearray(raw[: len(magic) + 4])
    pos = len(magic) + 4
    paths = deps = rewritten = path_id = 0
    while pos + 4 <= len(raw):
        header = struct.unpack_from("<I", raw, pos)[0]
        pos += 4
        size = header & 0x7FFFFFFF
        rec = raw[pos: pos + size]
        if len(rec) < size:
            break  # truncated trailing record: ninja drops it too
        pos += size
        if header & 0x80000000:
            deps += 1
            out += struct.pack("<I", header) + rec
            continue
        checksum = struct.unpack_from("<I", rec, size - 4)[0]
        if checksum != (~path_id) & 0xFFFFFFFF:
            raise ValueError("deps log checksum mismatch")
        p = rec[: size - 4].rstrip(b"\0")
        newp = subst(p)
        if newp != p:
            rewritten += 1
        padded = newp + b"\0" * ((4 - len(newp) % 4) % 4)
        out += struct.pack("<I", len(padded) + 4) + padded + struct.pack("<I", checksum)
        paths += 1
        path_id += 1
    path.write_bytes(bytes(out))
    return {"path_records": paths, "deps_records": deps, "paths_rewritten": rewritten}


_UNESCAPE = re.compile(r"\$([ :$])")


_BINDING = re.compile(r"^([A-Za-z0-9_.-]+)\s*=\s*(.*)$")
_VARREF = re.compile(r"\$\{([A-Za-z0-9_.-]+)\}|\$([A-Za-z0-9_-]+)")


def ninja_build_outputs(build_ninja: Path):
    """Yield ``(all_outputs, first_output)`` for each ``build`` statement.

    Top-level bindings are expanded in output paths: CMake spells a custom
    command's implicit output as ``${cmake_ninja_workdir}<path>`` and the log
    records the expanded absolute path.
    """
    bindings: dict[str, str] = {}

    def expand(tok: str) -> str:
        return _VARREF.sub(lambda m: bindings.get(m.group(1) or m.group(2), ""), tok)

    with build_ninja.open(errors="surrogateescape") as f:
        pending = ""
        for line in f:
            line = line.rstrip("\n")
            if line.endswith("$"):
                pending += line[:-1]
                continue
            line = pending + line
            pending = ""
            if not line.startswith("build "):
                m = _BINDING.match(line)
                if m and not line.startswith(" "):
                    bindings[m.group(1)] = expand(m.group(2))
                continue
            i = 0
            colon = -1
            while i < len(line):
                c = line[i]
                if c == "$":
                    i += 2
                    continue
                if c == ":":
                    colon = i
                    break
                i += 1
            if colon < 0:
                continue
            outs = [
                expand(_UNESCAPE.sub(r"\1", tok))
                for tok in re.split(r"(?<!\$)\s+", line[6:colon].strip())
                if tok not in ("", "|")
            ]
            if outs:
                yield outs, outs[0]


class CommandIndex:
    """Ninja's evaluated command per output, as the build log hashes it."""

    def __init__(self, build_dir: Path, ninja: str):
        self.build_dir = build_dir
        compdb = json.loads(
            subprocess.run([ninja, "-C", str(build_dir), "-t", "compdb"], check=True,
                           capture_output=True, text=True).stdout
        )
        self.cmd_by_first = {e["output"]: e["command"] for e in compdb if "output" in e}
        self.first_by_out: dict[str, str] = {}
        for outs, first in ninja_build_outputs(build_dir / "build.ninja"):
            for o in outs:
                self.first_by_out[o] = first
        self._rsp: dict[str, str] = {}

    def command_for(self, output: str) -> str | None:
        first = self.first_by_out.get(output)
        if first is None:
            return None
        cmd = self.cmd_by_first.get(first)
        if cmd is None:
            return None
        m = re.search(r"@(\S+\.rsp)", cmd)
        if m:
            rsp = m.group(1)
            if rsp not in self._rsp:
                p = self.build_dir / rsp
                self._rsp[rsp] = p.read_text(errors="surrogateescape") if p.exists() else ""
            if self._rsp[rsp]:
                # Ninja hashes command + ";rspfile=" + rspfile_content.
                cmd = cmd + ";rspfile=" + self._rsp[rsp]
        return cmd


def log_hash_match_rate(build_dir: Path, ninja: str, limit: int = 3000) -> dict:
    """How many of a build dir's own log entries our hasher reproduces.

    This is the control for the rehash: if it cannot reproduce the donor's
    own hashes (a different ninja, a different rsp convention) the retarget
    would either rebuild everything or, worse, hide a changed command.
    """
    header, entries = parse_ninja_log(build_dir / ".ninja_log")
    name = hasher_for_log(header)
    fn = HASHERS[name]
    index = CommandIndex(build_dir, ninja)
    checked = matched = 0
    for e in entries[-limit:]:
        cmd = index.command_for(e[3])
        if cmd is None:
            continue
        checked += 1
        if log_hash_equal(e[4], fn(cmd.encode("utf-8", "surrogateescape"))):
            matched += 1
    return {"hasher": name, "checked": checked, "matched": matched}


def rehash_ninja_log(build_dir: Path, ninja: str, hasher: str, subst=None) -> dict:
    """Retarget each entry's output path, then recompute its command hash.

    Most outputs are logged relative to the build dir, but an output CMake
    spells through ``${cmake_ninja_workdir}`` (generated sources, custom
    command stamps, the glob check) is logged absolute. Left naming the donor,
    Ninja finds no entry for the target's path and reruns the command, and
    every object compiled from its output with it.
    """
    log = build_dir / ".ninja_log"
    header, entries = parse_ninja_log(log)
    fn = HASHERS[hasher]
    index = CommandIndex(build_dir, ninja)
    stats = {"entries": len(entries), "rehashed": 0, "unmatched": 0,
             "paths_rewritten": 0, "hasher": hasher}
    for e in entries:
        if subst is not None:
            raw = e[3].encode("utf-8", "surrogateescape")
            new = subst(raw)
            if new != raw:
                e[3] = new.decode("utf-8", "surrogateescape")
                stats["paths_rewritten"] += 1
        cmd = index.command_for(e[3])
        if cmd is None:
            stats["unmatched"] += 1
            continue
        h = fn(cmd.encode("utf-8", "surrogateescape"))
        if not log_hash_equal(e[4], h):
            e[4] = f"{h:x}"  # ninja writes the hash unpadded
            stats["rehashed"] += 1
    write_ninja_log(log, header, entries)
    return stats


# ---------------------------------------------------------------------------
# clonefile
# ---------------------------------------------------------------------------

def clonefile(src: Path, dst: Path) -> None:
    libc = ctypes.CDLL(None, use_errno=True)
    fn = getattr(libc, "clonefile", None)
    if fn is None:
        raise OSError(errno.ENOTSUP, "clonefile unavailable on this platform")
    clone_nofollow = 0x0001
    rc = fn(os.fsencode(str(src)), os.fsencode(str(dst)), clone_nofollow)
    if rc != 0:
        e = ctypes.get_errno()
        raise OSError(e, os.strerror(e), str(dst))


# ---------------------------------------------------------------------------
# Retarget
# ---------------------------------------------------------------------------

def is_text(path: Path) -> bool:
    if path.suffix in BINARY_SUFFIXES:
        return False
    try:
        with path.open("rb") as f:
            head = f.read(8192)
    except OSError:
        return False
    return b"\0" not in head


def make_subst(pairs: list[tuple[bytes, bytes]]):
    """A substitution over whole path prefixes (longest first, so the build
    dir under the source dir wins) that stops at a path-component boundary,
    so ``.../pulp-x`` never rewrites ``.../pulp-x2``."""
    pairs = sorted(pairs, key=lambda p: -len(p[0]))
    pattern = re.compile(
        b"(" + b"|".join(re.escape(o) for o, _ in pairs) + b")(?![A-Za-z0-9_.\\-])"
    )
    table = dict(pairs)

    def subst(data: bytes) -> bytes:
        return pattern.sub(lambda m: table[m.group(1)], data)

    return subst, pattern


def binary_mentions(path: Path, pattern, chunk: int = 8 << 20) -> bool:
    """True when the pattern occurs anywhere in the file, read in chunks with
    overlap so a match across a chunk boundary is not missed."""
    overlap = 4096
    try:
        with path.open("rb") as f:
            prev = b""
            while True:
                buf = f.read(chunk)
                if not buf:
                    return False
                if pattern.search(prev + buf):
                    return True
                prev = buf[-overlap:]
    except OSError:
        return False


def rewrite_tree(root: Path, subst, pattern, taint_pattern) -> dict:
    """Retarget text files; delete binary outputs that embed the donor path."""
    files = rewritten = binaries = 0
    tainted: list[str] = []
    for dirpath, dirnames, filenames in os.walk(root):
        # Never follow symlinks: shared FetchContent sources live behind them.
        dirnames[:] = [d for d in dirnames if not os.path.islink(os.path.join(dirpath, d))]
        for name in filenames:
            p = Path(dirpath) / name
            if p.is_symlink() or name in (".ninja_deps", ".ninja_log", RECEIPT_NAME):
                continue
            if is_text(p):
                files += 1
                data = p.read_bytes()
                if not pattern.search(data):
                    continue
                st = p.stat()
                p.write_bytes(subst(data))
                os.utime(p, ns=(st.st_atime_ns, st.st_mtime_ns))
                rewritten += 1
                continue
            binaries += 1
            if binary_mentions(p, taint_pattern):
                tainted.append(str(p.relative_to(root)))
                p.unlink()
    return {"text_files": files, "text_files_rewritten": rewritten,
            "binaries_scanned": binaries, "binaries_tainted": len(tainted),
            "tainted": tainted}


# ---------------------------------------------------------------------------
# Source mtime sync
# ---------------------------------------------------------------------------

def git(cwd: Path, *args: str) -> str:
    return subprocess.run(["git", *args], cwd=cwd, check=True, capture_output=True,
                          text=True).stdout


def tracked_blobs(wt: Path) -> dict[str, tuple[str, str]]:
    out = {}
    for line in git(wt, "ls-tree", "-r", "-z", "HEAD").split("\0"):
        if not line:
            continue
        meta, path = line.split("\t", 1)
        mode, kind, sha = meta.split(" ")
        if kind == "blob":
            out[path] = (mode, sha)
    return out


def dirty_paths(wt: Path) -> set[str]:
    out = set()
    for line in git(wt, "status", "--porcelain", "-z", "--untracked-files=all").split("\0"):
        if len(line) > 3:
            out.add(line[3:])
    return out


def sync_source_mtimes(donor: Path, target: Path) -> dict:
    """Give unchanged, clean tracked files the donor's mtimes.

    A file that differs between the two HEADs, or is modified in either
    worktree, keeps its checkout mtime and so stays newer than every object
    that was built from the donor's version.
    """
    d = tracked_blobs(donor)
    t = tracked_blobs(target)
    skip = dirty_paths(donor) | dirty_paths(target)
    synced = differ = 0
    for path, blob in t.items():
        if path in skip:
            continue
        if d.get(path) != blob:
            differ += 1
            continue
        src = donor / path
        dst = target / path
        try:
            if os.path.islink(src) or not os.path.isfile(src) or os.path.islink(dst):
                continue
            st = os.stat(src)
            os.utime(dst, ns=(st.st_atime_ns, st.st_mtime_ns))
            synced += 1
        except OSError:
            continue
    subprocess.run(["git", "update-index", "-q", "--refresh"], cwd=target, check=False)
    return {"sources_synced": synced, "sources_differ_or_dirty": differ + len(skip)}


# ---------------------------------------------------------------------------
# Donor discovery and checks
# ---------------------------------------------------------------------------

def cache_value(cache: Path, key: str) -> str | None:
    try:
        with cache.open(errors="surrogateescape") as f:
            for line in f:
                if line.startswith(key + ":"):
                    return line.split("=", 1)[1].rstrip("\n")
    except OSError:
        return None
    return None


def donor_busy(donor_build: Path) -> bool:
    """True when a live process has its cwd inside the donor build dir."""
    try:
        out = subprocess.run(["lsof", "-Fn", "-d", "cwd"], capture_output=True,
                             text=True, timeout=30).stdout
    except (OSError, subprocess.TimeoutExpired):
        return False
    prefix = str(donor_build) + "/"
    return any(
        line.startswith("n") and (line[1:] == str(donor_build) or line[1:].startswith(prefix))
        for line in out.splitlines()
    )


def worktrees(repo: Path) -> list[Path]:
    out = []
    for line in git(repo, "worktree", "list", "--porcelain").splitlines():
        if line.startswith("worktree "):
            out.append(Path(line[len("worktree "):]))
    return out


def donor_reject_reason(donor_build: Path, target: Path, build_type: str | None,
                        examples: str | None) -> str | None:
    cache = donor_build / "CMakeCache.txt"
    if not cache.exists():
        return "no CMakeCache.txt"
    if cache_value(cache, "CMAKE_GENERATOR") != "Ninja":
        return "generator is not Ninja"
    if not (donor_build / ".ninja_log").exists() or not (donor_build / "build.ninja").exists():
        return "configured but never built"
    if build_type is not None:
        bt = cache_value(cache, "CMAKE_BUILD_TYPE") or ""
        if bt.lower() != build_type.lower():
            return f"build type is {bt or 'empty'}, not {build_type}"
    if examples is not None:
        ex = (cache_value(cache, "PULP_BUILD_EXAMPLES") or "OFF").upper()
        want = "ON" if examples.lower() in ("on", "1", "true", "yes") else "OFF"
        if ex != want:
            return f"PULP_BUILD_EXAMPLES is {ex}, not {want}"
    try:
        if os.stat(donor_build).st_dev != os.stat(target).st_dev:
            return "on a different volume"
    except OSError as e:
        return str(e)
    return None


def pick_donor(target: Path, build_dir: str, build_type: str | None, examples: str | None,
               notes: list[str]) -> Path | None:
    """The eligible sibling worktree whose HEAD is closest to the target's."""
    target = target.resolve()
    best = None
    for wt in worktrees(target):
        if wt.resolve() == target:
            continue
        donor_build = wt / build_dir
        why = donor_reject_reason(donor_build, target, build_type, examples)
        if why:
            notes.append(f"{wt}: {why}")
            continue
        try:
            counts = git(target, "rev-list", "--left-right", "--count",
                         f"{git(wt, 'rev-parse', 'HEAD').strip()}...HEAD").split()
            distance = int(counts[0]) + int(counts[1])
        except (subprocess.CalledProcessError, ValueError, IndexError):
            notes.append(f"{wt}: HEAD not comparable")
            continue
        freshness = (donor_build / ".ninja_log").stat().st_mtime
        key = (distance, -freshness)
        if best is None or key < best[0]:
            best = (key, wt)
    return best[1] if best else None


VERIFY_MANIFEST = ".pulp-seed-verify.ninja"


def cmake_rerun_pending(build_dir: Path) -> list[str]:
    """Inputs of the manifest's regeneration edge that are newer than it.

    When any exists (a CMakeLists.txt that differs from the donor's, say) the
    first build re-runs CMake, and the dry-run count below describes the
    donor's graph, so it is a lower bound rather than the plan.
    """
    manifest = build_dir / "build.ninja"
    stamp = manifest.stat().st_mtime_ns
    text = manifest.read_text(errors="surrogateescape")
    m = re.search(r"^build build\.ninja\b(.*?): RERUN_CMAKE((?:[^\n]*\$\n)*[^\n]*)",
                  text, re.M)
    if not m:
        return []
    deps = m.group(2).replace("$\n", " ").split("||")[0]
    newer = []
    for tok in re.split(r"(?<!\$)\s+", deps.replace("|", " ").strip()):
        tok = _UNESCAPE.sub(r"\1", tok)
        if not tok or "VerifyGlobs" in tok or tok.endswith("cmake.verify_globs"):
            continue
        p = Path(tok) if os.path.isabs(tok) else build_dir / tok
        try:
            if p.stat().st_mtime_ns > stamp:
                newer.append(tok)
        except OSError:
            newer.append(tok)
    return newer


def count_dirty_edges(build_dir: Path, ninja: str) -> int:
    """Edges a build would run, counted without Ninja's manifest check.

    ``ninja -n`` on ``build.ninja`` stops at the manifest's own regeneration
    edge whenever it looks dirty: CMake's ``CONFIGURE_DEPENDS`` glob check is
    an always-dirty ``restat`` edge, so a dry run prints "Re-checking globbed
    directories" + "Re-running CMake" and reports 2 whatever the tree needs.
    Loading a byte copy under another name leaves no edge that produces the
    loaded manifest, so Ninja plans the real build. The glob check itself
    still runs on the real build and only regenerates when a glob changed.
    """
    manifest = build_dir / VERIFY_MANIFEST
    shutil.copyfile(build_dir / "build.ninja", manifest)
    try:
        dry = subprocess.run([ninja, "-C", str(build_dir), "-f", VERIFY_MANIFEST, "-n"],
                             capture_output=True, text=True)
    finally:
        manifest.unlink(missing_ok=True)
    if dry.returncode != 0:
        raise RuntimeError(f"ninja -n failed: {dry.stderr.strip() or dry.stdout[-500:]}")
    # The "[i/N]" total, not a line count: a dry run skips the status line
    # of edges that finish together, so lines undercount by half or more.
    totals = [int(n) for n in re.findall(r"^\[\d+/(\d+)\] ", dry.stdout, re.M)]
    return max(totals, default=0)


def unsupported(msg: str) -> int:
    print(f"seed-build: unsupported: {msg}", file=sys.stderr)
    return UNSUPPORTED


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--from", dest="donor", default="auto",
                    help="donor worktree or build dir; 'auto' picks the eligible sibling "
                         "worktree closest to the target's HEAD (default)")
    ap.add_argument("--to", dest="target", required=True, help="target worktree")
    ap.add_argument("--build-dir", default="build", help="build dir name under each worktree")
    ap.add_argument("--build-type", default=None,
                    help="require this CMAKE_BUILD_TYPE in the donor (e.g. Release)")
    ap.add_argument("--examples", default=None, choices=["on", "off"],
                    help="require this PULP_BUILD_EXAMPLES value in the donor")
    ap.add_argument("--ninja", default=shutil.which("ninja") or "ninja")
    ap.add_argument("--json", action="store_true", help="print the receipt as JSON on stdout")
    ap.add_argument("--dry-run", action="store_true",
                    help="report the donor and the checks without cloning")
    ap.add_argument("--allow-busy-donor", action="store_true")
    ap.add_argument("--no-source-mtime-sync", action="store_true")
    ap.add_argument("--no-verify", action="store_true",
                    help="skip the closing `ninja -n` dirty-edge count")
    args = ap.parse_args(argv)

    t0 = time.monotonic()
    target = Path(os.path.abspath(args.target))
    target_build = target / args.build_dir
    if not target.is_dir():
        return unsupported(f"{target} is not a directory")
    if target_build.exists():
        return unsupported(f"{target_build} already exists")
    if sys.platform != "darwin":
        return unsupported("APFS clonefile is macOS-only")

    notes: list[str] = []
    if args.donor == "auto":
        donor_wt = pick_donor(target, args.build_dir, args.build_type, args.examples, notes)
        if donor_wt is None:
            for n in notes:
                print(f"seed-build: skipped {n}", file=sys.stderr)
            return unsupported("no eligible donor worktree")
        donor_build = donor_wt / args.build_dir
    else:
        donor = Path(args.donor).resolve()
        donor_build = donor if (donor / "CMakeCache.txt").exists() else donor / args.build_dir
        why = donor_reject_reason(donor_build, target, args.build_type, args.examples)
        if why:
            return unsupported(f"{donor_build}: {why}")
    old_src = cache_value(donor_build / "CMakeCache.txt", "CMAKE_HOME_DIRECTORY")
    old_build = cache_value(donor_build / "CMakeCache.txt", "CMAKE_CACHEFILE_DIR")
    if not old_src or not old_build:
        return unsupported("donor CMakeCache.txt lacks CMAKE_HOME_DIRECTORY/CMAKE_CACHEFILE_DIR")
    if not args.allow_busy_donor and donor_busy(donor_build):
        return unsupported(f"a live process has its cwd inside {donor_build}")

    receipt: dict = {
        "donor_build": str(donor_build), "target_build": str(target_build),
        "old_source": old_src, "new_source": str(target),
    }
    # Control: our hasher must reproduce the donor's own log before we rewrite
    # a single hash; otherwise a changed command could be hidden from Ninja.
    control = log_hash_match_rate(donor_build, args.ninja)
    receipt["hash_control"] = control
    if control["checked"] == 0 or control["matched"] < 0.9 * control["checked"]:
        return unsupported(
            f"cannot reproduce the donor's ninja log hashes "
            f"({control['matched']}/{control['checked']} with {control['hasher']}); "
            f"ninja version mismatch?"
        )
    if args.dry_run:
        receipt["dry_run"] = True
        print(json.dumps(receipt, indent=2) if args.json else
              f"seed-build: would clone {donor_build} -> {target_build}")
        return 0

    tmp = target / (args.build_dir + ".seed-tmp")
    if tmp.exists():
        shutil.rmtree(tmp)
    t = time.monotonic()
    try:
        clonefile(donor_build, tmp)
    except OSError as e:
        if e.errno in (errno.ENOTSUP, errno.EXDEV, errno.EOPNOTSUPP):
            return unsupported(f"clonefile: {e.strerror}")
        raise
    receipt["clone_s"] = round(time.monotonic() - t, 3)
    try:
        pairs = [(old_build.encode(), str(target_build).encode()),
                 (old_src.encode(), str(target).encode())]
        real_old = os.path.realpath(old_src)
        if real_old != old_src:
            pairs.append((os.path.join(real_old, args.build_dir).encode(),
                          os.path.realpath(target_build).encode()))
            pairs.append((real_old.encode(), os.path.realpath(target).encode()))
        subst, pattern = make_subst(pairs)
        taint = re.compile(b"|".join(re.escape(o) for o, _ in pairs))
        t = time.monotonic()
        receipt.update(rewrite_tree(tmp, subst, pattern, taint))
        receipt["rewrite_s"] = round(time.monotonic() - t, 3)
        t = time.monotonic()
        if (tmp / ".ninja_deps").exists():
            receipt["deps"] = rewrite_ninja_deps(tmp / ".ninja_deps", subst)
        receipt["deps_s"] = round(time.monotonic() - t, 3)
        # Into place before the rehash: `ninja -t compdb` must run at the
        # final path so it evaluates the retargeted build.ninja.
        os.rename(tmp, target_build)
        tmp = None
        t = time.monotonic()
        receipt["log"] = rehash_ninja_log(target_build, args.ninja, control["hasher"], subst)
        receipt["rehash_s"] = round(time.monotonic() - t, 3)
        if not args.no_source_mtime_sync:
            t = time.monotonic()
            receipt.update(sync_source_mtimes(Path(old_src), target))
            receipt["mtime_sync_s"] = round(time.monotonic() - t, 3)
        if not args.no_verify:
            t = time.monotonic()
            receipt["dirty_edges"] = count_dirty_edges(target_build, args.ninja)
            rerun = cmake_rerun_pending(target_build)
            receipt["cmake_rerun_pending"] = rerun[:20]
            receipt["verify_s"] = round(time.monotonic() - t, 3)
    except Exception:
        if tmp is not None and tmp.exists():
            shutil.rmtree(tmp)
        elif target_build.exists():
            shutil.rmtree(target_build)
        raise
    receipt["total_s"] = round(time.monotonic() - t0, 3)
    (target_build / RECEIPT_NAME).write_text(json.dumps(receipt, indent=2) + "\n")
    if args.json:
        print(json.dumps(receipt, indent=2))
    else:
        dirty = receipt.get("dirty_edges")
        print(
            f"seed-build: cloned {donor_build} -> {target_build} in {receipt['total_s']} s: "
            f"{receipt['text_files_rewritten']} text files retargeted, "
            f"{receipt['log']['rehashed']} log entries rehashed, "
            f"{receipt['binaries_tainted']} path-bearing binaries dropped, "
            f"{receipt.get('sources_synced', 0)} source mtimes synced"
            + (f"; {dirty} edges left to build" if dirty is not None else "")
            + ("" if not receipt.get("cmake_rerun_pending") else
               " before CMake re-runs for "
               + ", ".join(receipt["cmake_rerun_pending"][:3]) + " (a lower bound)")
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
