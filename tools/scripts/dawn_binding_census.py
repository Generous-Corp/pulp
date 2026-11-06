#!/usr/bin/env python3
"""Count the Dawn implementations each built artifact brings into a process.

    python3 tools/scripts/dawn_binding_census.py <artifact>... [--json out.json]

An artifact is a plug-in bundle (.vst3, .clap, .component), an app bundle, a
standalone executable or a shared library. Its image set is every Mach-O image
it carries (the bundle's main executable and every library inside the bundle)
plus every non-system library those images load, resolved through their
install names, @rpath (LC_RPATH), @loader_path and @executable_path. That is
the set of images one load of the artifact puts into a process.

For each image the census reads the symbol table with `nm` and records:

  - whether the image defines Dawn: any defined symbol of Dawn's C API
    (`wgpu*`, `dawn*`) or of its implementation (`dawn::native`), local symbols
    included, so a Dawn linked with hidden visibility still counts;
  - whether it exports Dawn: a defined *external* Dawn symbol;
  - whether it installs Dawn's proc table by calling into another image: an
    undefined reference to `dawnProcSetProcs`;
  - whether it is the bootstrap: it defines Vellum's Dawn bootstrap
    (`register_dawn_bootstrap` / `register_native_dawn_bootstrap`).

Rules, per artifact:

  1. At most one image in the set defines Dawn.
  2. A plug-in bundle's own main executable may carry Dawn only with hidden
     symbols: it must not export it, or two plug-ins in one host would bind
     each other's copy.
  3. Only the bootstrap image may install the proc table from outside: every
     image with an undefined `dawnProcSetProcs` reference must be the one that
     defines the bootstrap.

Exit status: 0 every artifact passes, 1 a rule failed, 2 an artifact or image
could not be read (a census that cannot see is not a pass).

What it cannot see: a stripped image keeps no local symbols, so a Dawn linked
privately into a stripped image is invisible (an exported one still shows), and
a library loaded with dlopen() at run time is not in the static image set.
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import re
import subprocess
import sys
from pathlib import Path
from typing import Callable, Iterable

SYSTEM_PREFIXES = ("/usr/lib/", "/System/")
BUNDLE_SUFFIXES = (".vst3", ".clap", ".component", ".app", ".appex", ".bundle", ".aaxplugin")
PLUGIN_SUFFIXES = (".vst3", ".clap", ".component", ".aaxplugin")

# A defined Dawn symbol: the C API (wgpuCreateInstance, dawnProcSetProcs, ...)
# or the implementation namespace dawn::native (mangled N4dawn6native).
_DAWN_C = re.compile(r"^_?(wgpu[A-Z]|dawn[A-Z])")
_DAWN_NATIVE = "4dawn6native"
_BOOTSTRAP = ("register_dawn_bootstrap", "register_native_dawn_bootstrap")
_PROC_INSTALL = re.compile(r"^_?dawnProcSetProcs$")


@dataclasses.dataclass
class Symbols:
    """One image's symbol table, as `nm` reports it."""

    defined_external: list = dataclasses.field(default_factory=list)
    defined_local: list = dataclasses.field(default_factory=list)
    undefined: list = dataclasses.field(default_factory=list)


@dataclasses.dataclass
class ImageReport:
    path: str
    defines_dawn: bool = False
    exports_dawn: bool = False
    installs_procs: bool = False
    is_bootstrap: bool = False
    dawn_symbols: int = 0


@dataclasses.dataclass
class ArtifactReport:
    artifact: str
    main_image: str
    is_plugin_bundle: bool
    images: list
    verdict: str = "PASS"
    reasons: list = dataclasses.field(default_factory=list)

    @property
    def dawn_images(self) -> list:
        return [i.path for i in self.images if i.defines_dawn]


class CensusError(RuntimeError):
    """An artifact or image could not be read."""


def _is_dawn(name: str) -> bool:
    return bool(_DAWN_C.match(name)) or _DAWN_NATIVE in name


