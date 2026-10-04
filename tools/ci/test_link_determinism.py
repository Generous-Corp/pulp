#!/usr/bin/env python3
"""A test executable's bytes must depend only on its inputs.

Apple's linker lays out ObjC selector stubs (__TEXT,__objc_stubs) differently
from one link of identical objects to the next once a binary carries enough
of them, so two builds of one tree used to produce test executables that
differed there and nowhere else. tools/cmake/PulpTestSuite.cmake links every
test executable and module with the deterministic stub form when the linker
has it. This relinks a real test executable from its own Ninja link command
(the objects already built) several times and requires one hash.

It fails when the link line lacks the option the configure chose, and when
the relinks disagree. It skips (exit 77) only when the configure found the
linker without the option, naming that linker so a toolchain that drops the
flag is visible rather than silent.

    test_link_determinism.py --build-dir B --target pulp-test-standalone-rt [--links 12]

Its pure parts run without a build: `python3 tools/ci/test_link_determinism.py`.
"""
from __future__ import annotations

import argparse
import hashlib
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SKIP = 77
DECISION = "pulp-test-link-determinism.txt"
_LAUNCHER = re.compile(r"^/bin/sh \S*link-members-launcher\.sh \S+ ")


def read_decision(build_dir: Path) -> dict[str, str] | None:
    try:
        text = (build_dir / DECISION).read_text(encoding="utf-8")
    except OSError:
        return None
    return dict(line.split("=", 1) for line in text.splitlines() if "=" in line)


def link_command(ninja_commands: str, target_output: str) -> str:
    """The bare link command for `target_output` from `ninja -t commands`:
    the last line, without the leading `: && `, the link-members launcher, or
    the post-link steps after the first ` && `."""
    line = next(l for l in reversed(ninja_commands.splitlines()) if f" -o {target_output}" in l)
    line = line.removeprefix(": && ")
    line = _LAUNCHER.sub("", line)
    return line.split(" && ", 1)[0]


def relink_hashes(command: str, target_output: str, build_dir: Path, links: int) -> list[str]:
    hashes = []
    with tempfile.TemporaryDirectory() as tmp:
        for i in range(links):
            # Same file name every time: the ad-hoc signature's identifier is
            # the output's basename, so a different name is a different binary.
            out = Path(tmp) / str(i) / Path(target_output).name
            out.parent.mkdir()
            cmd = command.replace(f" -o {target_output}", f" -o {out}")
            proc = subprocess.run(cmd, shell=True, cwd=build_dir, capture_output=True, text=True)
            if proc.returncode != 0:
                raise RuntimeError(f"relink {i} failed: {proc.stderr.strip()[-500:]}")
            hashes.append(hashlib.sha256(out.read_bytes()).hexdigest())
    return hashes


def run(build_dir: Path, target: str, links: int) -> int:
    decision = read_decision(build_dir)
    if decision is None:
        print(f"link-determinism: {DECISION} missing from {build_dir}: the configure did not decide", file=sys.stderr)
        return 1
    options = decision.get("options", "")
    if not options:
        print(f"link-determinism: SKIP: the linker ({decision.get('linker') or 'unknown version'}) has no "
              "-objc_stubs_small, so test links are not deterministic on this toolchain")
        return SKIP
    output = f"test/{target}"
    commands = subprocess.run(["ninja", "-C", str(build_dir), "-t", "commands", output],
                              capture_output=True, text=True)
    if commands.returncode != 0:
        print(f"link-determinism: ninja -t commands {output} failed: {commands.stderr.strip()}", file=sys.stderr)
        return 1
    command = link_command(commands.stdout, output)
    if options not in command.split():
        print(f"link-determinism: FAIL: {target}'s link line lacks {options}", file=sys.stderr)
        return 1
    hashes = relink_hashes(command, output, build_dir, links)
    distinct = sorted(set(hashes))
    print(f"link-determinism: {target}: {links} relinks, {len(distinct)} distinct hash(es) "
          f"with {options} ({decision.get('linker')})")
    if len(distinct) != 1:
        print(f"link-determinism: FAIL: {', '.join(h[:12] for h in distinct)}", file=sys.stderr)
        return 1
    return 0


class PureTests(unittest.TestCase):
    COMMANDS = (": && /usr/bin/c++ -O3 -c a.cpp -o a.o\n"
                ": && /bin/sh /src/tools/cmake/../ci/link-members-launcher.sh /b /usr/bin/c++ -O3 a.o "
                "-Wl,-objc_stubs_small -o test/pulp-test-x  lib.a && cd /b/test && /usr/bin/cmake -P add.cmake\n")

    def test_the_link_command_is_bare(self) -> None:
        self.assertEqual(link_command(self.COMMANDS, "test/pulp-test-x"),
                         "/usr/bin/c++ -O3 a.o -Wl,-objc_stubs_small -o test/pulp-test-x  lib.a")

    def test_the_decision_file_is_read(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / DECISION).write_text("options=-Wl,-objc_stubs_small\nlinker=PROJECT:ld-1\n")
            self.assertEqual(read_decision(Path(tmp)), {"options": "-Wl,-objc_stubs_small", "linker": "PROJECT:ld-1"})
            self.assertIsNone(read_decision(Path(tmp) / "missing"))

    def test_an_unsupported_linker_skips_naming_itself(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / DECISION).write_text("options=\nlinker=PROJECT:ld-9\n")
            self.assertEqual(run(Path(tmp), "pulp-test-x", 2), SKIP)

    def test_a_missing_decision_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(run(Path(tmp), "pulp-test-x", 2), 1)

    def test_the_helper_applies_the_option_to_every_test_executable_and_module(self) -> None:
        text = (Path(__file__).resolve().parents[2] / "tools/cmake/PulpTestSuite.cmake").read_text()
        self.assertIn('check_linker_flag(CXX "-Wl,-objc_stubs_small"', text)
        body = text.split("function(_pulp_test_link_process_directory", 1)[1].split("endfunction()", 1)[0]
        self.assertIn('"EXECUTABLE"', body)
        self.assertIn('"MODULE_LIBRARY"', body)
        self.assertIn("target_link_options(${_t} PRIVATE ${PULP_TEST_DETERMINISTIC_LINK_OPTIONS})", body)


def main(argv: list[str]) -> int:
    if len(argv) == 1:
        return 0 if unittest.main(argv=argv[:1], exit=False).result.wasSuccessful() else 1
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--build-dir", required=True)
    ap.add_argument("--target", required=True)
    ap.add_argument("--links", type=int, default=12)
    a = ap.parse_args(argv[1:])
    return run(Path(a.build_dir).resolve(), a.target, a.links)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
