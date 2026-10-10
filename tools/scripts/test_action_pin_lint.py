#!/usr/bin/env python3
import tempfile
import unittest
from pathlib import Path

from action_pin_lint import violations


class ActionPinLintTests(unittest.TestCase):
    def check(self, text: str):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "workflow.yml"
            path.write_text(text, encoding="utf-8")
            return violations([path])

    def test_tag_pin_is_a_failure_control(self):
        errors = self.check("""steps:\n  - uses: dorny/paths-filter@v3\n""")
        self.assertTrue(errors)
        self.assertIn("full commit SHA", errors[0])

    def test_sha_pin_with_release_comment_passes(self):
        self.assertEqual(
            self.check("""steps:\n  - uses: dorny/paths-filter@0e4a8c6effa4802afeda77dc8d303f8176d7dfad # v3.0.4\n"""),
            [],
        )

    def test_first_party_and_local_actions_are_exempt(self):
        self.assertEqual(
            self.check("""steps:\n  - uses: actions/checkout@v5\n  - uses: github/codeql-action/init@v3\n  - uses: ./.github/actions/retry\n"""),
            [],
        )

    def test_missing_release_comment_is_a_failure(self):
        errors = self.check("""steps:\n  - uses: dorny/paths-filter@0e4a8c6effa4802afeda77dc8d303f8176d7dfad\n""")
        self.assertTrue(any("release comment" in error for error in errors))


if __name__ == "__main__":
    unittest.main(verbosity=2)
