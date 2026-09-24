#!/usr/bin/env python3
"""Leg orchestration tests for test/cmake/test_ios_compile_gate.sh.

The gate runs its GPU leg in the background while the two SDK legs build, and
picks Ninja for the SDK legs when it is available. Both are control-flow
properties, so these tests run the real script against stub `cmake`, `xcrun`,
`ninja` and `python3` executables that record what they were asked to do and
create the artifacts the gate checks for. Nothing is compiled.

What must hold:
- a failing GPU leg fails the gate even though the SDK legs passed;
- a failing SDK leg fails the gate and does not leave the GPU leg running;
- the SDK legs configure with Ninja when it is on PATH and with Xcode when it
  is not, and the GPU leg always configures with Xcode;
- the SDK-leg checks accept Ninja's object output for the OBJECT-library
  shared-client contract, and still fail when it is absent.

Run:
    python3 tools/scripts/test_ios_compile_gate_legs.py
"""
from __future__ import annotations

import os
import stat
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
GATE = REPO_ROOT / "test" / "cmake" / "test_ios_compile_gate.sh"

STATIC_LIBS = (
    "pulp-timebase",
    "pulp-timeline",
    "pulp-playback",
    "pulp-sequence",
    "pulp-smf-interop",
    "pulp-smf-interchange",
    "pulp-midi",
)

# `cmake` stub. Configure (-S/-B): record the generator, write a CMakeCache.
# Build (--build): create the artifacts the gate looks for. STUB_FAIL selects
# a failing step by a substring of the build directory and a mode.
CMAKE_STUB = r"""#!/usr/bin/env bash
set -u
log="$STUB_LOG"
if [ "${1:-}" = "--build" ]; then
    dir="$2"
    echo "build $dir" >>"$log"
    case "${STUB_FAIL:-}" in
        "build:"*) pat="${STUB_FAIL#build:}"; case "$dir" in *"$pat") exit 3;; esac;;
    esac
    case "$dir" in
        *-gpu)
            if [ -n "${STUB_GPU_SLEEP:-}" ]; then sleep "$STUB_GPU_SLEEP"; fi
            mkdir -p "$dir/AUv3/PulpGpuSmoke.appex";;
        *)
            for lib in __LIBS__; do
                mkdir -p "$dir/lib"; : >"$dir/lib/lib$lib.a"
            done
            if [ -z "${STUB_OMIT_CONTRACT:-}" ]; then
                mkdir -p "$dir/obj"; : >"$dir/obj/test_coremidi_shared_client.cpp.o"
            fi
            mkdir -p "$dir/core/midi/PulpCoreMidiHarness.app";;
    esac
    exit 0
fi
src= bdir= gen=
while [ $# -gt 0 ]; do
    case "$1" in
        -S) src="$2"; shift 2;;
        -B) bdir="$2"; shift 2;;
        -G) gen="$2"; shift 2;;
        *) shift;;
    esac
done
echo "configure $bdir $gen" >>"$log"
case "${STUB_FAIL:-}" in
    "configure:"*) pat="${STUB_FAIL#configure:}"; case "$bdir" in *"$pat") exit 4;; esac;;
esac
mkdir -p "$bdir"
{
    echo "PULP_HAS_SKIA:INTERNAL=TRUE"
    echo "FETCHCONTENT_SOURCE_DIR_CHOC:PATH=$bdir/choc"
} >"$bdir/CMakeCache.txt"
mkdir -p "$bdir/choc/choc/containers"
exit 0
""".replace("__LIBS__", " ".join(STATIC_LIBS))

XCRUN_STUB = r"""#!/usr/bin/env bash
case "$*" in
    *"simctl list"*) echo '{"devices": {}}';;
    *"--show-sdk-path"*) echo /stub/sdk;;
    *) : ;;
esac
exit 0
"""

# python3 stub: the Skia slice fetch creates the library the GPU leg checks;
# anything else (the simulator picker) delegates to the real interpreter.
PYTHON_STUB = r"""#!/usr/bin/env bash
case "${1:-}" in
    *fetch_skia_for_release.py)
        dest=
        while [ $# -gt 0 ]; do
            [ "$1" = "--dest" ] && dest="$2"
            shift
        done
        mkdir -p "$dest/build/ios-gpu/lib/Release/simulator-arm64"
        : >"$dest/build/ios-gpu/lib/Release/simulator-arm64/libskia.a"
        exit 0;;
esac
exec "$REAL_PYTHON3" "$@"
"""


def _write_exe(path: Path, body: str) -> None:
    path.write_text(body, encoding="utf-8")
    path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


