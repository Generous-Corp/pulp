#!/usr/bin/env python3
"""external/lilv matches its recorded upstream digests, and the check notices
a one-byte change, a restored omitted file, or an unrecorded library."""
from __future__ import annotations

import shutil
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import verify_vendored_lilv as v  # noqa: E402


# The tree this test reads, named so a test selector reruns it on any change.
VENDORED = "external/lilv"


class VendoredLilvTests(unittest.TestCase):
    def test_vendored_tree_matches_the_record(self):
        self.assertEqual(v.VENDOR_DIR, v.ROOT / VENDORED)
        self.assertEqual(v.check(), [])

    def test_a_changed_byte_is_reported(self):
        with tempfile.TemporaryDirectory() as tmp:
            copy = Path(tmp) / "lilv"
            shutil.copytree(v.VENDOR_DIR, copy)
            target = copy / "serd" / "src" / "reader.c"
            data = bytearray(target.read_bytes())
            data[100] ^= 0x01
            target.write_bytes(bytes(data))
            problems = v.check(copy)
            self.assertEqual(len(problems), 1)
            self.assertIn("serd", problems[0])

    def test_an_omitted_file_and_an_unrecorded_library_are_reported(self):
        with tempfile.TemporaryDirectory() as tmp:
            copy = Path(tmp) / "lilv"
            shutil.copytree(v.VENDOR_DIR, copy)
            (copy / "sord" / "src" / "sordi.c").write_text("int main(void) { return 0; }\n")
            (copy / "extra").mkdir()
            problems = "\n".join(v.check(copy))
            self.assertIn("sordi.c", problems)
            self.assertIn("external/lilv/extra", problems)


if __name__ == "__main__":
    unittest.main()
