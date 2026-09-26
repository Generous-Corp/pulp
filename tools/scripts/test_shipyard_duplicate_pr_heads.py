#!/usr/bin/env python3
import json
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools/scripts/shipyard_duplicate_pr_heads.py"


def row(number, sha="sha", head_repo="Generous-Corp/pulp", base_repo="Generous-Corp/pulp", base_ref="main"):
    return {"number": number, "head": {"sha": sha, "repo": {"full_name": head_repo}},
            "base": {"ref": base_ref, "repo": {"full_name": base_repo}}}


class DuplicateHeadTests(unittest.TestCase):
    def run_check(self, rows):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "pulls.json"
            path.write_text(json.dumps(rows), encoding="utf-8")
            return subprocess.run(["python3", str(SCRIPT), "--pulls", str(path)],
                                  text=True, capture_output=True)

    def test_same_head_and_base_is_duplicate(self):
        result = self.run_check([row(8846), row(8851)])
        self.assertEqual(result.returncode, 1)
        self.assertEqual(json.loads(result.stdout)["duplicates"][0]["pull_requests"], [8846, 8851])

    def test_same_sha_with_different_base_is_separate(self):
        result = self.run_check([row(1), row(2, base_ref="develop")])
        self.assertEqual(result.returncode, 0)

    def test_same_branch_name_from_different_repo_is_separate(self):
        result = self.run_check([row(1, head_repo="alice/pulp"), row(2, head_repo="bob/pulp")])
        self.assertEqual(result.returncode, 0)

    def test_missing_head_fails_closed(self):
        result = self.run_check([{"number": 1}])
        self.assertEqual(result.returncode, 2)
        self.assertEqual(json.loads(result.stdout)["status"], "invalid_census")

    def test_output_file_matches_stdout(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "pulls.json"
            output = pathlib.Path(directory) / "duplicates.json"
            source.write_text(json.dumps([row(1)]), encoding="utf-8")
            result = subprocess.run(["python3", str(SCRIPT), "--pulls", str(source), "--output", str(output)],
                                    text=True, capture_output=True)
            self.assertEqual(result.returncode, 0)
            self.assertEqual(json.loads(output.read_text()), json.loads(result.stdout))


if __name__ == "__main__":
    unittest.main()
