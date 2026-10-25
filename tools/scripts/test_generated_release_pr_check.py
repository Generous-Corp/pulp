#!/usr/bin/env python3
"""Hostile fixtures for the release-please branch of the bump fast path.

Load-bearing behavior: generated_version_bump_check.py lets a release-please
release PR skip the native matrix only when:

* the PR is release-please's own (branch, title naming its surface, the
  `autorelease: pending` label, one or two commits, both SSH-signed with the
  pinned release-bot key and the bot as author and committer, the release
  commit's subject equal to the title, the refresh marker bound to the PR, a
  base on protected main);
* the title's version is canonical ASCII x.y.z, exactly one semantic-version
  step from the base, a major step only when a commit release-please counted
  since the surface's last release tag records a breaking change (a `!:`
  header or a `BREAKING CHANGE:` footer, never the editable PR body), and
  every version file reads as it afterwards;
* release-please's commit changes bytes only at the locations its
  protected-base config declares: each declared JSONPath (no duplicate keys,
  no reformatting, no escape changes) and the CMake marker line, by the old
  version as one whole token;
* an SDK PR's tree equals a rerun of the protected base's
  release_derived_refresh.py (a tampered or out-of-bounds refresh is refused,
  and so is a head still missing a refresh that has work to do);
* in a merge group, the tree is main plus exactly the PR's edits on paths main
  has not touched since the PR's base.

Every case outside that set exits 1, which keeps the full native path.
"""
from __future__ import annotations

import argparse
import contextlib
import io
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import fold_migration_notes  # noqa: E402
import generated_version_bump_check as gvb  # noqa: E402
import version_at_land  # noqa: E402

SDK_BRANCH = "release-please--branches--main"
PLUGIN_BRANCH = "release-please--branches--main--components--plugin"
REPO_NAME = "Generous-Corp/pulp"
PLUGIN_TITLE = "chore(main): release plugin 0.492.0"
SDK_TITLE = "chore(main): release 1.12.0"

VERSIONING = {
    "schema_version": 1,
    "surfaces": {
        "sdk": {
            "label": "sdk",
            "version_files": [{"path": "CMakeLists.txt", "kind": "cmake_project_version"}],
            "trigger_paths": ["core/**"],
            "release_please": {"config": "rp/sdk.config.json", "manifest": "rp/sdk.manifest.json"},
        },
        "plugin": {
            "label": "plugin",
            "version_files": [
                {"path": ".claude-plugin/plugin.json", "kind": "json_field", "field": "version"},
            ],
            "trigger_paths": [".claude-plugin/**"],
            "release_please": {"config": "rp/plugin.config.json",
                               "manifest": "rp/plugin.manifest.json"},
        },
    },
    "skills": {"skills_dir": ".agents/skills", "path_map": "map.json"},
}
PLUGIN_CONFIG = {"packages": {".": {"component": "plugin", "exclude-paths": ["docs"],
                                    "extra-files": [
    {"type": "json", "path": ".claude-plugin/plugin.json", "jsonpath": "$.version"},
    {"type": "json", "path": ".claude-plugin/plugin.json",
     "jsonpath": "$.entries[?(@.name=='pulp')].version"},
]}}}
SDK_CONFIG = {"packages": {".": {"extra-files": [{"type": "generic", "path": "CMakeLists.txt"}]}}}

PLUGIN_JSON = (
    '{\n  "name": "pulp",\n  "version": "0.491.8",\n  "min_cli_version": "0.491.8",\n'
    '  "description": "a — b",\n'
    '  "entries": [\n    {\n      "name": "pulp",\n      "version": "0.491.8"\n    }\n  ]\n}\n'
)
CMAKE = (
    "project(Pulp\n    VERSION 1.11.0 # x-release-please-version\n)\n"
    "FetchContent_Declare(dep GIT_TAG v1.11.0)\n"
)
NOTE = '---\nbreaking = false\nsummary = "Knob API"\n---\n\n## Migration steps\n- rename\n'


def bot_meta(*, signed: bool = True, committer: str = gvb.BOT_NAME) -> dict:
    person = {"name": gvb.BOT_NAME, "email": gvb.BOT_EMAIL}
    return {
        "author": {"login": gvb.BOT_LOGIN}, "committer": {"login": gvb.BOT_LOGIN},
        "commit": {
            "author": person, "committer": {"name": committer, "email": gvb.BOT_EMAIL},
            "verification": {"verified": signed, "reason": "valid" if signed else "unsigned",
                             "signature": "-----BEGIN SSH SIGNATURE-----\nx" if signed else ""},
        },
    }


def _keypair(directory: Path, name: str) -> tuple[Path, str]:
    key = directory / name
    subprocess.run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", str(key)],
                   check=True, capture_output=True)
    listed = subprocess.run(["ssh-keygen", "-l", "-f", f"{key}.pub"], check=True,
                            capture_output=True, text=True, encoding="utf-8").stdout
    return key, listed.split()[1]


