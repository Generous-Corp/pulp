#!/usr/bin/env python3
"""Unit and real-Git negative controls for vellum_freeze_check.py."""

from __future__ import annotations

import base64
import datetime as dt
import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


MODULE_PATH = pathlib.Path(__file__).with_name("vellum_freeze_check.py")
SPEC = importlib.util.spec_from_file_location("vellum_freeze_check", MODULE_PATH)
assert SPEC and SPEC.loader
freeze = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = freeze
SPEC.loader.exec_module(freeze)


def mapping(state: str = "active", slice_state: str = "framework-authoritative-transferred"):
    event_id = "20260722-authority-activation"
    accepted_at = "2026-07-22T20:00:00Z"
    authority_commit = "b" * 40
    counterpart = "provenance/authority/records/native-design-kernel-v1.json"
    return {
        "schema_version": 2,
        "framework_repository": "Generous-Corp/vellum",
        "freeze_owner": "@danielraffel",
        "activation": {
            "state": state,
            "pulp_extraction_base": "a" * 40,
            "vellum_authority_commit": authority_commit if state == "active" else None,
            "authority_record_path": counterpart if state == "active" else None,
            "initial_transition_event": event_id if state == "active" else None,
            "accepted_by": "@danielraffel" if state == "active" else None,
            "accepted_at": accepted_at if state == "active" else None,
        },
        "slices": [
            {
                "id": "design-ir",
                "state": slice_state,
                "paths": ["core/view/src/design_ir_json.cpp"],
                "authority": {
                    "event_id": event_id,
                    "vellum_commit": authority_commit,
                    "counterpart": counterpart,
                    "accepted_by": "@danielraffel",
                    "accepted_at": accepted_at,
                }
                if slice_state == "framework-authoritative-transferred"
                else None,
            }
        ],
    }


def change_event(event_id: str = "20260722-design-fix"):
    return {
        "schema_version": 1,
        "event_id": event_id,
        "kind": "change",
        "created_at": "2026-07-22T20:00:00Z",
        "slices": ["design-ir"],
        "rationale": "Pulp-specific adapter behavior only.",
        "tests": ["design-contract"],
        "disposition": "pulp-only",
    }


def authority_event():
    return {
        "schema_version": 1,
        "event_id": "20260722-authority-activation",
        "kind": "authority-transition",
        "created_at": "2026-07-22T20:00:00Z",
        "slices": ["design-ir"],
        "rationale": "Activate the independently validated Vellum authority.",
        "tests": ["Vellum freeze", "Vellum trusted freeze"],
        "transition": "activate",
        "vellum_authority_commit": "b" * 40,
        "approved_by": "@danielraffel",
        "counterpart": "provenance/authority/records/native-design-kernel-v1.json",
    }


def vellum_trust_responses(
    *,
    commit: str = "b" * 40,
    tag_object_type: str = "tag",
    signature_verified: bool = True,
    immutable_release: bool = True,
    check_app_id: int | None = None,
):
    tag_name = "authority/native-design-kernel-v1"
    tag_object_sha = "c" * 40
    app_id = freeze.EXPECTED_VELLUM_READER_APP_ID
    check_id = (
        freeze.EXPECTED_CHECK_APP_IDS["provenance-verify"]
        if check_app_id is None
        else check_app_id
    )
    checks = [
        {
            "name": name,
            "head_sha": commit,
            "conclusion": "success",
            "app": {
                "id": check_id
                if name == "provenance-verify"
                else expected_app_id
            },
        }
        for name, expected_app_id in freeze.EXPECTED_CHECK_APP_IDS.items()
    ]
    return {
        "https://api.github.com/app": {"id": app_id},
        "https://api.github.com/installation": {"app_id": app_id},
        "https://api.github.com/repos/Generous-Corp/vellum": {
            "id": freeze.EXPECTED_FRAMEWORK_REPOSITORY_ID,
            "full_name": "Generous-Corp/vellum",
            "private": True,
            "archived": False,
        },
        "https://api.github.com/installation/repositories?per_page=100": {
            "total_count": 1,
            "repositories": [{"id": freeze.EXPECTED_FRAMEWORK_REPOSITORY_ID}],
        },
        (
            "https://api.github.com/repos/Generous-Corp/vellum/git/ref/"
            f"tags/{tag_name}"
        ): {"object": {"type": tag_object_type, "sha": tag_object_sha}},
        (
            "https://api.github.com/repos/Generous-Corp/vellum/git/tags/"
            f"{tag_object_sha}"
        ): {
            "object": {"type": "commit", "sha": commit},
            "verification": {
                "verified": signature_verified,
                "reason": "valid" if signature_verified else "unsigned",
            },
        },
        (
            "https://api.github.com/repos/Generous-Corp/vellum/releases/tags/"
            "authority%2Fnative-design-kernel-v1"
        ): {
            "tag_name": tag_name,
            "draft": False,
            "immutable": immutable_release,
            "published_at": "2026-07-23T22:00:00Z",
        },
        (
            "https://api.github.com/repos/Generous-Corp/vellum/commits/"
            f"{commit}/check-runs?per_page=100"
        ): {"check_runs": checks},
    }