def classify(path: str, symbols: Symbols) -> ImageReport:
    report = ImageReport(path=path)
    ext = [s for s in symbols.defined_external if _is_dawn(s)]
    local = [s for s in symbols.defined_local if _is_dawn(s)]
    report.dawn_symbols = len(ext) + len(local)
    report.defines_dawn = report.dawn_symbols > 0
    report.exports_dawn = bool(ext)
    report.installs_procs = any(_PROC_INSTALL.match(s) for s in symbols.undefined)
    report.is_bootstrap = any(
        b in s for s in symbols.defined_external + symbols.defined_local for b in _BOOTSTRAP)
    return report


def judge(artifact: str, main_image: str, is_plugin_bundle: bool,
          images: list) -> ArtifactReport:
    report = ArtifactReport(artifact=artifact, main_image=main_image,
                            is_plugin_bundle=is_plugin_bundle, images=images)
    dawn = report.dawn_images
    if len(dawn) > 1:
        report.reasons.append(
            f"{len(dawn)} images define Dawn (at most one per process): " + ", ".join(dawn))
    main = next((i for i in images if i.path == main_image), None)
    if is_plugin_bundle and main is not None and main.exports_dawn:
        report.reasons.append(
            f"the plug-in's own executable exports Dawn ({main.path}); it must keep it hidden")
    bootstraps = [i.path for i in images if i.is_bootstrap]
    for image in images:
        if image.installs_procs and image.path not in bootstraps:
            report.reasons.append(
                f"{image.path} installs Dawn's proc table outside the bootstrap")
    if report.reasons:
        report.verdict = "FAIL"
    return report


# ── Reading Mach-O images ──────────────────────────────────────────────────


def _run(cmd: list) -> str:
    result = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8",
                            errors="replace")
    if result.returncode != 0:
        raise CensusError(f"{' '.join(cmd)} failed: {result.stderr.strip()[:300]}")
    return result.stdout


def read_symbols(path: Path) -> Symbols:
    """`nm` over one image: defined external, defined local, undefined."""
    symbols = Symbols()
    for line in _run(["nm", "-P", "-A", str(path)]).splitlines():
        # "<file>: <name> <type> [<value> [<size>]]"; a fat binary repeats.
        parts = line.rsplit(": ", 1)[-1].split()
        if len(parts) < 2:
            continue
        name, kind = parts[0], parts[1]
        if kind in ("U", "u"):
            symbols.undefined.append(name)
        elif kind.isupper():
            symbols.defined_external.append(name)
        else:
            symbols.defined_local.append(name)
    return symbols


def _load_commands(path: Path) -> tuple[list, list]:
    """(dylib install names, LC_RPATH entries) from `otool -l`."""
    dylibs, rpaths = [], []
    current = None
    for raw in _run(["otool", "-l", str(path)]).splitlines():
        line = raw.strip()
        if line.startswith("cmd "):
            current = line.split()[1]
        elif line.startswith("name ") and current in ("LC_LOAD_DYLIB", "LC_LOAD_WEAK_DYLIB",
                                                       "LC_REEXPORT_DYLIB",
                                                       "LC_LAZY_LOAD_DYLIB"):
            dylibs.append(line.split()[1])
        elif line.startswith("path ") and current == "LC_RPATH":
            rpaths.append(line.split()[1])
    return dylibs, rpaths


def is_macho(path: Path) -> bool:
    try:
        with path.open("rb") as f:
            magic = f.read(4)
    except OSError:
        return False
    return magic in (b"\xcf\xfa\xed\xfe", b"\xce\xfa\xed\xfe", b"\xca\xfe\xba\xbe",
                     b"\xbe\xba\xfe\xca", b"\xfe\xed\xfa\xcf", b"\xfe\xed\xfa\xce")


def _resolve(name: str, loader: Path, executable: Path, rpaths: list) -> Path | None:
    def expand(p: str) -> str:
        return (p.replace("@loader_path", str(loader.parent))
                 .replace("@executable_path", str(executable.parent)))

    if name.startswith("@rpath/"):
        rest = name[len("@rpath/"):]
        for rpath in rpaths:
            candidate = Path(expand(rpath)) / rest
            if candidate.exists():
                return candidate.resolve()
        return None
    candidate = Path(expand(name))
    return candidate.resolve() if candidate.exists() else None