class Fixture(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls._keys = tempfile.TemporaryDirectory()
        keys = Path(cls._keys.name)
        cls.bot_key, cls.bot_fingerprint = _keypair(keys, "bot")
        cls.other_key, _ = _keypair(keys, "other")

    @classmethod
    def tearDownClass(cls) -> None:
        cls._keys.cleanup()

    def setUp(self) -> None:
        self._regenerators = version_at_land._DERIVED_REGENERATORS
        self._tmp = tempfile.TemporaryDirectory()
        self.repo = Path(self._tmp.name)
        self.git("init", "-q", "-b", "main")
        self.git("config", "user.email", "t@example.com")
        self.git("config", "user.name", "t")
        self.git("config", "gpg.format", "ssh")
        self.sign_with(self.bot_key)
        self.write("tools/scripts/release_bot_signer.json",
                   json.dumps({"ssh_signing_key_fingerprint": self.bot_fingerprint}) + "\n")
        self.write("tools/scripts/versioning.json", json.dumps(VERSIONING, indent=2) + "\n")
        # The verifier binds the protected base's regenerator scripts; their
        # subjects are absent here, so only the migration-note fold runs.
        for script in ("pulp_tooling_disposition.py", "gpu_handoff_provenance.py"):
            self.write(f"tools/scripts/{script}", "# fixture\n")
        self.write("tools/scripts/release_derived_refresh.py",
                   (HERE / "release_derived_refresh.py").read_text(encoding="utf-8"))
        self.write("rp/plugin.config.json", json.dumps(PLUGIN_CONFIG, indent=2) + "\n")
        self.write("rp/sdk.config.json", json.dumps(SDK_CONFIG, indent=2) + "\n")
        self.write("rp/plugin.manifest.json", '{\n  ".": "0.491.8"\n}\n')
        self.write("rp/sdk.manifest.json", '{\n  ".": "1.11.0"\n}\n')
        self.write(".claude-plugin/plugin.json", PLUGIN_JSON)
        self.write("CMakeLists.txt", CMAKE)
        self.write("CHANGELOG.md", "# Changelog\n")
        self.write("core/a.cpp", "int a;\n")
        self.base = self.commit("base")
        self.meta: dict[str, dict] = {}

    def tearDown(self) -> None:
        version_at_land._DERIVED_REGENERATORS = self._regenerators
        self._tmp.cleanup()

    def git(self, *args: str) -> str:
        return subprocess.run(["git", "-C", str(self.repo), *args], check=True,
                              capture_output=True, text=True, encoding="utf-8").stdout.strip()

    def sign_with(self, key: Path) -> None:
        self.git("config", "user.signingkey", str(key))
        self.git("config", "commit.gpgsign", "true")

    def write(self, rel: str, text: str) -> None:
        path = self.repo / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def commit(self, message: str) -> str:
        self.git("add", "-A")
        self.git("commit", "-q", "--allow-empty", "-m", message)
        return self.git("rev-parse", "HEAD")

    def plugin_release(self, plugin_json: str | None = None, *, manifest: str = "0.492.0",
                       title: str = PLUGIN_TITLE) -> str:
        self.write(".claude-plugin/plugin.json", plugin_json if plugin_json is not None else
                   PLUGIN_JSON.replace('"version": "0.491.8"', '"version": "0.492.0"'))
        self.write("rp/plugin.manifest.json", f'{{\n  ".": "{manifest}"\n}}\n')
        return self.commit(title)

    def sdk_release(self, cmake: str | None = None, *, title: str = SDK_TITLE) -> str:
        self.write("CMakeLists.txt", cmake if cmake is not None else
                   CMAKE.replace("VERSION 1.11.0 #", "VERSION 1.12.0 #"))
        self.write("rp/sdk.manifest.json", '{\n  ".": "1.12.0"\n}\n')
        return self.commit(title)

    def refresh(self, release: str, number: int = 7) -> str:
        fold_migration_notes.fold(self.repo, "1.12.0")
        return self.commit("chore: refresh release-derived files\n\n"
                           f"Release-Derived-Refresh: #{number} {release}")

    def verify(self, candidate: str, *, branch: str = PLUGIN_BRANCH, title: str = PLUGIN_TITLE,
               commits: int = 1, compare: str = "ahead", event_name: str = "pull_request",
               head: str | None = None, base: str | None = None,
               labels: tuple[str, ...] = ("autorelease: pending",), body: str = "") -> int:
        base = base or self.base
        pull = {"number": 7, "state": "open", "title": title, "commits": commits, "body": body,
                "labels": [{"name": name} for name in labels],
                "head": {"ref": branch, "sha": candidate, "repo": {"full_name": REPO_NAME}},
                "base": {"ref": "main", "sha": base}}
        pull.update(getattr(self, "pull_overrides", {}) or {})
        if event_name == "pull_request":
            event = {"repository": {"full_name": REPO_NAME}, "pull_request": pull}
        else:
            event = {"repository": {"full_name": REPO_NAME},
                     "merge_group": {"base_sha": base, "head_sha": head}}
        event.update(getattr(self, "event_overrides", {}) or {})
        if getattr(self, "raw_event", None) is not None:
            event = self.raw_event

        def fake_gh(_repo, endpoint, *, paginate=False):
            if endpoint.startswith("compare/"):
                return {"status": compare}
            if endpoint.endswith("/pulls?per_page=100"):
                return getattr(self, "pulls_response", None) or [[{"number": 7}]]
            if endpoint == "pulls/7":
                detail = getattr(self, "detail_response", None)
                return detail if detail is not None else pull
            if endpoint.startswith("commits/"):
                return self.meta.get(endpoint.split("/", 1)[1], bot_meta())
            raise AssertionError(endpoint)

        event_path = self.repo.parent / f"{self.repo.name}-event.json"
        event_path.write_text(json.dumps(event), encoding="utf-8")
        original, gvb._gh_json = gvb._gh_json, fake_gh
        self.stderr = io.StringIO()
        try:
            with contextlib.redirect_stderr(self.stderr):
                return gvb.main(["--repo", str(self.repo), "--event-name", event_name,
                                 "--event-path", str(event_path), "--base", base,
                                 "--head", head or candidate])
        finally:
            gvb._gh_json = original
            version_at_land._DERIVED_REGENERATORS = self._regenerators
            event_path.unlink()

    def refused(self, candidate: str, why: str, **kwargs) -> None:
        self.assertEqual(self.verify(candidate, **kwargs), 1)
        self.assertIn(why, self.stderr.getvalue())


class PluginSurface(Fixture):
    def test_a_declared_version_only_release_takes_the_fast_path(self) -> None:
        self.assertEqual(self.verify(self.plugin_release()), 0, self.stderr.getvalue())

    def test_unicode_digits_are_not_a_version(self) -> None:
        candidate = self.plugin_release(title="chore(main): release plugin 0.492.٠")
        self.refused(candidate, "title is not release-please's",
                     title="chore(main): release plugin 0.492.٠")

    def test_a_duplicate_key_is_refused(self) -> None:
        dup = PLUGIN_JSON.replace('"version": "0.491.8",', '"version": "0.492.0",', 1)
        dup = dup.replace('  "name": "pulp",\n', '  "name": "pulp",\n  "name": "evil",\n', 1)
        self.refused(self.plugin_release(dup), "duplicate JSON key")

    def test_a_reformatted_file_is_refused(self) -> None:
        one_line = json.dumps(json.loads(PLUGIN_JSON.replace("0.491.8", "0.492.0")
                                         .replace('"min_cli_version": "0.492.0"',
                                                  '"min_cli_version": "0.491.8"')),
                              ensure_ascii=False) + "\n"
        self.refused(self.plugin_release(one_line), "line count")

    def test_an_escape_change_is_refused(self) -> None:
        escaped = PLUGIN_JSON.replace('"version": "0.491.8"', '"version": "0.492.0"') \
            .replace("—", "\\u2014")
        self.refused(self.plugin_release(escaped), "outside its version")

    def test_an_undeclared_field_holding_the_old_version_is_refused(self) -> None:
        moved = PLUGIN_JSON.replace("0.491.8", "0.492.0")
        self.refused(self.plugin_release(moved), "beyond its declared version fields")

    def major_release(self) -> tuple[str, str]:
        major = PLUGIN_JSON.replace('"version": "0.491.8"', '"version": "1.0.0"')
        title = "chore(main): release plugin 1.0.0"
        return self.plugin_release(major, manifest="1.0.0", title=title), title

    def landed(self, message: str, path: str = "core/a.cpp") -> None:
        self.write(path, message + "\n")
        self.base = self.commit(message)

    def test_a_major_step_needs_a_counted_breaking_commit(self) -> None:
        self.git("tag", "plugin-v0.491.8", self.base)
        self.landed("feat(x): add y")
        candidate, title = self.major_release()
        self.refused(candidate, "no breaking change recorded", title=title)

    def test_the_editable_pr_body_is_not_evidence(self) -> None:
        self.git("tag", "plugin-v0.491.8", self.base)
        candidate, title = self.major_release()
        self.refused(candidate, "no breaking change recorded", title=title,
                     body="### \u26a0 BREAKING CHANGES\n\n* drop x")

    def test_a_bang_header_or_footer_since_the_tag_allows_a_major_step(self) -> None:
        for message in ("feat(x)!: drop y", "fix: z\n\nBREAKING CHANGE: y is gone"):
            with self.subTest(message=message):
                self.setUp()
                self.git("tag", "plugin-v0.491.8", self.base)
                self.landed(message)
                candidate, title = self.major_release()
                self.assertEqual(self.verify(candidate, title=title), 0, self.stderr.getvalue())
                self.tearDown()

    def test_a_break_before_the_tag_or_only_in_excluded_paths_does_not_count(self) -> None:
        self.landed("feat(x)!: drop y")
        self.git("tag", "plugin-v0.491.8", self.base)
        self.landed("feat(docs)!: reword", path="docs/x.md")
        candidate, title = self.major_release()
        self.refused(candidate, "no breaking change recorded", title=title)

    def test_a_bang_title_on_a_merge_commit_is_not_read(self) -> None:
        self.git("tag", "plugin-v0.491.8", self.base)
        self.git("checkout", "-q", "-b", "topic")
        self.landed("work")
        self.git("checkout", "-q", "main")
        self.git("merge", "-q", "--no-ff", "-m", "Merge pull request #2 from topic\n\nfeat!: y",
                 "topic")
        self.base = self.git("rev-parse", "HEAD")
        candidate, title = self.major_release()
        self.refused(candidate, "no breaking change recorded", title=title)

    def test_a_missing_previous_tag_is_refused_for_a_major_step(self) -> None:
        candidate, title = self.major_release()
        self.refused(candidate, "previous release tag", title=title)

    def test_a_commit_signed_with_another_key_is_refused(self) -> None:
        self.sign_with(self.other_key)
        self.refused(self.plugin_release(), "pinned release-bot key")

    def test_an_unsigned_local_commit_is_refused(self) -> None:
        self.git("config", "commit.gpgsign", "false")
        self.refused(self.plugin_release(), "pinned release-bot key")

    def test_an_unbounded_jump_is_refused(self) -> None:
        jumped = PLUGIN_JSON.replace('"version": "0.491.8"', '"version": "99.0.0"')
        title = "chore(main): release plugin 99.0.0"
        self.refused(self.plugin_release(jumped, manifest="99.0.0", title=title),
                     "not one semantic-version step", title=title)

    def test_a_leading_zero_is_refused(self) -> None:
        padded = PLUGIN_JSON.replace('"version": "0.491.8"', '"version": "0.492.00"')
        title = "chore(main): release plugin 0.492.00"
        self.refused(self.plugin_release(padded, manifest="0.492.00", title=title),
                     "canonical", title=title)

    def test_a_version_other_than_the_titles_is_refused(self) -> None:
        other = PLUGIN_JSON.replace('"version": "0.491.8"', '"version": "0.493.0"')
        self.refused(self.plugin_release(other, manifest="0.493.0"),
                     "beyond its declared version fields")

    def test_a_path_outside_the_declared_files_is_refused(self) -> None:
        self.write("core/a.cpp", "int b;\n")
        self.refused(self.plugin_release(), "outside its declared files")

    def test_an_unsigned_or_foreign_release_commit_is_refused(self) -> None:
        candidate = self.plugin_release()
        self.meta[candidate] = bot_meta(signed=False)
        self.refused(candidate, "not SSH-signed")
        self.meta[candidate] = bot_meta(committer="Someone Else")
        self.refused(candidate, "committer is not the release bot")

    def test_the_pr_must_be_release_pleases_own(self) -> None:
        candidate = self.plugin_release()
        self.refused(candidate, "title is not release-please's", title="chore: something")
        self.refused(candidate, "names the other surface", title="chore(main): release 0.492.0")
        self.refused(candidate, "holds commits other than", commits=2)
        self.refused(candidate, "autorelease: pending", labels=("release: plugin",))
        self.refused(candidate, "not on protected main", compare="diverged")
        # Another branch is not routed to the release-PR recognizer at all;
        # the version-at-land recognizer refuses it.
        self.refused(candidate, "canonical bump subject", branch="release/other")

    def test_a_merge_group_on_a_moved_main_overlays_untouched_paths(self) -> None:
        candidate = self.plugin_release()
        self.git("checkout", "-q", "-b", "later", self.base)
        self.write("core/a.cpp", "int c;\n")
        moved = self.commit("feat: unrelated")
        self.git("merge", "-q", "--no-ff", "-m", "group", candidate)
        group = self.git("rev-parse", "HEAD")
        self.assertEqual(self.verify(candidate, event_name="merge_group", head=group,
                                     base=moved), 0, self.stderr.getvalue())

    def test_a_merge_group_where_main_touched_an_edited_file_is_refused(self) -> None:
        candidate = self.plugin_release()
        self.git("checkout", "-q", "-b", "later", self.base)
        self.write(".claude-plugin/plugin.json", PLUGIN_JSON.replace("a — b", "c"))
        moved = self.commit("fix: plugin description")
        subprocess.run(["git", "-C", str(self.repo), "merge", "-q", "--no-ff", "-m", "group",
                        "-X", "theirs", candidate], check=True, capture_output=True)
        group = self.git("rev-parse", "HEAD")
        self.refused(candidate, "main changed .claude-plugin/plugin.json",
                     event_name="merge_group", head=group, base=moved)


class SdkSurface(Fixture):
    def test_the_cmake_marker_line_release_takes_the_fast_path(self) -> None:
        candidate = self.sdk_release()
        self.assertEqual(self.verify(candidate, branch=SDK_BRANCH, title=SDK_TITLE), 0,
                         self.stderr.getvalue())

    def test_another_occurrence_of_the_old_version_is_refused(self) -> None:
        both = CMAKE.replace("1.11.0", "1.12.0")
        self.refused(self.sdk_release(both), "beyond its marked version token",
                     branch=SDK_BRANCH, title=SDK_TITLE)

    def test_a_second_marker_line_cannot_move_alone(self) -> None:
        self.write("CMakeLists.txt", CMAKE + "set(X 1.11.0) # x-release-please-version\n")
        self.base = self.commit("base with a second marker line")
        text = (self.repo / "CMakeLists.txt").read_text(encoding="utf-8")
        moved = text.replace("set(X 1.11.0)", "set(X 1.12.0)")
        self.refused(self.sdk_release(moved), "does not read as 1.12.0",
                     branch=SDK_BRANCH, title=SDK_TITLE)

    def test_a_partial_token_match_is_refused(self) -> None:
        self.write("CMakeLists.txt", CMAKE.replace(
            "VERSION 1.11.0 # x-release-please-version",
            "VERSION 1.11.0 # x-release-please-version v11.11.0"))
        self.base = self.commit("base with a lookalike token")
        lookalike = (self.repo / "CMakeLists.txt").read_text(encoding="utf-8").replace(
            "v11.11.0", "v11.12.0")
        self.refused(self.sdk_release(lookalike), "beyond its marked version token",
                     branch=SDK_BRANCH, title=SDK_TITLE)

    def test_unicode_digits_in_cmake_are_refused(self) -> None:
        arabic = CMAKE.replace("VERSION 1.11.0 #", "VERSION 1.12.٠ #")
        self.refused(self.sdk_release(arabic), "beyond its marked version token",
                     branch=SDK_BRANCH, title=SDK_TITLE)

    def test_a_reproduced_refresh_takes_the_fast_path(self) -> None:
        self.write("docs/migrations/unreleased/knob.md", NOTE)
        self.base = self.commit("feat: knob, with an unreleased note")
        release = self.sdk_release()
        refresh = self.refresh(release)
        self.assertTrue((self.repo / "docs/migrations/v1.12.0.md").exists())
        self.assertEqual(self.verify(refresh, branch=SDK_BRANCH, title=SDK_TITLE, commits=2),
                         0, self.stderr.getvalue())

    def test_a_head_missing_its_refresh_is_refused(self) -> None:
        self.write("docs/migrations/unreleased/knob.md", NOTE)
        self.base = self.commit("feat: knob, with an unreleased note")
        self.refused(self.sdk_release(), "tree differs", branch=SDK_BRANCH, title=SDK_TITLE)

    def test_a_tampered_refresh_is_refused(self) -> None:
        self.write("docs/migrations/unreleased/knob.md", NOTE)
        self.base = self.commit("feat: knob, with an unreleased note")
        release = self.sdk_release()
        fold_migration_notes.fold(self.repo, "1.12.0")
        note = self.repo / "docs/migrations/v1.12.0.md"
        note.write_text(note.read_text(encoding="utf-8") + "tampered\n", encoding="utf-8")
        refresh = self.commit("chore: refresh release-derived files\n\n"
                              f"Release-Derived-Refresh: #7 {release}")
        self.refused(refresh, "tree differs", branch=SDK_BRANCH, title=SDK_TITLE, commits=2)

    def test_a_refresh_outside_the_derived_paths_is_refused(self) -> None:
        release = self.sdk_release()
        self.write("core/a.cpp", "int evil;\n")
        refresh = self.commit("chore: refresh release-derived files\n\n"
                              f"Release-Derived-Refresh: #7 {release}")
        self.refused(refresh, "non-derived path", branch=SDK_BRANCH, title=SDK_TITLE, commits=2)

    def test_a_refresh_bound_to_another_release_or_unsigned_is_refused(self) -> None:
        release = self.sdk_release()
        refresh = self.refresh(release, number=8)
        self.refused(refresh, "marker does not bind", branch=SDK_BRANCH, title=SDK_TITLE,
                     commits=2)
        self.git("reset", "-q", "--hard", release)
        refresh = self.refresh(release)
        self.meta[refresh] = bot_meta(signed=False)
        self.refused(refresh, "derived-file refresh is not SSH-signed", branch=SDK_BRANCH,
                     title=SDK_TITLE, commits=2)

    def test_a_refresh_on_the_plugin_pr_is_refused(self) -> None:
        release = self.plugin_release()
        refresh = self.refresh(release)
        self.refused(refresh, "only the SDK release PR", commits=2)


class Guards(Fixture):
    """Each refusal named by its own reason, so no guard hides behind a later one."""

    def rebase_config(self, surface: str, config: dict) -> None:
        self.write(f"rp/{surface}.config.json", json.dumps(config, indent=2) + "\n")
        self.base = self.commit(f"config for {surface}")

    def test_a_merge_commit_as_the_release_commit_is_refused(self) -> None:
        self.git("checkout", "-q", "-b", "topic")
        self.write("core/a.cpp", "int z;\n")
        self.commit("topic work")
        self.git("checkout", "-q", "main")
        self.git("merge", "-q", "--no-ff", "-m", PLUGIN_TITLE, "topic")
        self.refused(self.git("rev-parse", "HEAD"), "is not single-parent")

    def test_a_base_without_the_surface_or_its_config_is_refused(self) -> None:
        versioning = json.loads(json.dumps(VERSIONING))
        del versioning["surfaces"]["plugin"]
        self.write("tools/scripts/versioning.json", json.dumps(versioning))
        self.base = self.commit("no plugin surface")
        self.refused(self.plugin_release(), "has no plugin surface")

    def test_a_surface_without_a_release_please_mapping_is_refused(self) -> None:
        for mapping in ({"config": "rp/plugin.config.json"},
                        {"manifest": "rp/plugin.manifest.json"}):
            with self.subTest(mapping=mapping):
                self.setUp()
                versioning = json.loads(json.dumps(VERSIONING))
                versioning["surfaces"]["plugin"]["release_please"] = mapping
                self.write("tools/scripts/versioning.json", json.dumps(versioning))
                self.base = self.commit("partial mapping")
                self.refused(self.plugin_release(), "names no release-please config")
                self.tearDown()

    def test_a_config_that_is_not_one_root_package_is_refused(self) -> None:
        for config in ({"packages": {".": PLUGIN_CONFIG["packages"]["."], "x": {}}},
                       {"packages": {".": []}}):
            with self.subTest(config=config):
                self.setUp()
                self.rebase_config("plugin", config)
                self.refused(self.plugin_release(), "not one root package")
                self.tearDown()

    def test_an_unknown_extra_file_type_is_refused(self) -> None:
        config = json.loads(json.dumps(PLUGIN_CONFIG))
        config["packages"]["."]["extra-files"].append({"type": "toml", "path": "x.toml"})
        self.rebase_config("plugin", config)
        self.refused(self.plugin_release(), "unsupported extra-files type")

    def test_version_files_that_disagree_on_the_base_are_refused(self) -> None:
        versioning = json.loads(json.dumps(VERSIONING))
        versioning["surfaces"]["plugin"]["version_files"].append(
            {"path": "rp/plugin.manifest.json", "kind": "json_field", "field": "x"})
        self.write("tools/scripts/versioning.json", json.dumps(versioning))
        self.base = self.commit("second version file")
        self.refused(self.plugin_release(), "version files drift")

    def test_an_added_path_is_refused(self) -> None:
        self.write(".claude-plugin/new.json", "{}\n")
        self.refused(self.plugin_release(), "adds, deletes or renames")

    def test_the_pinned_signer_config_must_exist_and_pin_a_sha256(self) -> None:
        for text, why in ((None, "has no tools/scripts/release_bot_signer.json"),
                          ('{"ssh_signing_key_fingerprint": "MD5:x"}\n', "pins no SHA256"),
                          ("{}\n", "pins no SHA256")):
            with self.subTest(text=text):
                self.setUp()
                path = self.repo / "tools/scripts/release_bot_signer.json"
                if text is None:
                    path.unlink()
                else:
                    path.write_text(text, encoding="utf-8")
                self.base = self.commit("signer config")
                self.refused(self.plugin_release(), why)
                self.tearDown()

    def test_metadata_must_be_the_bots_on_every_field(self) -> None:
        candidate = self.plugin_release()
        cases = [
            ("not-a-dict", "metadata is not an object"),
            ({**bot_meta(), "commit": {**bot_meta()["commit"], "verification": {
                "verified": True, "reason": "unknown_key",
                "signature": "-----BEGIN SSH SIGNATURE-----\nx"}}}, "not SSH-signed"),
            ({**bot_meta(), "commit": {**bot_meta()["commit"], "verification": {
                "verified": True, "reason": "valid", "signature": "-----BEGIN PGP"}}},
             "not SSH-signed"),
            ({**bot_meta(), "author": {"login": "someone"}}, "author is not the release bot"),
            ({**bot_meta(), "commit": {**bot_meta()["commit"],
                                      "author": {"name": gvb.BOT_NAME, "email": "x@y"}}},
             "author is not the release bot"),
        ]
        for meta, why in cases:
            with self.subTest(why=why, meta=str(meta)[:40]):
                self.meta[candidate] = meta
                self.refused(candidate, why)

    def test_the_release_commit_subject_must_be_the_title(self) -> None:
        candidate = self.plugin_release(title="chore(main): release plugin 0.492.0 again")
        self.refused(candidate, "subject is not the release PR title")

    def test_the_pull_must_name_this_head_repo_and_main(self) -> None:
        candidate = self.plugin_release()
        for field, value, why in (
            ("head", {"ref": PLUGIN_BRANCH, "sha": candidate, "repo": {"full_name": "fork/pulp"}},
             "head is not this candidate"),
            ("base", {"ref": "develop", "sha": self.base}, "does not target main"),
            ("state", "closed", "not open"),
        ):
            with self.subTest(field=field, why=why):
                self.pull_overrides = {field: value}
                self.refused(candidate, why)
        self.pull_overrides = {}
        self.git("checkout", "-q", "-b", "later", self.base)
        self.git("merge", "-q", "--no-ff", "-m", "group", candidate)
        group = self.git("rev-parse", "HEAD")
        self.pull_overrides = {"head": {"ref": PLUGIN_BRANCH, "sha": "f" * 40,
                                        "repo": {"full_name": REPO_NAME}}}
        self.refused(candidate, "head is not this candidate", event_name="merge_group",
                     head=group)
        self.pull_overrides = {}

    def test_a_pull_request_event_behind_main_is_refused(self) -> None:
        candidate = self.plugin_release()
        self.git("checkout", "-q", "-b", "later", self.base)
        self.write("core/a.cpp", "int q;\n")
        moved = self.commit("feat: later")
        self.refused(candidate, "behind main", base=moved)

    def test_event_shape_guards(self) -> None:
        candidate = self.plugin_release()
        for repository in ({}, {"full_name": ""}):
            with self.subTest(repository=repository):
                self.event_overrides = {"repository": repository}
                self.refused(candidate, "omitted repository.full_name")
        self.event_overrides = {}
        self.refused(candidate, "unrelated to the pull request event", head="e" * 40)
        self.pull_overrides = {"base": {"ref": "main", "sha": "e" * 40}}
        self.refused(candidate, "do not match immutable event commits")
        self.pull_overrides = {"base": {"ref": "main", "sha": "x" * 40}}
        self.refused(candidate, "do not match immutable event commits", base="x" * 40)
        self.pull_overrides = {}

    def test_merge_group_shape_guards(self) -> None:
        candidate = self.plugin_release()
        self.git("checkout", "-q", "-b", "later", self.base)
        self.git("merge", "-q", "--no-ff", "-m", "group", candidate)
        group = self.git("rev-parse", "HEAD")
        self.event_overrides = {"merge_group": {"base_sha": self.base, "head_sha": "d" * 40}}
        self.refused(candidate, "does not match the merge-group event",
                     event_name="merge_group", head=group)
        self.event_overrides = {}
        self.refused(candidate, "unknown topology", event_name="merge_group", head=group,
                     base=candidate)
        self.refused(candidate, "unknown topology", event_name="merge_group", head=candidate,
                     base=self.base)

    def test_a_merge_group_must_be_exactly_main_plus_the_edits(self) -> None:
        candidate = self.plugin_release()
        self.git("checkout", "-q", "-b", "later", self.base)
        self.git("merge", "-q", "--no-ff", "--no-commit", candidate)
        self.write("core/a.cpp", "int smuggled;\n")
        self.git("add", "-A")
        self.git("commit", "-q", "-m", "group")
        group = self.git("rev-parse", "HEAD")
        self.refused(candidate, "not main plus the release PR's edits", event_name="merge_group",
                     head=group)

    def test_a_merge_group_associated_with_another_branch_or_many_prs_is_refused(self) -> None:
        candidate = self.plugin_release()
        self.git("checkout", "-q", "-b", "later", self.base)
        self.git("merge", "-q", "--no-ff", "-m", "group", candidate)
        group = self.git("rev-parse", "HEAD")
        self.refused(candidate, "not a release-please release PR", event_name="merge_group",
                     head=group, branch="feature/x")
        self.pulls_response = [[{"number": 7}, {"number": 8}]]
        self.refused(candidate, "exactly one pull request", event_name="merge_group", head=group)
        self.pulls_response = [[]]
        self.refused(candidate, "exactly one pull request", event_name="merge_group", head=group)
        self.pulls_response = None
        self.detail_response = ["not", "a", "dict"]
        self.refused(candidate, "detailed pull response", event_name="merge_group", head=group)
        self.detail_response = None

    def test_a_merge_group_of_an_sdk_refresh_is_routed_and_proven(self) -> None:
        self.write("docs/migrations/unreleased/knob.md", NOTE)
        self.base = self.commit("feat: knob, with an unreleased note")
        release = self.sdk_release()
        refresh = self.refresh(release)
        self.git("checkout", "-q", "-b", "later", self.base)
        self.git("merge", "-q", "--no-ff", "-m", "group", refresh)
        group = self.git("rev-parse", "HEAD")
        self.assertEqual(self.verify(refresh, branch=SDK_BRANCH, title=SDK_TITLE, commits=2,
                                     event_name="merge_group", head=group), 0,
                         self.stderr.getvalue())

    def test_a_base_without_the_refresh_script_is_refused_for_the_sdk(self) -> None:
        (self.repo / "tools/scripts/release_derived_refresh.py").unlink()
        self.base = self.commit("no refresh script")
        self.refused(self.sdk_release(), "no release_derived_refresh.py", branch=SDK_BRANCH,
                     title=SDK_TITLE)

    def test_a_non_object_event_is_refused(self) -> None:
        self.raw_event = ["not", "an", "object"]
        self.refused(self.plugin_release(), "event payload is not an object")
        self.raw_event = None

    def test_an_unsupported_event_is_refused_directly(self) -> None:
        args = argparse.Namespace(event_name="push", base="a" * 40, head="a" * 40)
        with self.assertRaisesRegex(gvb.NotGeneratedBump, "unsupported event"):
            gvb._verify_release_pr(args, self.repo, {"repository": {"full_name": REPO_NAME}})

    def test_a_previous_tag_that_is_not_an_ancestor_is_refused(self) -> None:
        self.git("checkout", "-q", "-b", "side", self.base)
        side = self.commit("side")
        self.git("tag", "plugin-v0.491.8", side)
        self.git("checkout", "-q", "main")
        major = PLUGIN_JSON.replace('"version": "0.491.8"', '"version": "1.0.0"')
        title = "chore(main): release plugin 1.0.0"
        candidate = self.plugin_release(major, manifest="1.0.0", title=title)
        self.refused(candidate, "not an ancestor", title=title)

    def test_a_previous_tag_only_on_origin_is_fetched(self) -> None:
        holder = tempfile.TemporaryDirectory()
        self.addCleanup(holder.cleanup)
        origin = Path(holder.name) / "origin.git"
        subprocess.run(["git", "init", "-q", "--bare", str(origin)], check=True)
        self.git("remote", "add", "origin", str(origin))
        self.write("core/a.cpp", "int dropped;\n")
        self.landed_breaking = self.commit("feat(x)!: drop y")
        self.git("tag", "plugin-v0.491.8", self.base)
        self.git("push", "-q", "origin", "refs/tags/plugin-v0.491.8")
        self.git("tag", "-d", "plugin-v0.491.8")
        self.base = self.landed_breaking
        major = PLUGIN_JSON.replace('"version": "0.491.8"', '"version": "1.0.0"')
        title = "chore(main): release plugin 1.0.0"
        candidate = self.plugin_release(major, manifest="1.0.0", title=title)
        self.assertEqual(self.verify(candidate, title=title), 0, self.stderr.getvalue())


class Units(unittest.TestCase):
    def test_tag_names_follow_the_component_options(self) -> None:
        self.assertEqual(gvb._tag_for({"include-component-in-tag": True,
                                       "packages": {".": {}}}, "1.0.0"), "v1.0.0")
        self.assertEqual(gvb._tag_for({"include-component-in-tag": False,
                                       "packages": {".": {"component": "p"}}}, "1.0.0"), "v1.0.0")
        self.assertEqual(gvb._tag_for({"packages": {".": {"component": "p"}}}, "1.0.0"),
                         "p-v1.0.0")
        self.assertEqual(gvb._tag_for({"packages": {".": {"component": "p",
                                                          "include-v-in-tag": False,
                                                          "tag-separator": "/"}}}, "1.0.0"),
                         "p/1.0.0")

    def test_jsonpath_refuses_what_it_does_not_understand(self) -> None:
        doc = {"a": {"name": "x"}, "items": [{"name": "x"}, {"other": 1}]}
        for path in ("a.b", "$.a[?(@.name=='x')]", "$.items.name"):
            with self.subTest(path=path), self.assertRaises(gvb.NotGeneratedBump):
                gvb._resolve_jsonpath(doc, path)

    def test_json_proof_guards(self) -> None:
        before = '{\n  "v": "1.0.0",\n  "w": "x"\n}\n'
        with self.assertRaisesRegex(gvb.NotGeneratedBump, "does not hold"):
            gvb._prove_json_file(before.replace('"1.0.0"', '"0.9.0"'),
                                 before.replace('"1.0.0"', '"1.1.0"'), "f", ["$.v"], "1.0.0",
                                 "1.1.0")
        with self.assertRaisesRegex(gvb.NotGeneratedBump, "outside its version fields"):
            gvb._prove_json_file(before, before.replace('"1.0.0"', '"1.1.0"')
                                 .replace('"w": "x"', '"w":  "x"'), "f", ["$.v"], "1.0.0", "1.1.0")
        with self.assertRaisesRegex(gvb.NotGeneratedBump, "outside its version token"):
            gvb._prove_json_file(before, before.replace('"v": "1.0.0"', '"v":"1.1.0"'), "f",
                                 ["$.v"], "1.0.0", "1.1.0")

    def test_generic_proof_guards(self) -> None:
        text = "VERSION 1.0.0 # x-release-please-version\nset(Y 1.0.0)\n"
        with self.assertRaisesRegex(gvb.NotGeneratedBump, "without moving a version"):
            gvb._prove_generic_file(text, text, "f", "1.0.0", "1.1.0")
        with self.assertRaisesRegex(gvb.NotGeneratedBump, "marked version token"):
            gvb._prove_generic_file(text, text.replace("set(Y 1.0.0)", "set(Y 1.1.0)"), "f",
                                    "1.0.0", "1.1.0")

    def test_signature_parsing_refuses_malformed_armor(self) -> None:
        import base64 as b64
        def blob(magic=b"SSHSIG", key=b"k" * 32, claimed=None):
            length = len(key) if claimed is None else claimed
            return magic + b"\x00\x00\x00\x01" + length.to_bytes(4, "big") + key
        def commit_text(armor_lines):
            return "tree x\nauthor a\ngpgsig " + "\n ".join(armor_lines) + "\n\nmsg\n"
        good = b64.b64encode(blob()).decode()
        cases = {
            "no gpgsig": "tree x\n\nmsg\n",
            "no begin": commit_text(["-----BEGIN PGP SIGNATURE-----", good,
                                     "-----END SSH SIGNATURE-----"]),
            "no end": commit_text(["-----BEGIN SSH SIGNATURE-----", good, "-----END PGP-----"]),
            "bad base64": commit_text(["-----BEGIN SSH SIGNATURE-----", "!!!",
                                       "-----END SSH SIGNATURE-----"]),
            "bad magic": commit_text(["-----BEGIN SSH SIGNATURE-----",
                                      b64.b64encode(blob(magic=b"NOTSIG")).decode(),
                                      "-----END SSH SIGNATURE-----"]),
            "short": commit_text(["-----BEGIN SSH SIGNATURE-----",
                                  b64.b64encode(b"SSHSIG\x00").decode(),
                                  "-----END SSH SIGNATURE-----"]),
            "truncated key": commit_text(["-----BEGIN SSH SIGNATURE-----",
                                          b64.b64encode(blob(claimed=64)).decode(),
                                          "-----END SSH SIGNATURE-----"]),
            "empty key": commit_text(["-----BEGIN SSH SIGNATURE-----",
                                      b64.b64encode(blob(key=b"")).decode(),
                                      "-----END SSH SIGNATURE-----"]),
        }
        original = gvb._git
        try:
            for name, text in cases.items():
                with self.subTest(name=name):
                    gvb._git = lambda *_a, _t=text, **_k: subprocess.CompletedProcess([], 0, _t, "")
                    self.assertIsNone(gvb._signing_key_fingerprint(Path("."), "c"))
            gvb._git = lambda *_a, **_k: subprocess.CompletedProcess(
                [], 0, commit_text(["-----BEGIN SSH SIGNATURE-----", good,
                                    "-----END SSH SIGNATURE-----"]), "")
            self.assertTrue(gvb._signing_key_fingerprint(Path("."), "c").startswith("SHA256:"))
        finally:
            gvb._git = original


class Pieces(unittest.TestCase):
    def test_one_semver_step(self) -> None:
        for new in ("0.491.9", "0.492.0", "1.0.0"):
            gvb._one_step("0.491.8", new, breaking=True)
        for new in ("0.491.9", "0.492.0"):
            gvb._one_step("0.491.8", new, breaking=False)
        with self.assertRaises(gvb.NotGeneratedBump):
            gvb._one_step("0.491.8", "1.0.0", breaking=False)
        for new in ("0.491.10", "0.492.1", "1.0.1", "0.491.8", "0.490.0", "2.0.0"):
            with self.subTest(new=new), self.assertRaises(gvb.NotGeneratedBump):
                gvb._one_step("0.491.8", new, breaking=True)

    def test_jsonpath_subset(self) -> None:
        doc = {".": "1", "a": [{"name": "x", "v": "1"}, {"name": "y", "v": "1"}]}
        self.assertEqual(gvb._resolve_jsonpath(doc, '$["."]'), [(".",)])
        self.assertEqual(gvb._resolve_jsonpath(doc, "$.a[?(@.name=='y')].v"), [("a", 1, "v")])
        self.assertEqual(gvb._resolve_jsonpath(doc, "$.a[0].v"), [("a", 0, "v")])
        for path in ("$..v", "$.a[*].v", "$.missing"):
            with self.subTest(path=path), self.assertRaises(gvb.NotGeneratedBump):
                gvb._resolve_jsonpath(doc, path)


if __name__ == "__main__":
    unittest.main()