class FreezeUnitTests(unittest.TestCase):
    def test_private_api_failure_names_endpoint_without_query_or_token(self):
        error = freeze.urllib.error.HTTPError(
            "https://api.github.com/repos/Generous-Corp/vellum/commits/deadbeef",
            404,
            "Not Found",
            {},
            None,
        )
        with mock.patch.object(
            freeze.urllib.request, "urlopen", side_effect=error
        ), self.assertRaisesRegex(
            freeze.FreezeError,
            (
                r"at /repos/Generous-Corp/vellum/commits/deadbeef: "
                r"HTTP Error 404"
            ),
        ) as raised:
            freeze._github_json(
                (
                    "https://api.github.com/repos/Generous-Corp/vellum/"
                    "commits/deadbeef?secret=do-not-log"
                ),
                "secret-token",
            )
        message = str(raised.exception)
        self.assertNotIn("do-not-log", message)
        self.assertNotIn("secret-token", message)

    def test_prepared_map_transfers_nothing(self):
        value = mapping("prepared", "pulp-authoritative-untransferred")
        freeze.validate_map(value)
        self.assertEqual(
            freeze.affected_slices([value], ["core/view/src/design_ir_json.cpp"]), {}
        )

    def test_prepared_schema_v1_can_migrate_without_transferring_authority(self):
        old = mapping("prepared", "pulp-authoritative-untransferred")
        old["schema_version"] = 1
        old["activation"].pop("authority_record_path")
        old["activation"].pop("initial_transition_event")
        for item in old["slices"]:
            item.pop("authority")
        new = mapping("prepared", "pulp-authoritative-untransferred")
        self.assertEqual(freeze.validate_map_transition(old, new), set())

        old["activation"]["state"] = "active"
        with self.assertRaisesRegex(freeze.FreezeError, "only the prepared"):
            freeze.validate_map_transition(old, new)

    def test_active_map_matches_globs_and_exact_paths(self):
        value = mapping()
        freeze.validate_map(value)
        self.assertEqual(
            freeze.affected_slices(
                [value],
                [
                    "README.md",
                    "core/view/src/design_ir_json.cpp",
                ],
            ),
            {
                "design-ir": ["core/view/src/design_ir_json.cpp"]
            },
        )

    def test_active_authority_cannot_be_rewritten(self):
        base = mapping()
        head = json.loads(json.dumps(base))
        head["activation"]["vellum_authority_commit"] = "c" * 40
        with self.assertRaisesRegex(freeze.FreezeError, "immutable"):
            freeze.validate_map_transition(base, head)

    def test_transferred_slice_cannot_be_removed(self):
        base = mapping()
        head = json.loads(json.dumps(base))
        head["slices"] = []
        with self.assertRaisesRegex(freeze.FreezeError, "cannot change"):
            freeze.validate_map_transition(base, head)

    def test_activation_reports_exact_new_slice(self):
        base = mapping("prepared", "pulp-authoritative-untransferred")
        head = mapping()
        self.assertEqual(
            freeze.validate_map_transition(base, head), {"design-ir"}
        )

    def test_schema_v2_rejects_later_slice_transfer(self):
        base = mapping()
        base["slices"].append(
            {
                "id": "render",
                "state": "pulp-authoritative-untransferred",
                "paths": ["core/render/src/render.cpp"],
                "authority": None,
            }
        )
        head = json.loads(json.dumps(base))
        head["slices"][1]["state"] = "framework-authoritative-transferred"
        head["slices"][1]["authority"] = dict(head["slices"][0]["authority"])
        with self.assertRaisesRegex(freeze.FreezeError, "one-shot coherent transfer"):
            freeze.validate_map_transition(base, head)

    def test_authority_metadata_is_derived_from_the_transition_event(self):
        head = mapping()
        event = authority_event()
        freeze._validate_event_coverage(
            [(".github/vellum-change-events/20260722-authority-activation.json", event)],
            {},
            {"design-ir"},
            "b" * 40,
            "activate",
            head,
        )
        for field, replacement in (
            ("initial_transition_event", "20260722-wrong-event"),
            ("authority_record_path", "provenance/authority/records/wrong.json"),
            ("accepted_by", "@wrong"),
            ("accepted_at", "2026-07-22T20:00:01Z"),
        ):
            broken = json.loads(json.dumps(head))
            broken["activation"][field] = replacement
            with self.assertRaisesRegex(freeze.FreezeError, "does not derive"):
                freeze._validate_event_coverage(
                    [("event.json", event)],
                    {},
                    {"design-ir"},
                    "b" * 40,
                    "activate",
                    broken,
                )

    def test_prepared_and_nontransferred_slices_cannot_claim_authority(self):
        prepared = mapping("prepared", "pulp-authoritative-untransferred")
        prepared["activation"]["initial_transition_event"] = (
            "20260722-authority-activation"
        )
        with self.assertRaisesRegex(freeze.FreezeError, "must remain null"):
            freeze.validate_map(prepared)

        prepared = mapping("prepared", "pulp-authoritative-untransferred")
        prepared["slices"][0]["authority"] = mapping()["slices"][0]["authority"]
        with self.assertRaisesRegex(freeze.FreezeError, "cannot carry"):
            freeze.validate_map(prepared)

    def test_old_counterpart_namespace_is_rejected(self):
        event = authority_event()
        event["counterpart"] = "provenance/activation/pending.json"
        with self.assertRaisesRegex(freeze.FreezeError, "authority record path"):
            freeze.validate_event(
                event,
                ".github/vellum-change-events/20260722-authority-activation.json",
            )

    def test_emergency_is_short_lived_and_issue_backed(self):
        event = change_event("20260722-emergency")
        event.update(
            {
                "disposition": "emergency-exception",
                "owner": "@owner",
                "expiry": "2026-08-20",
                "follow_up": "https://github.com/Generous-Corp/vellum/issues/1",
            }
        )
        with self.assertRaisesRegex(freeze.FreezeError, "within 14 days"):
            freeze.validate_event(
                event, ".github/vellum-change-events/20260722-emergency.json"
            )

    def test_unknown_event_fields_fail_closed(self):
        event = change_event()
        event["surprise"] = True
        with self.assertRaisesRegex(freeze.FreezeError, "unknown fields"):
            freeze.validate_event(
                event, ".github/vellum-change-events/20260722-design-fix.json"
            )

    def test_nested_event_path_cannot_duplicate_a_durable_event_id(self):
        with self.assertRaisesRegex(freeze.FreezeError, "direct .json child"):
            freeze.validate_event(
                change_event(),
                ".github/vellum-change-events/nested/20260722-design-fix.json",
            )

    def test_unresolved_manifest_row_cannot_transfer(self):
        manifest = {
            "source_commit": "a" * 40,
            "entries": [
                {
                    "source_path": "core/view/src/design_ir_json.cpp",
                    "classification": "unresolved",
                }
            ],
        }
        with self.assertRaisesRegex(freeze.FreezeError, "forbidden cut classification"):
            freeze.validate_transferred_projection(mapping(), manifest)

    def test_test_only_manifest_row_can_transfer(self):
        manifest = {
            "source_commit": "a" * 40,
            "entries": [
                {
                    "source_path": "core/view/src/design_ir_json.cpp",
                    "classification": "test-only",
                }
            ],
        }
        freeze.validate_transferred_projection(mapping(), manifest)

    def test_same_slice_cannot_receive_conflicting_events(self):
        first = change_event("20260722-pulp-only")
        second = change_event("20260722-backport")
        second.update({"disposition": "framework-backport", "framework_commit": "c" * 40})
        with self.assertRaisesRegex(freeze.FreezeError, "exactly match"):
            freeze._validate_event_coverage(
                [("first.json", first), ("second.json", second)],
                {"design-ir": ["core/view/src/design_ir_json.cpp"]},
                set(),
                None,
                None,
                mapping(),
            )

    def test_historical_emergency_document_remains_parseable_after_expiry(self):
        event = change_event("20260722-replay")
        event.update({
            "disposition": "emergency-exception",
            "owner": "@owner",
            "expiry": "2026-07-29",
            "follow_up": "https://github.com/Generous-Corp/vellum/issues/1",
        })
        freeze.validate_event(
            event,
            ".github/vellum-change-events/20260722-replay.json",
        )

    def test_new_emergency_must_be_live_at_its_source_commit(self):
        event = change_event("20260722-stale-emergency")
        event.update({
            "disposition": "emergency-exception",
            "owner": "@owner",
            "expiry": "2026-07-29",
            "follow_up": "https://github.com/Generous-Corp/vellum/issues/1",
        })
        with self.assertRaisesRegex(freeze.FreezeError, "already expired"):
            freeze.validate_event(
                event,
                ".github/vellum-change-events/20260722-stale-emergency.json",
                commit_time=dt.datetime(2026, 7, 30, tzinfo=dt.timezone.utc),
            )

    def test_nonexistent_authority_commit_is_rejected(self):
        event = {
            "kind": "authority-transition",
            "vellum_authority_commit": "b" * 40,
            "counterpart": "provenance/authority/records/pending.json",
        }
        with mock.patch.object(
            freeze, "_github_json", return_value={"sha": "c" * 40}
        ):
            with self.assertRaisesRegex(freeze.FreezeError, "resolve exactly"):
                freeze.verify_authority_counterpart(
                    repo=pathlib.Path("/unused"),
                    base="a" * 40,
                    head="c" * 40,
                    manifest={},
                    events=[("authority.json", event)],
                    transferred={"design-ir"},
                    head_map=mapping(),
                    token="redacted-test-token",
                    app_jwt="redacted-app-jwt",
                )

    def test_nonexistent_backport_commit_is_rejected(self):
        event = change_event("20260722-backport-proof")
        event.update({"disposition": "framework-backport", "framework_commit": "d" * 40})
        with mock.patch.object(
            freeze, "_github_json", return_value={"sha": "e" * 40}
        ):
            with self.assertRaisesRegex(freeze.FreezeError, "did not resolve"):
                freeze.verify_framework_backports(
                    [("backport.json", event)], "redacted-test-token"
                )

    def test_vellum_trust_requires_exact_app_jwt_identity(self):
        responses = vellum_trust_responses()
        responses["https://api.github.com/app"] = {
            "id": freeze.EXPECTED_VELLUM_READER_APP_ID + 1
        }
        with mock.patch.object(
            freeze, "_github_json", side_effect=lambda url, _token: responses[url]
        ):
            with self.assertRaisesRegex(freeze.FreezeError, "wrong GitHub App"):
                freeze._verify_vellum_trust(
                    commit="b" * 40,
                    authority_ref="refs/tags/authority/native-design-kernel-v1",
                    token="redacted-test-token",
                    app_jwt="redacted-app-jwt",
                )

    def test_authority_ref_is_tag_only_and_app_jwt_is_mandatory(self):
        self.assertIsNotNone(
            freeze.AUTHORITY_REF_RE.fullmatch(
                "refs/tags/authority/native-design-kernel-v1"
            )
        )
        self.assertIsNone(
            freeze.AUTHORITY_REF_RE.fullmatch(
                "refs/heads/authority/native-design-kernel-v1"
            )
        )
        with self.assertRaisesRegex(freeze.FreezeError, "APP_JWT"):
            freeze.verify_authority_counterpart(
                repo=pathlib.Path("/unused"),
                base="a" * 40,
                head="b" * 40,
                manifest={},
                events=[],
                transferred={"design-ir"},
                head_map=mapping(),
                token="redacted-test-token",
                app_jwt=None,
            )

    def test_vellum_trust_requires_annotated_signed_tag_immutable_release_and_checks(self):
        cases = (
            ("tag_object_type", "commit", "annotated tag"),
            ("signature_verified", False, "unsigned"),
            ("immutable_release", False, "immutable release"),
            ("check_app_id", 999, "pinned checks"),
        )
        for field, replacement, message in cases:
            kwargs = {field: replacement}
            responses = vellum_trust_responses(**kwargs)
            with self.subTest(field=field), mock.patch.object(
                freeze,
                "_github_json",
                side_effect=lambda url, _token, values=responses: values[url],
            ):
                with self.assertRaisesRegex(freeze.FreezeError, message):
                    freeze._verify_vellum_trust(
                        commit="b" * 40,
                        authority_ref="refs/tags/authority/native-design-kernel-v1",
                        token="redacted-test-token",
                        app_jwt="redacted-app-jwt",
                    )

    def test_vellum_trust_accepts_exact_tag_release_and_app_bound_checks(self):
        responses = vellum_trust_responses()
        with mock.patch.object(
            freeze, "_github_json", side_effect=lambda url, _token: responses[url]
        ):
            freeze._verify_vellum_trust(
                commit="b" * 40,
                authority_ref="refs/tags/authority/native-design-kernel-v1",
                token="redacted-test-token",
                app_jwt="redacted-app-jwt",
            )

    def test_schema_v2_record_binds_prepared_candidate_and_active_projection(self):
        with tempfile.TemporaryDirectory() as temporary:
            repo = pathlib.Path(temporary)

            def git(*args: str) -> str:
                return subprocess.check_output(
                    ["git", "-C", str(repo), *args], text=True
                ).strip()

            def write(relative: str, value) -> None:
                path = repo / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                if isinstance(value, dict):
                    value = json.dumps(value, indent=2, sort_keys=True) + "\n"
                path.write_text(value, encoding="utf-8")

            git("init", "-q")
            git("config", "user.name", "Authority Test")
            git("config", "user.email", "authority@example.invalid")
            prepared = mapping("prepared", "pulp-authoritative-untransferred")
            write(".github/vellum-ownership.json", prepared)
            write("core/view/src/design_ir_json.cpp", "candidate\n")
            git("add", ".")
            git("commit", "-qm", "prepared candidate")
            candidate = git("rev-parse", "HEAD")
            ownership_blob = git(
                "rev-parse", f"{candidate}:.github/vellum-ownership.json"
            )
            source_blob = git(
                "rev-parse", f"{candidate}:core/view/src/design_ir_json.cpp"
            )

            active = mapping()
            event = authority_event()
            write(".github/vellum-ownership.json", active)
            write(
                ".github/vellum-change-events/20260722-authority-activation.json",
                event,
            )
            git("add", ".")
            git("commit", "-qm", "activate authority")
            head = git("rev-parse", "HEAD")

            cut_manifest = {
                "schema": "pulp.vellum.initial-cut-manifest.v1",
                "source_commit": "a" * 40,
                "entry_count": 1,
                "entries": [
                    {
                        "source_path": "core/view/src/design_ir_json.cpp",
                        "classification": "framework-core",
                        "classification_source": "test",
                        "git_blob_sha": "9" * 40,
                        "git_mode": "100644",
                        "selected_by": ["core/view/src/design_ir_json.cpp"],
                    }
                ],
            }
            record = {
                "schema_version": 2,
                "state": "pending-pulp-activation",
                "source_repository": "Generous-Corp/pulp",
                "framework_repository": "Generous-Corp/vellum",
                "pulp_extraction_base": "a" * 40,
                "historical_seed_commit": "d" * 40,
                "pulp_candidate_commit": candidate,
                "pulp_ownership_projection_blob": ownership_blob,
                "authority_start_commit": "e" * 40,
                "authority_record_ref": (
                    "refs/tags/authority/native-design-kernel-v1"
                ),
                "cut_manifest_sha256": freeze._canonical_sha256(cut_manifest),
                "authority_groups": [
                    {
                        "id": "native-design-kernel-v1",
                        "lineage_mode": (
                            "history-seed-ancestor-active-reimplementation"
                        ),
                        "pulp_legacy_slices": ["design-ir"],
                        "pulp_historical_seed_projection": {
                            "core/view/src/design_ir_json.cpp": {
                                "blob": "9" * 40,
                                "mode": "100644",
                                "classification": "framework-core",
                            }
                        },
                        "pulp_activation_candidate_projection": {
                            "core/view/src/design_ir_json.cpp": {
                                "blob": source_blob,
                                "mode": "100644",
                            }
                        },
                        "vellum_implementation_projection": {
                            "runtime/view.cpp": {
                                "blob": "8" * 40,
                                "mode": "100644",
                            }
                        },
                    }
                ],
                "pulp_activation": None,
                "approved_by": "@danielraffel",
                "approved_at": "2026-07-22T19:00:00Z",
            }
            encoded = base64.b64encode(
                json.dumps(record).encode("utf-8")
            ).decode("ascii")

            def github(url: str, _token: str):
                if url == "https://api.github.com/app":
                    return {"id": 9001}
                if url == "https://api.github.com/installation":
                    return {"app_id": 9001}
                if url.endswith("/repos/Generous-Corp/vellum"):
                    return {
                        "id": freeze.EXPECTED_FRAMEWORK_REPOSITORY_ID,
                        "full_name": "Generous-Corp/vellum",
                        "private": True,
                        "archived": False,
                    }
                if "/installation/repositories" in url:
                    return {
                        "total_count": 1,
                        "repositories": [
                            {"id": freeze.EXPECTED_FRAMEWORK_REPOSITORY_ID}
                        ],
                    }
                if "/git/ref/tags/" in url:
                    return {"object": {"type": "tag", "sha": "6" * 40}}
                if "/git/tags/" in url:
                    return {
                        "object": {"type": "commit", "sha": "b" * 40},
                        "verification": {"verified": True, "reason": "valid"},
                    }
                if "/releases/tags/" in url:
                    return {
                        "tag_name": "authority/native-design-kernel-v1",
                        "draft": False,
                        "immutable": True,
                        "published_at": "2026-07-23T22:00:00Z",
                    }
                if "/check-runs" in url:
                    return {
                        "check_runs": [
                            {
                                "name": name,
                                "head_sha": "b" * 40,
                                "conclusion": "success",
                                "app": {"id": app_id},
                            }
                            for name, app_id in freeze.EXPECTED_CHECK_APP_IDS.items()
                        ]
                    }
                if "/commits/" in url:
                    return {"sha": "b" * 40}
                if "/contents/" in url:
                    return {"encoding": "base64", "content": encoded}
                if "/git/ref/" in url:
                    return {"object": {"sha": "b" * 40}}
                raise AssertionError(f"unexpected URL: {url}")

            with mock.patch.object(
                freeze, "EXPECTED_VELLUM_READER_APP_ID", 9001
            ), mock.patch.object(freeze, "_github_json", side_effect=github):
                freeze.verify_authority_counterpart(
                    repo=repo,
                    base=candidate,
                    head=head,
                    manifest=cut_manifest,
                    events=[("authority.json", event)],
                    transferred={"design-ir"},
                    head_map=active,
                    token="redacted-test-token",
                    app_jwt="redacted-app-jwt",
                )

            record["pulp_ownership_projection_blob"] = "7" * 40
            encoded = base64.b64encode(
                json.dumps(record).encode("utf-8")
            ).decode("ascii")
            with mock.patch.object(
                freeze, "EXPECTED_VELLUM_READER_APP_ID", 9001
            ), mock.patch.object(freeze, "_github_json", side_effect=github):
                with self.assertRaisesRegex(freeze.FreezeError, "ownership blob"):
                    freeze.verify_authority_counterpart(
                        repo=repo,
                        base=candidate,
                        head=head,
                        manifest=cut_manifest,
                        events=[("authority.json", event)],
                        transferred={"design-ir"},
                        head_map=active,
                        token="redacted-test-token",
                        app_jwt="redacted-app-jwt",
                    )


class FreezeGitIntegrationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.repo = pathlib.Path(self.temporary.name)
        self._git("init", "-q")
        self._git("config", "user.name", "Freeze Test")
        self._git("config", "user.email", "freeze@example.invalid")
        self._write(".github/vellum-ownership.json", mapping())
        self._write(
            "docs/contracts/vellum-initial-cut-manifest.json",
            {
                "source_commit": "a" * 40,
                "entries": [
                    {
                        "source_path": "core/view/src/design_ir_json.cpp",
                        "classification": "framework-core",
                    }
                ],
            },
        )
        self._write("core/view/src/design_ir_json.cpp", "before\n")
        self._git("add", ".")
        self._git("commit", "-qm", "base")
        self.base = self._git("rev-parse", "HEAD").strip()

    def tearDown(self):
        self.temporary.cleanup()

    def _git(self, *args: str) -> str:
        return subprocess.check_output(
            ["git", "-C", str(self.repo), *args], text=True
        )

    def _write(self, relative: str, value):
        path = self.repo / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        if isinstance(value, dict):
            value = json.dumps(value, indent=2, sort_keys=True) + "\n"
        path.write_text(value, encoding="utf-8")

    def _commit(self, message: str) -> str:
        self._git("add", "-A")
        self._git("commit", "-qm", message)
        return self._git("rev-parse", "HEAD").strip()

    def _run(self, head: str) -> int:
        return freeze.main(
            [
                "--repo", str(self.repo),
                "--base", self.base,
                "--head", head,
                "--output", str(self.repo / "outbox.json"),
            ]
        )

    def test_frozen_change_without_event_fails(self):
        self._write("core/view/src/design_ir_json.cpp", "after\n")
        head = self._commit("missing event")
        self.assertEqual(self._run(head), 1)

    def test_frozen_change_with_added_event_passes(self):
        self._write("core/view/src/design_ir_json.cpp", "after\n")
        self._write(
            ".github/vellum-change-events/20260722-design-fix.json",
            change_event(),
        )
        head = self._commit("classified change")
        self.assertEqual(self._run(head), 0)
        outbox = json.loads((self.repo / "outbox.json").read_text())
        self.assertEqual(
            outbox["event_refs"][0]["path"],
            ".github/vellum-change-events/20260722-design-fix.json",
        )
        self.assertRegex(outbox["event_refs"][0]["sha256"], r"^[0-9a-f]{64}$")

    def test_existing_event_is_append_only(self):
        path = ".github/vellum-change-events/20260722-existing.json"
        self._write(path, change_event("20260722-existing"))
        self.base = self._commit("event baseline")
        event = change_event("20260722-existing")
        event["rationale"] = "Rewritten after review"
        self._write(path, event)
        head = self._commit("rewrite event")
        self.assertEqual(self._run(head), 1)

    def test_rename_out_of_frozen_path_is_detected(self):
        self._git("mv", "core/view/src/design_ir_json.cpp", "outside.cpp")
        head = self._commit("rename out of slice")
        self.assertEqual(self._run(head), 1)


if __name__ == "__main__":
    unittest.main()
