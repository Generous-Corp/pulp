#!/usr/bin/env python3
import json
import os
import subprocess
import shlex
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parent
TOOL = ROOT.parent.parent / "tools/scripts/prepush_claim_check.py"


class ClaimCheckTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.stub = self.dir / "gh"
        # Honor the requested connection limits and cursors so truncating the
        # production query reproduces the real missing-claim failure.
        self.stub.write_text("""import json, os, re, sys
args = dict(arg.split('=', 1) for arg in sys.argv[1:] if '=' in arg)
query = args['query']
prs = json.load(open(os.environ['PULP_CLAIM_JSON'], encoding='utf-8'))
def page(items, connection, cursor):
    spec = re.search(connection + r'\\(([^)]*)\\)', query).group(1)
    count = re.search(r'(first|last):\\s*(\\d+)', spec)
    direction, size = count.group(1), int(count.group(2))
    start = int(args.get(cursor, '0'))
    if direction == 'last':
        start = max(0, len(items) - size)
    end = min(start + size, len(items))
    return {'nodes': items[start:end], 'pageInfo': {
        'hasNextPage': end < len(items), 'endCursor': str(end)}}
def pr_node(pr):
    return {'number': pr['number'], 'comments': page(
        [{'body': body} for body in pr['comments']], 'comments', 'commentsCursor')}
if 'pullRequests(' in query:
    connection = page(prs, 'pullRequests', 'prsCursor')
    connection['nodes'] = [pr_node(pr) for pr in connection['nodes']]
    result = {'pullRequests': connection}
else:
    pr = next(pr for pr in prs if pr['number'] == int(args['number']))
    result = {'pullRequest': pr_node(pr)}
print(json.dumps({'data': {'repository': result}}))
""", encoding="utf-8")

    def tearDown(self):
        self.tmp.cleanup()

    def run_check(self, comments, *, own=None, pr=True, prs=None):
        if prs is None:
            prs = [{"number": 42, "comments": comments}] if pr else []
        data = self.dir / "response.json"
        data.write_text(json.dumps(prs), encoding="utf-8")
        env = os.environ.copy()
        env.pop("PULP_SKIP_CLAIM_CHECK", None)
        env.pop("PULP_REPAIR_CLAIM_ID", None)
        env.update(PULP_CLAIM_CHECK_GH=shlex.join([sys.executable, str(self.stub)]),
                   PULP_CLAIM_JSON=str(data))
        command = [sys.executable, str(TOOL), "--repo", "OWNER/REPO", "--branch", "feature/test"]
        if own:
            command += ["--own-claim", own]
        return subprocess.run(command, env=env, text=True, encoding="utf-8", capture_output=True)

    def test_claim_on_second_pr_refuses(self):
        result = self.run_check([], prs=[
            {"number": 41, "comments": []},
            {"number": 42, "comments": ["repair-claim: lane-a until 2099-01-02T03:04Z"]}])
        self.assertEqual(result.returncode, 1, result.stderr)

    def test_claim_on_second_pr_page_refuses(self):
        prs = [{"number": n, "comments": []} for n in range(101)]
        prs[-1]["comments"] = ["repair-claim: lane-a until 2099-01-02T03:04Z"]
        result = self.run_check([], prs=prs)
        self.assertEqual(result.returncode, 1, result.stderr)

    def test_old_live_claim_survives_comment_pagination(self):
        result = self.run_check(["repair-claim: lane-a until 2099-01-02T03:04Z"] + ["noise"] * 205)
        self.assertEqual(result.returncode, 1, result.stderr)

    def test_claim_on_later_comment_page_refuses(self):
        result = self.run_check(["noise"] * 205 + ["repair-claim: lane-a until 2099-01-02T03:04Z"])
        self.assertEqual(result.returncode, 1, result.stderr)

    def test_release_on_later_comment_page_passes(self):
        result = self.run_check(["repair-claim: lane/a until 2099-01-02T03:04Z"] +
                                ["noise"] * 205 + ["repair-claim-released: lane/a"])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("could not read", result.stderr)

    def test_named_release_preserves_other_actor_claim(self):
        for first, second in [("lane-a", "graph-lead-claude"),
                              ("lane/a", "graph/lead-claude")]:
            for released, remaining in [(first, second), (second, first)]:
                with self.subTest(released=released):
                    result = self.run_check([
                        f"repair-claim: {first} until 2099-01-02T03:04Z",
                        f"repair-claim: {second} until 2099-01-02T03:04Z",
                        f"repair-claim-released: {released} (finished)",
                    ])
                    self.assertEqual(result.returncode, 1, result.stderr)
                    self.assertIn(f"live repair-claim {remaining}", result.stderr)
                    # The released ID really disappears; it is not just that
                    # both claims remain and still produce a refusal.
                    result = self.run_check([
                        f"repair-claim: {released} until 2099-01-02T03:04Z",
                        f"repair-claim-released: {released} (finished)",
                    ])
                    self.assertEqual(result.returncode, 0, result.stderr)

    def test_malformed_named_release_preserves_claims(self):
        for release in ["(lane-a)", "lane/a/extra", "lane-a:"]:
            with self.subTest(release=release):
                result = self.run_check([
                    "repair-claim: lane-a until 2099-01-02T03:04Z",
                    f"repair-claim-released: {release}",
                ])
                self.assertEqual(result.returncode, 1, result.stderr)

    def test_malformed_timestamp_counts_as_live(self):
        result = self.run_check(["repair-claim: lane-a until 2099-XX-02T03:04Z"])
        self.assertEqual(result.returncode, 1, result.stderr)
        result = self.run_check(["repair-claim: lane-a until"])
        self.assertEqual(result.returncode, 1, result.stderr)

    def test_release_in_other_pr_does_not_clear_claim(self):
        result = self.run_check([], prs=[
            {"number": 41, "comments": ["repair-claim-released:"]},
            {"number": 42, "comments": ["repair-claim: lane-a until 2099-01-02T03:04Z"]}])
        self.assertEqual(result.returncode, 1, result.stderr)

    def run_hook_check(self, branches, held, remote_url="https://github.com/OWNER/REPO.git",
                       held_repo="OWNER/REPO"):
        hook = (ROOT.parent.parent / ".githooks" / "pre-push").read_text(encoding="utf-8")
        function = hook[hook.index("prepush_claim_check_final() {"):]
        function = function[:function.index("\n}\n") + 3]
        checker = self.dir / "checker.py"
        checker.write_text("import sys\nargs = sys.argv\nraise SystemExit(1 if "
                           "args[args.index('--branch') + 1] == " + repr(held) +
                           " and args[args.index('--repo') + 1] == " + repr(held_repo) +
                           " else 0)\n", encoding="utf-8")
        # Execute the actual hook function with isolated git/Python commands;
        # no expensive gates or real GitHub reads are part of this fixture.
        script = self.dir / "hook.sh"
        script.write_text(function + """
git() { echo https://github.com/OWNER/REPO.git; }
run_checker() { python3 "$CHECKER" "$@"; }
PULP_PYTHON=run_checker
PREPUSH_REMOTE_URL=""" + repr(remote_url) + """
prepush_claim_check_final
""", encoding="utf-8")
        records = []
        for branch, sha in branches:
            records.append(f"refs/heads/source {sha} {branch} {'0' * 40}")
        env = {**os.environ, "ROOT": str(ROOT.parent.parent), "CHECKER": str(checker),
               "PREPUSH_UPDATE_RECORDS": "\n".join(records)}
        return subprocess.run(["bash", str(script)], env=env, capture_output=True, text=True, encoding="utf-8")

    def test_hook_checks_second_branch(self):
        result = self.run_hook_check([("refs/heads/free", "a" * 40),
                                      ("refs/heads/held", "a" * 40)], "held")
        self.assertEqual(result.returncode, 1, result.stderr)

    def test_hook_all_free_branches_pass(self):
        result = self.run_hook_check([("refs/heads/free", "a" * 40),
                                      ("refs/heads/also-free", "a" * 40)], "held")
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_hook_uses_push_destination_instead_of_origin(self):
        for url in ["https://github.com/OTHER/DEST.git",
                    "git@github.com:OTHER/DEST.git"]:
            with self.subTest(url=url):
                result = self.run_hook_check([("refs/heads/held", "a" * 40)], "held",
                                             remote_url=url, held_repo="OTHER/DEST")
                self.assertEqual(result.returncode, 1, result.stderr)
                result = self.run_hook_check([("refs/heads/held", "a" * 40)], "held",
                                             remote_url=url, held_repo="OWNER/REPO")
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_hook_skips_deletions_and_tags(self):
        result = self.run_hook_check([("refs/heads/held", "0" * 40),
                                      ("refs/tags/held", "a" * 40),
                                      ("refs/heads/free", "a" * 40)], "held")
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_live_foreign_claim_refuses(self):
        result = self.run_check(["repair-claim: lane-a until 2099-01-02T03:04Z"])
        self.assertEqual(result.returncode, 1)
        self.assertIn("live repair-claim lane-a", result.stderr)

    def test_own_claim_passes(self):
        self.assertEqual(self.run_check(["repair-claim: lane-a until 2099-01-02T03:04Z"], own="lane-a").returncode, 0)

    def test_expired_claim_passes(self):
        self.assertEqual(self.run_check(["repair-claim: lane-a until 2000-01-02T03:04Z"]).returncode, 0)
        self.assertEqual(self.run_check(["repair-claim: lane-a until 2000-01-02 03:04 UTC"]).returncode, 0)
        self.assertEqual(self.run_check(["repair-claim: lane-a until 2000-01-02T03:04 UTC"]).returncode, 0)

    def test_id_release_passes(self):
        self.assertEqual(self.run_check(["repair-claim: OWNER/REPO until 2099-01-02T03:04Z", "repair-claim-released: OWNER/REPO"]).returncode, 0)

    def test_bare_release_passes(self):
        self.assertEqual(self.run_check(["repair-claim: lane-a until 2099-01-02T03:04Z", "repair-claim-released:"]).returncode, 0)

    def test_read_failure_passes_with_warning(self):
        bad = self.dir / "bad"
        bad.write_text("#!/bin/sh\necho unavailable >&2\nexit 7\n", encoding="utf-8")
        bad.chmod(0o755)
        env = os.environ.copy()
        env["PULP_CLAIM_CHECK_GH"] = str(bad)
        result = subprocess.run(["python3", str(TOOL), "--repo", "OWNER/REPO", "--branch", "feature/test"], env=env, text=True, encoding="utf-8", capture_output=True)
        self.assertEqual(result.returncode, 0)
        self.assertIn("claim check could not read", result.stderr)

    def test_malformed_response_warns_instead_of_crashing(self):
        def response(nodes):
            return {"data": {"repository": {"pullRequests": {
                "nodes": [{"number": 42, "comments": {
                    "nodes": nodes, "pageInfo": {"hasNextPage": False}}}],
                "pageInfo": {"hasNextPage": False}}}}}
        for payload in [None, [], response([None]), response([{"body": None}]),
                        response([{"body": 42}]), response([{}])]:
            with self.subTest(payload=payload):
                bad = self.dir / "malformed.py"
                bad.write_text("print(" + repr(json.dumps(payload)) + ")\n", encoding="utf-8")
                env = {k: v for k, v in os.environ.items()
                       if k not in ("PULP_SKIP_CLAIM_CHECK", "PULP_REPAIR_CLAIM_ID")}
                env["PULP_CLAIM_CHECK_GH"] = shlex.join([sys.executable, str(bad)])
                result = subprocess.run([sys.executable, str(TOOL), "--repo", "OWNER/REPO",
                                         "--branch", "feature/test"], env=env, text=True,
                                        encoding="utf-8", capture_output=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("claim check could not read", result.stderr)
                self.assertNotIn("Traceback", result.stderr)

    def test_unreadable_claim_time_counts_as_live(self):
        result = self.run_check(["repair-claim: lane-a until 2026-13-45T03:04Z"])
        self.assertEqual(result.returncode, 1)

    def run_owner_guard(self, requested_owner=None):
        # Exercise Git's actual hook entrypoint and ref update in an isolated
        # local repository. Unrelated expensive gates are absent in the fixture.
        repo = self.dir / "owner-repo"
        remote = self.dir / "destination.git"
        (repo / "tools" / "scripts").mkdir(parents=True)
        (repo / ".githooks").mkdir()
        source = ROOT / "worktree_lineage.sh"
        target = repo / "tools/scripts/worktree_lineage.sh"
        target.write_bytes(source.read_bytes())
        target.chmod(0o755)
        hook = repo / ".githooks/pre-push"
        hook.write_bytes((ROOT.parent.parent / ".githooks/pre-push").read_bytes())
        hook.chmod(0o755)
        (repo / "tools/scripts/gate_host_policy.sh").write_text(
            "pulp_gate_host_builds_reserved() { return 1; }\n", encoding="utf-8")
        env = {k: v for k, v in os.environ.items()
               if not k.startswith(("GIT_", "PULP_"))}
        env["GIT_CONFIG_NOSYSTEM"] = "1"
        env["GIT_CONFIG_GLOBAL"] = os.devnull
        def git(*args):
            return subprocess.run(["git", *args], cwd=repo, env=env,
                                  check=True, capture_output=True, text=True)
        git("init", "-q")
        git("init", "-q", "--bare", str(remote))
        git("config", "user.email", "test@example.invalid")
        git("config", "user.name", "test")
        git("config", "core.hooksPath", ".githooks")
        (repo / "marker").write_text("fixture\n", encoding="utf-8")
        git("add", "marker")
        git("commit", "-qm", "fixture")
        subprocess.run(["bash", str(target), "mark", "--status", "active", "--owner", "recorded-owner"],
                       cwd=repo, env=env, check=True, stdout=subprocess.DEVNULL)
        # This bypass exists only inside the disposable hook fixture. The real
        # repair push must execute every required gate.
        env["PULP_SKIP_PREPUSH"] = "1"
        env["PYTHON"] = sys.executable
        if requested_owner is not None:
            env["PULP_AGENT_OWNER"] = requested_owner
        result = subprocess.run(["git", "push", str(remote), "HEAD:refs/heads/test"],
                                cwd=repo, env=env, capture_output=True, text=True)
        remote_head = git("ls-remote", str(remote), "refs/heads/test").stdout.strip()
        return result, remote_head

    def test_hook_owner_guard_refuses_foreign_owner(self):
        result, remote_head = self.run_owner_guard("foreign-owner")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("ownership does not match", result.stderr)
        self.assertEqual(remote_head, "")

    def test_hook_owner_guard_allows_matching_owner(self):
        result, remote_head = self.run_owner_guard("recorded-owner")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(remote_head, result.stderr)

    def test_hook_owner_guard_allows_unset_owner(self):
        result, remote_head = self.run_owner_guard()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(remote_head, result.stderr)

    def test_hook_checks_claims_on_every_allowed_push_path(self):
        hook = (ROOT.parent.parent / ".githooks" / "pre-push").read_text(encoding="utf-8")
        skip = hook.index('prepush_receipt_note skipped all-gates "PULP_SKIP_PREPUSH=1"')
        self.assertIn("prepush_claim_check_final", hook[skip:hook.index("exit 0", skip)])
        clean = hook.index('echo "[pre-push] gates clean." >&2')
        self.assertIn("prepush_claim_check_final", hook[hook.rindex("if [ \"$fail\" = \"0\" ]", 0, clean):clean])
        demoted = hook.index("gate failures present but demoted by env override")
        self.assertIn("prepush_claim_check_final", hook[demoted:hook.index("exit 0", demoted)])

    def test_no_pr_passes(self):
        self.assertEqual(self.run_check([], pr=False).returncode, 0)


if __name__ == "__main__":
    unittest.main()
