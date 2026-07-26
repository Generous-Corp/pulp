#!/usr/bin/env python3
"""Tests for check_bundle_relocatable.py (the bundle self-containment guard).

Tests the pure decision logic (no otool / no real binaries needed), which is the
part that encodes the rule. The otool front-end is a thin parser over it.
"""
import os
import sys
import tempfile
from unittest import mock

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
    assert cb.is_toolchain_runtime(
        "@rpath/libclang_rt.asan_osx_dynamic.dylib") is True
    assert cb.is_toolchain_runtime("@rpath/libwgpu_native.dylib") is False

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

    # 6. The otool front-end must preserve that bundle boundary. This guards
    #    against accidentally weakening the pure resolver at its call site.
    binary = macos + "/Plugin"
    with mock.patch.object(
        cb, "_otool_rpaths", return_value=["@loader_path/../../../escape"]
    ), mock.patch.object(
        cb, "_otool_deps", return_value=["@rpath/libwgpu_native.dylib"]
    ), mock.patch.object(cb.os.path, "exists", return_value=True):
        _external, unresolved, _toolchain = cb.check_binary(binary, bundle)
    assert unresolved == ["@rpath/libwgpu_native.dylib"]

    # 7. A sanitizer runtime is a hard unresolved dependency by default and is
    #    softened only when the caller explicitly marks a test-only build.
    sanitizer = "@rpath/libclang_rt.asan_osx_dynamic.dylib"
    with mock.patch.object(cb, "_otool_rpaths", return_value=[]), \
         mock.patch.object(cb, "_otool_deps", return_value=[sanitizer]):
        _external, unresolved, toolchain = cb.check_binary(binary, bundle)
        assert unresolved == [sanitizer] and toolchain == []
        _external, unresolved, toolchain = cb.check_binary(
            binary, bundle, allow_toolchain_runtime=True
        )
        assert unresolved == [] and toolchain == [sanitizer]

    # 8. Relative and trailing-slash bundle inputs normalize to the same
    #    absolute boundary used by binary candidate paths.
    with tempfile.TemporaryDirectory(dir=os.getcwd()) as real_bundle:
        relative_bundle = os.path.relpath(real_bundle, os.getcwd()) + os.sep
        target, root = cb.bundle_context(relative_bundle)
        assert target == os.path.abspath(real_bundle)
        assert root == os.path.abspath(real_bundle)

    print("OK — check_bundle_relocatable.py: 9 logic groups passed")


if __name__ == "__main__":
    main()
