#!/usr/bin/env python3
"""Behavioral coverage for example-validation path classification."""

from __future__ import annotations

import unittest

import example_validation_paths as paths


class ExampleValidationPathTests(unittest.TestCase):
    def test_example_affecting_surfaces(self) -> None:
        watched = (
            "examples/PulpTempoSampler/CMakeLists.txt",
            "external/miniz/miniz.h",
            "core/state/include/pulp/state/parameter.hpp",
            "core/signal/include/pulp/signal/fft.hpp",
            "core/audio/include/pulp/audio/processor.hpp",
            "core/view/include/pulp/view/view.hpp",
            "core/timeline/include/pulp/timeline/project.hpp",
            "core/playback/include/pulp/playback/transport.hpp",
            "core/format/src/clap_adapter.cpp",
            "core/view/cmake/PulpViewSources.cmake",
            "core/timeline/PulpTimelineSources.cmake",
            "core/playback/PulpPlaybackSources.cmake",
            "tools/cmake/PulpUtils.cmake",
            "tools/deps/manifest.json",
            ".github/actions/install-linux-build-deps/action.yml",
            ".github/workflows/examples-validation.yml",
            "test/au_bundle_lifecycle.cpp",
            "test/clap_bundle_lifecycle.cpp",
            "test/vst3_bundle_lifecycle.cpp",
            "tools/ci/linux_build_deps.json",
            "tools/ci/install_linux_build_deps.py",
            "tools/ci/lib/auval-exec-check.sh",
            "tools/ci/run-auval-component.sh",
            "tools/ci/governed-build.sh",
            "setup.sh",
            "tools/scripts/fetch_skia_for_release.py",
        )
        for path in watched:
            with self.subTest(path=path):
                self.assertTrue(paths.affects_examples(path))

    def test_unrelated_surfaces_are_not_example_affecting(self) -> None:
        unrelated = (
            "docs/guides/local-ci.md",
            "test/cmake/quality_tests.cmake",
            "core/signal/src/fft.cpp",
            "tools/scripts/unrelated.py",
        )
        for path in unrelated:
            with self.subTest(path=path):
                self.assertFalse(paths.affects_examples(path))


if __name__ == "__main__":
    unittest.main()
