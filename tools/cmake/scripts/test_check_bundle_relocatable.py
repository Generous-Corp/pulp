#!/usr/bin/env python3
"""Tests for check_bundle_relocatable.py (the bundle self-containment guard).

Tests the pure decision logic (no otool / no real binaries needed), which is the
part that encodes the rule. The otool front-end is a thin parser over it.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import check_bundle_relocatable as cb  # noqa: E402


def main():
    # ── is_external_rpath ──
    assert cb.is_external_rpath("@loader_path") is False
    assert cb.is_external_rpath("@executable_path/../Frameworks") is False
    assert cb.is_external_rpath("@rpath") is False
    assert cb.is_external_rpath("/usr/lib") is False
    assert cb.is_external_rpath("/System/Library/Frameworks") is False
    # The actual footgun: an rpath into the build cache / fetchcontent / home.
    assert cb.is_external_rpath(
        "/Users/dev/Library/Caches/Pulp/fetchcontent-src/wgpu-macos-aarch64/lib") is True
    assert cb.is_external_rpath("/Volumes/Workshop/Code/pulp/build") is True
    assert cb.is_external_rpath("/opt/homebrew/lib") is True

    bundle = "/X/SuperConvolver.clap"
    macos = bundle + "/Contents/MacOS"
    bundled = {macos + "/libwgpu_native.dylib"}
    exists = lambda p: p in bundled  # noqa: E731

    # 1. @rpath dep + @loader_path rpath + dylib present in bundle → resolves.
    assert cb.unresolved_rpath_deps(
        ["@rpath/libwgpu_native.dylib"], ["@loader_path"],
        macos, macos, bundle, exists) == []

    # 2. @rpath dep but the ONLY rpath is the external cache dir → unresolved
    #    (this is exactly the v1.0.0/1.0.1 bug).
    assert cb.unresolved_rpath_deps(
        ["@rpath/libwgpu_native.dylib"],
        ["/Users/dev/Library/Caches/Pulp/fetchcontent-src/wgpu/lib"],
        macos, macos, bundle, exists) == ["@rpath/libwgpu_native.dylib"]

    # 3. @loader_path rpath but the dylib isn't actually in the bundle → unresolved.
    assert cb.unresolved_rpath_deps(
        ["@rpath/libmissing.dylib"], ["@loader_path"],
        macos, macos, bundle, exists) == ["@rpath/libmissing.dylib"]

    # 4. Non-@rpath deps (absolute system libs) are ignored.
    assert cb.unresolved_rpath_deps(
        ["/usr/lib/libSystem.B.dylib"], ["@loader_path"],
        macos, macos, bundle, exists) == []

    # 5. A loader rpath that resolves OUTSIDE the bundle root doesn't count as
    #    in-bundle (defends against @loader_path/../../somewhere escapes).
    assert cb.unresolved_rpath_deps(
        ["@rpath/libwgpu_native.dylib"], ["@loader_path/../../../escape"],
        macos, macos, bundle, lambda p: True) == ["@rpath/libwgpu_native.dylib"]

    print("OK — check_bundle_relocatable.py: 5 logic groups passed")


if __name__ == "__main__":
    main()