class GateHarness:
    def __init__(self, tmp: Path, *, with_ninja: bool) -> None:
        self.tmp = tmp
        self.bin = tmp / "bin"
        self.bin.mkdir()
        self.log = tmp / "calls.log"
        self.build_root = tmp / "build-ios"
        _write_exe(self.bin / "cmake", CMAKE_STUB)
        _write_exe(self.bin / "xcrun", XCRUN_STUB)
        _write_exe(self.bin / "python3", PYTHON_STUB)
        if with_ninja:
            _write_exe(self.bin / "ninja", "#!/usr/bin/env bash\nexit 0\n")

    def run(self, **env_overrides: str) -> subprocess.CompletedProcess[str]:
        real_python = subprocess.run(
            ["/usr/bin/env", "python3", "-c", "import sys; print(sys.executable)"],
            capture_output=True, text=True, check=True,
        ).stdout.strip()
        env = {
            "PATH": f"{self.bin}:/usr/bin:/bin:/usr/sbin:/sbin",
            "HOME": str(self.tmp),
            "STUB_LOG": str(self.log),
            "REAL_PYTHON3": real_python,
            # The real governor would ask tartci for a lease; keep it local.
            "PULP_TARTCI_BIN": "",
        }
        env.update(env_overrides)
        return subprocess.run(
            ["bash", str(GATE), str(REPO_ROOT), str(self.build_root)],
            capture_output=True, text=True, env=env, timeout=120, cwd=self.tmp,
        )

    def calls(self) -> list[str]:
        if not self.log.exists():
            return []
        return self.log.read_text(encoding="utf-8").splitlines()


@unittest.skipUnless(os.uname().sysname == "Darwin", "the gate targets macOS hosts")
class GateLegTests(unittest.TestCase):
    def harness(self, *, with_ninja: bool = True) -> GateHarness:
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        return GateHarness(Path(tmp.name), with_ninja=with_ninja)

    def configures(self, harness: GateHarness) -> dict[str, str]:
        result = {}
        for line in harness.calls():
            parts = line.split()
            if parts[0] == "configure":
                result[Path(parts[1]).name] = parts[2]
        return result

    def test_all_legs_pass(self) -> None:
        harness = self.harness()
        proc = harness.run()
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("OK: iOS GPU smoke example compiled", proc.stdout)
        builds = [c for c in harness.calls() if c.startswith("build ")]
        self.assertEqual(len(builds), 3, harness.calls())

    def test_sdk_legs_use_ninja_and_gpu_leg_uses_xcode(self) -> None:
        harness = self.harness(with_ninja=True)
        proc = harness.run()
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(
            self.configures(harness),
            {
                "iphonesimulator": "Ninja",
                "iphoneos": "Ninja",
                "iphonesimulator-gpu": "Xcode",
            },
        )

    def test_sdk_legs_fall_back_to_xcode_without_ninja(self) -> None:
        harness = self.harness(with_ninja=False)
        proc = harness.run()
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(
            set(self.configures(harness).values()), {"Xcode"}, harness.calls()
        )

    def test_gpu_leg_failure_fails_the_gate(self) -> None:
        for mode in ("configure:-gpu", "build:-gpu"):
            with self.subTest(mode=mode):
                harness = self.harness()
                proc = harness.run(STUB_FAIL=mode)
                self.assertNotEqual(proc.returncode, 0, proc.stdout)
                self.assertIn("iOS GPU leg failed", proc.stderr)
                # The SDK legs still ran to completion alongside it.
                sdk_builds = [
                    c for c in harness.calls()
                    if c.startswith("build ") and not c.endswith("-gpu")
                ]
                self.assertEqual(len(sdk_builds), 2, harness.calls())

    def test_sdk_leg_failure_fails_the_gate_and_stops_the_gpu_leg(self) -> None:
        harness = self.harness()
        proc = harness.run(STUB_FAIL="build:iphonesimulator", STUB_GPU_SLEEP="30")
        self.assertNotEqual(proc.returncode, 0, proc.stdout)
        self.assertIn("iphonesimulator Release target build failed", proc.stderr)
        # The background leg was sleeping inside its build; the gate must not
        # have waited for it (it would take 30 s) or left it behind.
        # A terminated process can take a moment to be reaped; poll briefly.
        import time

        deadline = time.monotonic() + 10
        while True:
            leftover = subprocess.run(
                ["pgrep", "-f", str(harness.build_root)],
                capture_output=True, text=True,
            ).stdout.strip()
            if not leftover or time.monotonic() > deadline:
                break
            time.sleep(0.2)
        self.assertEqual(leftover, "", "GPU leg left running")

    def test_missing_shared_client_contract_fails(self) -> None:
        harness = self.harness()
        proc = harness.run(STUB_OMIT_CONTRACT="1")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("did not compile the CoreMIDI shared-client contract", proc.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
