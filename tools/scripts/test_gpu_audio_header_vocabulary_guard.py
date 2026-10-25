#!/usr/bin/env python3
"""Selftest for gpu_audio_header_vocabulary_guard.py."""

from __future__ import annotations

import importlib.util
import pathlib
import tempfile
import unittest

SCRIPT = pathlib.Path(__file__).with_name("gpu_audio_header_vocabulary_guard.py")
SPEC = importlib.util.spec_from_file_location("gpu_audio_header_vocabulary_guard", SCRIPT)
guard = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(guard)


class HeaderVocabularyGuardTest(unittest.TestCase):
    def write(self, root: pathlib.Path, name: str, text: str) -> None:
        path = root / "pulp" / "gpu_audio" / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def test_real_headers_are_backend_neutral(self) -> None:
        # Control: the guard reads the shipped headers and finds them clean.
        count, hits = guard.scan(guard.DEFAULT_INCLUDE)
        self.assertGreater(count, 0)
        self.assertEqual(hits, [])

    def test_planted_backend_types_fail(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            self.write(root, "clean.hpp", "struct Clean {};\n")
            self.write(root, "device.hpp", "#include <webgpu/webgpu_cpp.h>\nwgpu::Device device;\n")
            self.write(root, "native.hpp", "void f(dawn::native::Instance&);\n")
            self.write(root, "c_api.hpp", "WGPUDevice raw = nullptr;\n")
            self.assertEqual(guard.main(["--include-root", str(root)]), 1)
            _, hits = guard.scan(root)
            self.assertEqual(sorted(h.split(":")[0] for h in hits),
                             ["pulp/gpu_audio/c_api.hpp", "pulp/gpu_audio/device.hpp",
                              "pulp/gpu_audio/native.hpp"])

    def test_prose_mentioning_a_backend_passes(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            self.write(root, "doc.hpp", "// Uses wgpu and dawn::native internally.\n"
                                        "/* WGPUDevice is never exposed. */\nstruct Node {};\n")
            self.assertEqual(guard.main(["--include-root", str(root)]), 0)

    def test_empty_root_is_an_error_not_a_pass(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(guard.main(["--include-root", tmp]), 2)


if __name__ == "__main__":
    unittest.main()