def image_set(main: Path, extra: Iterable[Path] = ()) -> list:
    """`main`, `extra` and every non-system library they load, transitively."""
    seen: dict = {}
    queue = [(main.resolve(), [])] + [(p.resolve(), []) for p in extra]
    while queue:
        image, inherited = queue.pop(0)
        if image in seen:
            continue
        seen[image] = True
        dylibs, rpaths = _load_commands(image)
        search = rpaths + inherited
        for name in dylibs:
            if name.startswith(SYSTEM_PREFIXES):
                continue
            resolved = _resolve(name, image, main, search)
            if resolved is None:
                raise CensusError(f"{image}: cannot resolve {name}")
            # A dependency inherits its loader's rpaths, as dyld searches them.
            queue.append((resolved, [r.replace("@loader_path", str(image.parent))
                                     for r in search]))
    return list(seen)


def bundle_images(bundle: Path) -> tuple[Path, list]:
    """The bundle's main executable and every other Mach-O image inside it."""
    macos = bundle / "Contents" / "MacOS"
    mains = sorted(p for p in macos.iterdir() if p.is_file() and is_macho(p)) if macos.is_dir() else []
    if not mains:
        raise CensusError(f"{bundle}: no executable in Contents/MacOS")
    others = sorted(p for p in bundle.rglob("*")
                    if p.is_file() and p not in mains and is_macho(p))
    return mains[0], others


def census(artifact: Path,
           reader: Callable[[Path], Symbols] = read_symbols,
           expand: Callable[..., list] = image_set) -> ArtifactReport:
    if not artifact.exists():
        raise CensusError(f"{artifact}: does not exist")
    is_bundle = artifact.is_dir() and artifact.suffix in BUNDLE_SUFFIXES
    if is_bundle:
        main, others = bundle_images(artifact)
    else:
        if not is_macho(artifact):
            raise CensusError(f"{artifact}: not a Mach-O image")
        main, others = artifact, []
    images = [classify(str(p), reader(p)) for p in expand(main, others)]
    return judge(str(artifact), str(main.resolve()), artifact.suffix in PLUGIN_SUFFIXES, images)


def render(reports: list) -> str:
    lines = []
    for r in reports:
        lines.append(f"{r.verdict}  {r.artifact}: {len(r.dawn_images)} Dawn image(s) in "
                     f"{len(r.images)} image(s)")
        for image in r.images:
            if image.defines_dawn or image.installs_procs or image.is_bootstrap:
                flags = [f for f, on in (("defines Dawn", image.defines_dawn),
                                         ("exports it", image.exports_dawn),
                                         ("installs procs", image.installs_procs),
                                         ("bootstrap", image.is_bootstrap)) if on]
                lines.append(f"      {image.path}: {', '.join(flags)} "
                             f"({image.dawn_symbols} Dawn symbols)")
        for reason in r.reasons:
            lines.append(f"      - {reason}")
    return "\n".join(lines)


def to_json(reports: list) -> list:
    out = []
    for r in reports:
        d = dataclasses.asdict(r)
        d["dawn_images"] = r.dawn_images
        out.append(d)
    return out


def main(argv: list | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("artifacts", nargs="+", type=Path)
    parser.add_argument("--json", type=Path, help="also write the reports as JSON here")
    args = parser.parse_args(argv)
    if sys.platform != "darwin":
        print("dawn-binding-census: reads Mach-O images; run it on macOS", file=sys.stderr)
        return 2
    reports = []
    try:
        for artifact in args.artifacts:
            reports.append(census(artifact))
    except CensusError as error:
        print(f"dawn-binding-census: {error}", file=sys.stderr)
        return 2
    print(render(reports))
    if args.json:
        args.json.write_text(json.dumps(to_json(reports), indent=2) + "\n", encoding="utf-8")
    return 1 if any(r.verdict == "FAIL" for r in reports) else 0


if __name__ == "__main__":
    sys.exit(main())
