#!/usr/bin/env python3
"""Compile and link the DSP buffer at Android's supported API floor and API 28."""

import argparse
from pathlib import Path
import subprocess
import tempfile


SOURCE = r"""
#include <pulp/signal/simd_buffer.hpp>
#include <cstdint>
int main() {
    pulp::signal::AlignedBufferT<float> floats(17);
    pulp::signal::AlignedBufferT<double> doubles(19);
    floats.resize(65);
    doubles.resize(67);
    return !floats.data() || !doubles.data()
        || reinterpret_cast<std::uintptr_t>(floats.data()) % 64
        || reinterpret_cast<std::uintptr_t>(doubles.data()) % 64;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ndk", type=Path, required=True)
    args = parser.parse_args()
    prebuilt = [path for path in (args.ndk / "toolchains/llvm/prebuilt").glob("*/bin/clang++*")
                if path.name in {"clang++", "clang++.exe"}]
    if len(prebuilt) != 1:
        parser.error(f"expected one NDK host compiler, found {len(prebuilt)}")
    compiler = prebuilt[0]
    sysroot = compiler.parent.parent / "sysroot"
    root = Path(__file__).resolve().parents[2]
    failed = False
    with tempfile.TemporaryDirectory(prefix="pulp-android-aligned-") as directory:
        path = Path(directory)
        source = path / "buffer.cpp"
        source.write_text(SOURCE)
        for abi in ("aarch64", "x86_64"):
            for api in (26, 28):
                target = f"{abi}-linux-android{api}"
                result = subprocess.run([
                    str(compiler), f"--target={target}", f"--sysroot={sysroot}",
                    "-std=c++20", "-O2", "-I", str(root / "core/signal/include"),
                    str(source), "-o", str(path / target),
                ], capture_output=True, text=True)
                print(f"{target}: {'PASS' if result.returncode == 0 else 'FAIL'}")
                if result.returncode:
                    print(result.stderr)
                    failed = True
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
