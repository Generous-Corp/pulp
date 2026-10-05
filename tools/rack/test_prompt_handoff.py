#!/usr/bin/env python3
"""The generators read a prompt handed over by file exactly as an argument."""
from __future__ import annotations

import contextlib
import io
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import prompt_handoff  # noqa: E402


class ResolveTest(unittest.TestCase):
    def setUp(self) -> None:
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.dir = tmp.name

    def write(self, data: bytes) -> str:
        path = os.path.join(self.dir, "run.prompt")
        with open(path, "wb") as f:
            f.write(data)
        return path

    def test_the_file_takes_the_prompts_place_and_is_removed(self) -> None:
        text = "don't stop 音 \U0001f3b9 $X `id` \n" * 3
        path = self.write(text.encode("utf-8"))
        argv = ["patch.py", "build", "--prompt-file", path, "--retries", "1"]
        self.assertEqual(prompt_handoff.resolve(argv, 2),
                         ["patch.py", "build", text, "--retries", "1"])
        self.assertFalse(os.path.exists(path))

    def test_bytes_decode_as_argv_would(self) -> None:
        raw = b"caf\xc3\xa9 and a stray \xff byte"
        path = self.write(raw)
        resolved = prompt_handoff.resolve(["generate.py", "--prompt-file", path], 1)
        self.assertEqual(resolved[1], os.fsdecode(raw))
        self.assertEqual(os.fsencode(resolved[1]), raw)

    def test_a_positional_prompt_is_left_alone(self) -> None:
        argv = ["patch.py", "build", "a prompt"]
        self.assertEqual(prompt_handoff.resolve(argv, 2), argv)

    def test_the_flag_anywhere_but_the_prompts_place_is_refused(self) -> None:
        path = self.write(b"text")
        with self.assertRaisesRegex(SystemExit, "takes the place of the prompt"):
            prompt_handoff.resolve(["patch.py", "build", "a prompt", "--prompt-file", path], 2)
        with self.assertRaisesRegex(SystemExit, "takes the place of the prompt"):
            prompt_handoff.resolve(["patch.py", "build", "--prompt-file"], 2)
        self.assertTrue(os.path.exists(path))  # nothing read, nothing removed

    def test_an_unreadable_file_is_named(self) -> None:
        missing = os.path.join(self.dir, "gone.prompt")
        with self.assertRaisesRegex(SystemExit, "cannot read the prompt file"):
            prompt_handoff.resolve(["generate.py", "--prompt-file", missing], 1)

    def test_a_nul_byte_is_refused(self) -> None:
        path = self.write(b"before\0after")
        with self.assertRaisesRegex(SystemExit, "NUL"):
            prompt_handoff.resolve(["generate.py", "--prompt-file", path], 1)


class EntryPointTest(unittest.TestCase):
    """Both generators resolve the file before anything else reads argv."""

    def setUp(self) -> None:
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.path = os.path.join(tmp.name, "run.prompt")
        with open(self.path, "w", encoding="utf-8") as f:
            f.write("a request")

    def test_patch_build_resolves_the_prompt_file(self) -> None:
        import patch
        with contextlib.redirect_stdout(io.StringIO()):
            patch.main(["patch.py", "build", "--prompt-file", self.path, "--help"])
        self.assertFalse(os.path.exists(self.path))

    def test_generate_resolves_the_prompt_file(self) -> None:
        import generate
        with contextlib.redirect_stdout(io.StringIO()), self.assertRaises(SystemExit):
            generate._main(["generate.py", "--prompt-file", self.path, "--help"],
                           contextlib.ExitStack())
        self.assertFalse(os.path.exists(self.path))


if __name__ == "__main__":
    unittest.main()
