#!/usr/bin/env python3

import importlib.util
import json
import os
import py_compile
import tempfile
import unittest
from pathlib import Path
from unittest import mock


SCRIPT = Path(__file__).with_name("gpu_trace_overhead_acceptance.py")
ROOT = SCRIPT.resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("gpu_trace_overhead_acceptance", SCRIPT)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class GpuTraceOverheadAcceptanceTests(unittest.TestCase):
    def bound_receipt(self):
        head = MODULE.subprocess.run(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, check=True,
            capture_output=True, text=True,
        ).stdout.strip()
        trace = ROOT / "test/fixtures/perfetto-gpu/first-frame-pipeline-upload-stall.pftrace"
        binding = MODULE.source_binding(ROOT, head, trace)
        return {
            "source_revision": head,
            "integration_head": binding["integration_head"],
            "source_blobs": binding["source_blobs"],
            "artifacts": {"trace": {
                "role": "repository/test/fixtures/perfetto-gpu/"
                        "first-frame-pipeline-upload-stall.pftrace"
            }},
        }

    def human_review_document(self, trace_digest="1" * 64):
        return {
            "schema": MODULE.HUMAN_REVIEW_SCHEMA,
            "question": "gpu-startup",
            "artifact_sha256": trace_digest,
            "review_authority": {
                "kind": "independent-human-same-artifact-review",
                "reviewer_kind": "human",
                "reviewer": "Daniel Raffel",
                "git_commit_author": "Daniel Raffel <daniel@example.com>",
                "attestation": (
                    "I directly inspected this exact trace in Perfetto UI and "
                    "recorded the visible spans below."
                ),
            },
            "reviewed_utc": "2026-08-28T05:34:54Z",
            "ui_revision": "v58.3-11fbaed8",
            "delivery": "official localhost embedding protocol",
            "observed_spans": [
                {
                    "name": "gpu_pipeline_prepare", "duration_ns": 1_800_000,
                    "gpu_evidence_id": "44444444444444444444444444444444",
                    "frame_index": 0, "sequence": 1, "health_state": "healthy",
                },
                {
                    "name": "gpu_resource_upload", "duration_ns": 900_000,
                    "gpu_evidence_id": "44444444444444444444444444444444",
                    "frame_index": 0, "sequence": 2, "health_state": "healthy",
                },
            ],
        }

    @staticmethod
    def semantic_result():
        return {
            "contributors": [
                {
                    "stage": "pipeline-prepare", "duration_ns": 1_800_000,
                    "evidence_id": "44444444444444444444444444444444",
                    "frame_index": 0, "sequence": 1, "health_state": "healthy",
                },
                {
                    "stage": "resource-upload", "duration_ns": 900_000,
                    "evidence_id": "44444444444444444444444444444444",
                    "frame_index": 0, "sequence": 2, "health_state": "healthy",
                },
            ]
        }

    def test_validates_dedicated_human_review_for_same_startup_trace(self):
        document = self.human_review_document()
        result = MODULE.validate_independent_human_review_document(
            document, question="gpu-startup", trace_sha256="1" * 64,
            semantic_result=self.semantic_result(),
        )
        self.assertEqual(result, document)

    def test_mcp_response_timeout_terminates_the_child(self):
        session = MODULE.McpSession(Path("/bin/cat"), {})
        with mock.patch.object(MODULE.select, "select", return_value=([], [], [])):
            with self.assertRaisesRegex(RuntimeError, "response exceeded"):
                session._request("initialize")
        session.close()
        self.assertIsNotNone(session.process.returncode)

    def test_mcp_partial_line_cannot_escape_the_response_deadline(self):
        with tempfile.TemporaryDirectory() as temporary:
            executable = Path(temporary) / "partial-mcp"
            executable.write_text("#!/bin/sh\nprintf '{'\nsleep 10\n")
            executable.chmod(0o755)
            session = MODULE.McpSession(executable, {})
            with mock.patch.object(MODULE, "ANALYSIS_TIMEOUT_SECONDS", 0.05):
                with self.assertRaisesRegex(RuntimeError, "response exceeded"):
                    session._request("initialize")
            session.close()
            self.assertIsNotNone(session.process.returncode)

    def test_mcp_rejects_non_object_and_boolean_id_responses(self):
        for response in ([], {"jsonrpc": "2.0", "id": True, "result": {}}):
            with self.subTest(response=response):
                session = MODULE.McpSession(Path("/bin/cat"), {})
                try:
                    with mock.patch.object(
                        MODULE,
                        "read_bounded_process_line",
                        return_value=json.dumps(response) + "\n",
                    ):
                        with self.assertRaisesRegex(RuntimeError, "invalid pulp-mcp"):
                            session._request("initialize")
                finally:
                    session.close()

    def test_local_source_loader_ignores_planted_unchecked_bytecode(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "planted.py"
            cache = root / "__pycache__" / "planted.pyc"
            cache.parent.mkdir()
            source.write_text('origin = "bytecode"\n')
            py_compile.compile(
                str(source), cfile=str(cache), doraise=True,
                invalidation_mode=py_compile.PycInvalidationMode.UNCHECKED_HASH,
            )
            source.write_text('origin = "source"\n')
            with mock.patch.object(MODULE, "SCRIPT_DIR", root):
                loaded = MODULE._load_local_source("planted_local_source", source.name)
            self.assertEqual(loaded.origin, "source")

    def test_recorder_exports_no_callable_terminal_authority(self):
        self.assertIsNone(getattr(MODULE, "certify_fresh_recording", None))
        self.assertIsNone(getattr(MODULE, "terminal_acceptance_status", None))

    def test_human_review_rejects_different_trace_digest(self):
        with self.assertRaisesRegex(ValueError, "not bound to the measured trace"):
            MODULE.validate_independent_human_review_document(
                self.human_review_document(),
                question="gpu-startup", trace_sha256="2" * 64,
                semantic_result=self.semantic_result(),
            )

    def test_human_review_rejects_non_startup_question(self):
        with self.assertRaisesRegex(ValueError, "only to gpu-startup"):
            MODULE.validate_independent_human_review_document(
                self.human_review_document(),
                question="gpu-health", trace_sha256="1" * 64,
                semantic_result=self.semantic_result(),
            )

    def test_human_review_rejects_old_receipt_or_missing_authority(self):
        old_receipt = {"protocol": {"question": "gpu-startup"}}
        with self.assertRaisesRegex(ValueError, "document schema"):
            MODULE.validate_independent_human_review_document(
                old_receipt, question="gpu-startup", trace_sha256="1" * 64,
                semantic_result=self.semantic_result(),
            )
        missing_authority = self.human_review_document()
        del missing_authority["review_authority"]
        with self.assertRaisesRegex(ValueError, "lacks required fields: review_authority"):
            MODULE.validate_independent_human_review_document(
                missing_authority, question="gpu-startup", trace_sha256="1" * 64,
                semantic_result=self.semantic_result(),
            )
        magic_marker = self.human_review_document()
        magic_marker["terminal_status"] = "pass"
        with self.assertRaisesRegex(ValueError, "only the exact review fields"):
            MODULE.validate_independent_human_review_document(
                magic_marker, question="gpu-startup", trace_sha256="1" * 64,
                semantic_result=self.semantic_result(),
            )

    def test_human_review_rejects_stripped_visual_review_fields(self):
        for field in ("reviewed_utc", "ui_revision", "delivery"):
            with self.subTest(field=field):
                document = self.human_review_document()
                del document[field]
                with self.assertRaisesRegex(ValueError, f"lacks required fields: {field}"):
                    MODULE.validate_independent_human_review_document(
                        document, question="gpu-startup", trace_sha256="1" * 64,
                        semantic_result=self.semantic_result(),
                    )
        document = self.human_review_document()
        document["observed_spans"] = []
        with self.assertRaisesRegex(ValueError, "lacks observed span details"):
            MODULE.validate_independent_human_review_document(
                document, question="gpu-startup", trace_sha256="1" * 64,
                semantic_result=self.semantic_result(),
            )

    def test_human_review_rejects_agent_or_missing_human_identity(self):
        for reviewer_kind, reviewer in (
            (None, "Daniel Raffel"),
            ("agent", "Daniel Raffel"),
            ("human", "Codex visual acceptance agent"),
            ("human", "automated review bot"),
            ("human", "GPT-5 visual reviewer"),
            ("human", "OpenAI reasoning assistant"),
        ):
            with self.subTest(
                reviewer_kind=reviewer_kind, reviewer=reviewer
            ):
                document = self.human_review_document()
                authority = document["review_authority"]
                if reviewer_kind is None:
                    del authority["reviewer_kind"]
                else:
                    authority["reviewer_kind"] = reviewer_kind
                authority["reviewer"] = reviewer
                with self.assertRaisesRegex(
                    ValueError,
                    "human same-artifact authority|automated agent or model|identity fields",
                ):
                    MODULE.validate_independent_human_review_document(
                        document,
                        question="gpu-startup",
                        trace_sha256="1" * 64,
                        semantic_result=self.semantic_result(),
                    )

    def test_human_review_rejects_missing_or_untyped_contributor_fields(self):
        for semantic_field, observed_field, invalid in (
            ("duration_ns", "duration_ns", None),
            ("evidence_id", "gpu_evidence_id", None),
            ("frame_index", "frame_index", False),
            ("sequence", "sequence", -1),
            ("health_state", "health_state", "mystery"),
        ):
            with self.subTest(field=semantic_field, invalid=invalid):
                document = self.human_review_document()
                semantic = self.semantic_result()
                if invalid is None:
                    del semantic["contributors"][0][semantic_field]
                    del document["observed_spans"][0][observed_field]
                else:
                    semantic["contributors"][0][semantic_field] = invalid
                    document["observed_spans"][0][observed_field] = invalid
                with self.assertRaisesRegex(ValueError, "lacks typed"):
                    MODULE.validate_independent_human_review_document(
                        document, question="gpu-startup", trace_sha256="1" * 64,
                        semantic_result=semantic,
                    )

    def test_human_review_rejects_non_startup_document(self):
        document = self.human_review_document()
        document["question"] = "gpu-health"
        with self.assertRaisesRegex(ValueError, "must be for gpu-startup"):
            MODULE.validate_independent_human_review_document(
                document, question="gpu-startup", trace_sha256="1" * 64,
                semantic_result=self.semantic_result(),
            )

    def test_human_review_binds_prior_single_file_commit_and_exact_trace_name(self):
        with tempfile.TemporaryDirectory() as temporary:
            repository = Path(temporary) / "repository"
            repository.mkdir()
            for command in (
                ["git", "init", "-q"],
                ["git", "config", "user.email", "daniel@example.com"],
                ["git", "config", "user.name", "Daniel Raffel"],
            ):
                MODULE.subprocess.run(command, cwd=repository, check=True)
            (repository / "base").write_text("base\n")
            MODULE.subprocess.run(["git", "add", "base"], cwd=repository, check=True)
            MODULE.subprocess.run(
                ["git", "commit", "-qm", "base"], cwd=repository, check=True
            )
            digest = "1" * 64
            review_path = (
                repository / MODULE.HUMAN_REVIEW_PATH_PREFIX / f"{digest}.json"
            )
            review_path.parent.mkdir(parents=True)
            review_path.write_text(
                json.dumps(self.human_review_document(digest), sort_keys=True) + "\n"
            )
            relative = review_path.relative_to(repository).as_posix()
            MODULE.subprocess.run(["git", "add", relative], cwd=repository, check=True)
            MODULE.subprocess.run(
                ["git", "commit", "-qm", "human review"],
                cwd=repository, check=True,
            )
            (repository / "source").write_text("source\n")
            MODULE.subprocess.run(["git", "add", "source"], cwd=repository, check=True)
            MODULE.subprocess.run(
                ["git", "commit", "-qm", "source"], cwd=repository, check=True
            )
            source_revision = MODULE._git_text(repository, "rev-parse", "HEAD")
            human, source = MODULE.bind_independent_human_review_document(
                repository, source_revision, review_path,
                question="gpu-startup", trace_sha256=digest,
                semantic_result=self.semantic_result(),
            )
            self.assertEqual(human["artifact_sha256"], digest)
            self.assertTrue(source["review_commit_changed_exactly_this_file"])
            self.assertEqual(
                source["review_commit_author"],
                human["review_authority"]["git_commit_author"],
            )

    def test_human_review_rejects_mixed_publication_commit(self):
        with tempfile.TemporaryDirectory() as temporary:
            repository = Path(temporary) / "repository"
            repository.mkdir()
            for command in (
                ["git", "init", "-q"],
                ["git", "config", "user.email", "daniel@example.com"],
                ["git", "config", "user.name", "Daniel Raffel"],
            ):
                MODULE.subprocess.run(command, cwd=repository, check=True)
            (repository / "base").write_text("base\n")
            MODULE.subprocess.run(["git", "add", "base"], cwd=repository, check=True)
            MODULE.subprocess.run(
                ["git", "commit", "-qm", "base"], cwd=repository, check=True
            )
            digest = "1" * 64
            review_path = (
                repository / MODULE.HUMAN_REVIEW_PATH_PREFIX / f"{digest}.json"
            )
            review_path.parent.mkdir(parents=True)
            review_path.write_text(
                json.dumps(self.human_review_document(digest), sort_keys=True) + "\n"
            )
            (repository / "unrelated").write_text("mixed\n")
            MODULE.subprocess.run(
                ["git", "add", review_path.relative_to(repository).as_posix(), "unrelated"],
                cwd=repository, check=True,
            )
            MODULE.subprocess.run(
                ["git", "commit", "-qm", "mixed review"],
                cwd=repository, check=True,
            )
            (repository / "source").write_text("source\n")
            MODULE.subprocess.run(["git", "add", "source"], cwd=repository, check=True)
            MODULE.subprocess.run(
                ["git", "commit", "-qm", "source"], cwd=repository, check=True
            )
            with self.assertRaisesRegex(ValueError, "change exactly its review file"):
                MODULE.bind_independent_human_review_document(
                    repository, MODULE._git_text(repository, "rev-parse", "HEAD"),
                    review_path, question="gpu-startup", trace_sha256=digest,
                    semantic_result=self.semantic_result(),
                )

    def test_plan_binding_requires_exact_lowercase_hex(self):
        self.assertTrue(MODULE.valid_lower_hex("a" * 40, 40))
        self.assertTrue(MODULE.valid_lower_hex("0" * 64, 64))
        self.assertFalse(MODULE.valid_lower_hex("A" * 40, 40))
        self.assertFalse(MODULE.valid_lower_hex("a" * 39, 40))
        self.assertFalse(MODULE.valid_lower_hex("g" * 64, 64))

    def test_cli_and_mcp_must_share_one_source_revision(self):
        revision = "a" * 40
        self.assertFalse(MODULE.source_revisions_match(revision, ""))
        self.assertTrue(MODULE.source_revisions_match(revision, revision))
        self.assertFalse(MODULE.source_revisions_match(revision, "b" * 40))
        self.assertTrue(MODULE.valid_lower_hex(revision, 40))
        self.assertFalse(MODULE.valid_lower_hex("HEAD", 40))

    def test_object_shells_and_replaced_globals_cannot_mint_local_authority(self):
        directory_shell = object.__new__(MODULE.RetainedDirectoryClaim)
        directory_shell.closed = True
        tree_shell = object.__new__(MODULE.RetainedTreeClaim)
        tree_shell.closed = True
        shells = (
            directory_shell,
            object.__new__(MODULE.CombinedClaims),
            tree_shell,
            object.__new__(MODULE.RetainedClaimSet),
        )
        self.assertEqual(len(shells), 4)
        with mock.patch.object(MODULE, "assert_exact_live_head", return_value="f" * 40):
            self.assertIsNone(getattr(MODULE, "certify_fresh_recording", None))
        self.assertNotIn("fresh_recorder_certification", SCRIPT.read_text())
        after_claim_closure = SCRIPT.read_text().rsplit(
            "output_parent_claim.close()", 1
        )[1].split("return 0", 1)[0]
        self.assertIn("structural-evidence-written", after_claim_closure)
        self.assertNotRegex(after_claim_closure, r"(?i)terminal|certif")

    def test_fresh_certification_rejects_stale_parent_revision(self):
        parent = MODULE._git_text(ROOT, "rev-parse", "HEAD^")
        with self.assertRaisesRegex(ValueError, "exact recording checkout HEAD"):
            MODULE.assert_exact_live_head(ROOT, parent)

    def test_mixed_install_prefix_cannot_borrow_current_build_stamp(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build = root / "build"
            prefix = root / "prefix"
            pairs = {
                "pulp": (build / "pulp", prefix / "bin/pulp", b"rust-front"),
                "pulp-cpp": (
                    build / "tools/cli/pulp-cpp", prefix / "bin/pulp-cpp",
                    b"cpp-delegate",
                ),
                "pulp-mcp": (
                    build / "tools/mcp/pulp-mcp", prefix / "bin/pulp-mcp",
                    b"mcp-server",
                ),
            }
            for built, installed, payload in pairs.values():
                built.parent.mkdir(parents=True, exist_ok=True)
                installed.parent.mkdir(parents=True, exist_ok=True)
                built.write_bytes(payload)
                installed.write_bytes(payload)
                os.chmod(built, 0o755)
                os.chmod(installed, 0o755)
            def retained_identity():
                descriptor = os.open(prefix, MODULE._directory_open_flags())
                claim = MODULE.RetainedDirectoryClaim(
                    prefix,
                    descriptor,
                    MODULE._directory_identity(os.fstat(descriptor)),
                    "install-prefix",
                )
                try:
                    claim.seal()
                    build_outputs = MODULE._bind_build_outputs(build, prefix, claim)
                    return MODULE._build_install_binary_identity(
                        build, prefix, build_outputs, claim
                    )
                finally:
                    claim.close()

            identity = retained_identity()
            self.assertEqual(
                identity["pulp-mcp"]["sha256"],
                identity["pulp-mcp"]["build_output_sha256"],
            )
            stale_mcp = pairs["pulp-mcp"][1]
            stale_mcp.write_bytes(b"stale-mixed-prefix-mcp")
            os.chmod(stale_mcp, 0o755)
            with self.assertRaisesRegex(ValueError, "differ from the exact build output"):
                retained_identity()

    def test_retained_build_output_rejects_replace_and_restore(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build = root / "build"
            prefix = root / "prefix"
            prefix.mkdir()
            pairs = {
                "pulp": build / "pulp",
                "pulp-cpp": build / "tools/cli/pulp-cpp",
                "pulp-mcp": build / "tools/mcp/pulp-mcp",
            }
            for index, path in enumerate(pairs.values()):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(f"build-output-{index}".encode())
                os.chmod(path, 0o755)
            descriptor = os.open(prefix, MODULE._directory_open_flags())
            claim = MODULE.RetainedDirectoryClaim(
                prefix,
                descriptor,
                MODULE._directory_identity(os.fstat(descriptor)),
                "install-prefix",
            )
            try:
                claim.seal()
                MODULE._bind_build_outputs(build, prefix, claim)
                target = pairs["pulp"]
                moved = root / "moved-pulp"
                payload = target.read_bytes()
                target.rename(moved)
                target.write_bytes(payload)
                os.chmod(target, 0o755)
                target.unlink()
                moved.rename(target)
                with self.assertRaisesRegex(ValueError, "mutation event"):
                    claim.assert_current()
            finally:
                claim.close()

    def test_output_must_be_new_and_outside_every_protected_tree(self):
        with tempfile.TemporaryDirectory() as temporary:
            external = Path(temporary)
            allowed = external / "receipt.json"
            MODULE.validate_output_path(allowed, (ROOT,))

            with self.assertRaisesRegex(ValueError, "outside every protected tree"):
                MODULE.validate_output_path(ROOT / "receipt.json", (ROOT,))

            existing = external / "existing.json"
            existing.write_text("do not overwrite", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "must be a new path"):
                MODULE.validate_output_path(existing, (ROOT,))

            symlink = external / "receipt-link.json"
            symlink.symlink_to(existing)
            with self.assertRaisesRegex(ValueError, "must be a new path"):
                MODULE.validate_output_path(symlink, (ROOT,))

            with self.assertRaises(FileExistsError):
                MODULE.atomic_write_json(existing, {"must_not": "replace"})
            self.assertEqual(existing.read_text(encoding="utf-8"), "do not overwrite")

            published = external / "published.json"
            MODULE.atomic_write_json(published, {"status": "pass"})
            self.assertEqual(json.loads(published.read_text()), {"status": "pass"})

    def test_output_publication_rejects_parent_directory_swap(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            parent = root / "parent"
            parent.mkdir()
            moved = root / "moved-parent"
            output = parent / "receipt.json"
            real_link = MODULE.os.link

            def swap_parent_before_link(*args, **kwargs):
                parent.rename(moved)
                parent.mkdir()
                return real_link(*args, **kwargs)

            with mock.patch.object(MODULE.os, "link", side_effect=swap_parent_before_link):
                with self.assertRaisesRegex(ValueError, "output-parent"):
                    MODULE.atomic_write_json(output, {"status": "pass"})
            self.assertFalse(output.exists())
            self.assertFalse((moved / "receipt.json").exists())

    def test_output_publication_rejects_staged_inode_swap(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = root / "receipt.json"
            real_link = MODULE.os.link

            def swap_staged_file_before_link(source, destination, **kwargs):
                staged = root / source
                staged.rename(root / "original.tmp")
                staged.write_text('{"forged":true}\n', encoding="utf-8")
                return real_link(source, destination, **kwargs)

            with mock.patch.object(
                MODULE.os, "link", side_effect=swap_staged_file_before_link
            ):
                with self.assertRaisesRegex(ValueError, "identity changed"):
                    MODULE.atomic_write_json(output, {"status": "pass"})
            self.assertFalse(output.exists())

    def test_output_publication_rejects_in_place_receipt_mutation(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = root / "receipt.json"
            real_link = MODULE.os.link

            def mutate_after_link(source, destination, **kwargs):
                result = real_link(source, destination, **kwargs)
                output.write_text('{"forged":true}\n', encoding="utf-8")
                return result

            with mock.patch.object(MODULE.os, "link", side_effect=mutate_after_link):
                with self.assertRaisesRegex(ValueError, "bytes differ"):
                    MODULE.atomic_write_json(output, {"status": "pass"})
            self.assertFalse(output.exists())

    def test_output_parent_claim_spans_recording_before_publication(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            parent = root / "parent"
            parent.mkdir()
            output = parent / "receipt.json"
            moved = root / "moved-parent"
            claim = MODULE.retain_existing_directory(parent, "output-parent")
            try:
                parent.rename(moved)
                moved.rename(parent)
                with self.assertRaisesRegex(ValueError, "mutation event"):
                    MODULE.atomic_write_json(
                        output, {"status": "pass"}, claim
                    )
            finally:
                claim.close()
            self.assertFalse(output.exists())

    def test_fresh_directory_claim_binds_created_inode_not_swapped_staging(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            path = root / "prefix"
            real_rename = MODULE._renameat_no_replace

            def swap_then_publish(source_parent, source_name, destination_parent,
                                  destination_name):
                os.rename(
                    source_name, source_name + ".original",
                    src_dir_fd=source_parent, dst_dir_fd=source_parent,
                )
                os.mkdir(source_name, dir_fd=source_parent)
                return real_rename(
                    source_parent, source_name,
                    destination_parent, destination_name,
                )

            with mock.patch.object(
                MODULE, "_renameat_no_replace", side_effect=swap_then_publish
            ):
                with self.assertRaisesRegex(ValueError, "created inode"):
                    MODULE.claim_fresh_directory(path, "install-prefix")
            self.assertTrue(path.is_dir())

    def test_retained_git_tree_rejects_source_write_and_restore(self):
        with tempfile.TemporaryDirectory() as temporary:
            repository = Path(temporary) / "repository"
            repository.mkdir()
            for command in (
                ["git", "init", "-q"],
                ["git", "config", "user.email", "test@example.com"],
                ["git", "config", "user.name", "Test"],
            ):
                MODULE.subprocess.run(command, cwd=repository, check=True)
            source = repository / "source.cpp"
            source.write_text("exact\n", encoding="utf-8")
            MODULE.subprocess.run(["git", "add", "source.cpp"], cwd=repository, check=True)
            MODULE.subprocess.run(
                ["git", "commit", "-qm", "source"], cwd=repository, check=True
            )
            revision = MODULE._git_text(repository, "rev-parse", "HEAD")
            claim, evidence = MODULE.retain_git_source_tree(
                repository, revision, "test source"
            )
            try:
                self.assertEqual(evidence["revision"], revision)
                source.write_text("substituted\n", encoding="utf-8")
                source.write_text("exact\n", encoding="utf-8")
                with self.assertRaisesRegex(ValueError, "mutation event"):
                    claim.assert_current()
            finally:
                claim.close()

    def test_retained_generated_build_inputs_reject_write_and_restore(self):
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary) / "build"
            (build / "CMakeFiles").mkdir(parents=True)
            files = {
                "CMakeCache.txt": "CMAKE_BUILD_TYPE:STRING=Release\n",
                "build.ninja": "rule exact\n",
                "CMakeFiles/rules.ninja": "rule exact\n",
                "generated.hpp": "#define EXACT 1\n",
            }
            for relative, contents in files.items():
                path = build / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(contents, encoding="utf-8")
            claim, evidence = MODULE.retain_generated_build_inputs(
                build, "test build inputs"
            )
            try:
                self.assertEqual(
                    evidence["build_targets"], list(MODULE.BUILD_TARGETS)
                )
                self.assertTrue(evidence["forced_clean_before_build"])
                graph = build / "build.ninja"
                graph.write_text("rule substituted\n", encoding="utf-8")
                graph.write_text(files["build.ninja"], encoding="utf-8")
                with self.assertRaisesRegex(ValueError, "mutation event"):
                    claim.assert_current()
            finally:
                claim.close()

    def test_render_provider_claim_retains_actual_skia_dawn_and_v8_inputs(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build = root / "build"
            skia = root / "skia"
            skia_source = root / "skia-src"
            v8 = root / "v8"
            (build / "CMakeFiles").mkdir(parents=True)
            (skia / "build/include/include/core").mkdir(parents=True)
            (skia / "build/mac-gpu/lib/Release").mkdir(parents=True)
            (skia_source / "src/core").mkdir(parents=True)
            (v8 / "include").mkdir(parents=True)
            (v8 / "lib").mkdir(parents=True)
            (skia / "build/include/include/core/SkCanvas.h").write_text("skia")
            (skia_source / "src/core/SkCanvas.cpp").write_text("source")
            _manifest, skia_assets = MODULE._pinned_provider_asset_digests("Skia")
            _manifest, v8_assets = MODULE._pinned_provider_asset_digests("V8")
            (skia / ".skia-asset-sha256").write_text(
                sorted(skia_assets)[0] + "\n"
            )
            skia_library = skia / "build/mac-gpu/lib/Release/libskia.a"
            skia_library.write_bytes(b"skia archive")
            (skia / "build/mac-gpu/lib/Release/libdawn_combined.a").write_bytes(
                b"dawn archive"
            )
            (v8 / "include/v8.h").write_text("v8")
            (v8 / ".v8-asset-sha256").write_text(sorted(v8_assets)[0] + "\n")
            runtime = v8 / "lib/libv8.dylib"
            runtime.write_bytes(b"v8 runtime")
            (build / "CMakeCache.txt").write_text(
                "PULP_HAS_SKIA:INTERNAL=TRUE\n"
                "PULP_JS_ENGINE:STRING=v8\n"
                f"SKIA_DIR:PATH={skia}\n"
                f"V8_RUNTIME_LIBRARY:FILEPATH={runtime}\n"
            )
            (build / "build.ninja").write_text(f"build x: link {skia_library}\n")
            (build / "CMakeFiles/rules.ninja").write_text("rule link\n")
            claim, evidence = MODULE.retain_resolved_render_provider_inputs(
                build, "test providers"
            )
            try:
                self.assertEqual(
                    set(evidence["providers"]),
                    {"skia_dawn", "skia_source", "v8"},
                )
                self.assertEqual(
                    evidence["skia_source_disposition"],
                    "retained-complete-adjacent-source-tree",
                )
                self.assertEqual(
                    evidence["providers"]["skia_source"]["required_members"][0][
                        "path"
                    ],
                    "src/core/SkCanvas.cpp",
                )
                self.assertEqual(
                    evidence["providers"]["skia_dawn"]["root_authority"][
                        "provider_layout"
                    ],
                    "build/<platform>-gpu/lib/Release",
                )
                skia_library.write_bytes(b"substituted")
                skia_library.write_bytes(b"skia archive")
                with self.assertRaisesRegex(ValueError, "mutation event"):
                    claim.assert_full()
            finally:
                claim.close()

    def test_complete_tree_rejects_provider_symlink_escape(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            provider = root / "provider"
            outside = root / "outside"
            provider.mkdir()
            outside.write_text("outside")
            (provider / "escape").symlink_to(outside)
            with self.assertRaisesRegex(ValueError, "absolute symlink|symlink escape"):
                MODULE.retain_current_regular_tree(provider, "test provider")

    def test_provider_resolution_rejects_organization_or_monorepo_roots(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            organization = root / "organization"
            nested = organization / "products/render-provider"
            archive_dir = nested / "build/mac-gpu/lib/Release"
            header_dir = nested / "build/include/include/core"
            archive_dir.mkdir(parents=True)
            header_dir.mkdir(parents=True)
            (organization / "unrelated-product").mkdir()
            (nested / ".skia-asset-sha256").write_text(
                sorted(MODULE._pinned_provider_asset_digests("Skia")[1])[0] + "\n"
            )
            skia = archive_dir / "libskia.a"
            skia.write_bytes(b"skia")
            (archive_dir / "libdawn_combined.a").write_bytes(b"dawn")
            (header_dir / "SkCanvas.h").write_text("header")
            build = root / "build"
            (build / "CMakeFiles").mkdir(parents=True)
            (build / "CMakeCache.txt").write_text(
                f"SKIA_DIR:PATH={organization}\n"
            )
            (build / "build.ninja").write_text(f"build x: link {skia}\n")
            (build / "CMakeFiles/rules.ninja").write_text("rule link\n")
            with self.assertRaisesRegex(ValueError, "exact-layout Skia/Dawn"):
                MODULE._resolved_skia_root(
                    build, MODULE._cmake_cache(build)
                )

            v8_org = root / "v8-organization"
            runtime = v8_org / "products/v8/lib/libv8.dylib"
            runtime.parent.mkdir(parents=True)
            runtime.write_bytes(b"v8")
            (v8_org / "include").mkdir()
            (v8_org / "include/v8.h").write_text("decoy")
            with self.assertRaisesRegex(ValueError, "direct include/v8.h"):
                MODULE._resolved_v8_root({"V8_RUNTIME_LIBRARY": str(runtime)})

    def test_claimed_install_prefix_rejects_path_substitution(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            prefix = root / "prefix"
            prefix.mkdir()
            descriptor = os.open(prefix, MODULE._directory_open_flags())
            claim = MODULE._directory_identity(os.fstat(descriptor))
            moved = root / "moved-prefix"
            prefix.rename(moved)
            prefix.mkdir()
            try:
                with self.assertRaisesRegex(ValueError, "no longer names"):
                    MODULE._assert_directory_path_identity(
                        prefix, descriptor, claim, "install-prefix"
                    )
            finally:
                os.close(descriptor)

    def test_retained_executable_claim_rejects_child_inode_swap(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            prefix = root / "prefix"
            binary = prefix / "bin/pulp"
            binary.parent.mkdir(parents=True)
            binary.write_bytes(b"claimed executable")
            descriptor = os.open(prefix, MODULE._directory_open_flags())
            claim = MODULE.RetainedDirectoryClaim(
                prefix,
                descriptor,
                MODULE._directory_identity(os.fstat(descriptor)),
                "install-prefix",
            )
            claim.bind_file(binary, "installed Rust CLI", MODULE.sha256(binary))
            claim.seal()
            moved = binary.with_name("pulp-original")
            binary.rename(moved)
            binary.write_bytes(b"substituted executable")
            binary.unlink()
            moved.rename(binary)
            try:
                with self.assertRaisesRegex(ValueError, "mutation event"):
                    claim.assert_current()
            finally:
                claim.close()

    def test_checked_in_receipt_uses_one_source_checkout(self):
        receipt = json.loads(
            (ROOT / "docs" / "validation" / "gpu-trace-overhead" /
             "m3-a2t-offline-analysis-20260828.json").read_text(encoding="utf-8")
        )
        self.assertEqual(receipt["source_revision"], receipt["mcp_source_revision"])
        self.assertTrue(receipt["artifacts"]["sibling_binding"]
                        ["verified_same_resolved_parent"])
        disposition = receipt["producer_overhead_disposition"]
        self.assertEqual(disposition["formal_plan_status"], "accepted-canonical-plan")
        self.assertTrue(MODULE.valid_lower_hex(disposition["formal_plan_revision"], 40))
        self.assertTrue(MODULE.valid_lower_hex(disposition["formal_plan_sha256"], 64))

    def test_checked_in_receipt_is_stale_until_exact_source_binding_is_regenerated(self):
        receipt = json.loads(
            (ROOT / "docs" / "validation" / "gpu-trace-overhead" /
             "m3-a2t-offline-analysis-20260828.json").read_text(encoding="utf-8")
        )
        errors = MODULE.source_binding_errors(receipt, ROOT)
        self.assertIn("integration_head must be an exact Git SHA", errors)
        self.assertIn(
            (
                "source_blobs does not bind the exact A2T "
                "behavior/build/fixture/producer-authority set"
            ),
            errors,
        )

    def test_exact_current_source_binding_passes(self):
        self.assertEqual(MODULE.source_binding_errors(self.bound_receipt(), ROOT), [])

    def test_planted_stale_head_and_declared_blob_fail_closed(self):
        stale_head = self.bound_receipt()
        stale_head["integration_head"] = "0" * 40
        errors = MODULE.source_binding_errors(stale_head, ROOT)
        self.assertTrue(any("source blob mismatch" in error for error in errors))

        stale_blob = self.bound_receipt()
        target = "experimental/pulp-rs/src/cmd/trace_gpu_analysis.rs"
        stale_blob["source_blobs"][target] = "0" * 40
        errors = MODULE.source_binding_errors(stale_blob, ROOT)
        self.assertIn(f"source blob mismatch for {target}", errors)
        self.assertIn(f"current HEAD source blob drift for {target}", errors)

    def test_planted_current_head_and_checkout_drift_fail_closed(self):
        receipt = self.bound_receipt()
        target = ".agents/skills/trace-sql/pulp_gpu_probe_correlation.sql"
        original_git_blobs = MODULE.git_blobs

        def drifted_head(repository, revision, paths):
            blobs = original_git_blobs(repository, revision, paths)
            if revision == "HEAD":
                blobs[target] = "f" * 40
            return blobs

        with mock.patch.object(MODULE, "git_blobs", side_effect=drifted_head):
            errors = MODULE.source_binding_errors(receipt, ROOT)
        self.assertIn(f"current HEAD source blob drift for {target}", errors)

        checkout = MODULE.checkout_blobs(ROOT, set(receipt["source_blobs"]))
        checkout[target] = "e" * 40
        with mock.patch.object(MODULE, "checkout_blobs", return_value=checkout):
            errors = MODULE.source_binding_errors(receipt, ROOT)
        self.assertIn(f"current checkout source blob drift for {target}", errors)

    def test_checked_in_agent_review_is_not_human_acceptance(self):
        receipt = json.loads(
            (ROOT / "docs" / "validation" / "gpu-trace-overhead" /
             "m3-a2t-offline-analysis-20260828.json").read_text(encoding="utf-8")
        )
        trace_digest = receipt["artifacts"]["trace"]["sha256"]
        self.assertEqual(receipt["protocol"]["question"], "gpu-startup")
        with self.assertRaisesRegex(ValueError, "document schema"):
            MODULE.validate_independent_human_review_document(
                receipt,
                question=receipt["protocol"]["question"],
                trace_sha256=trace_digest,
                semantic_result=receipt["semantic_result"],
            )

    def test_percentile_interpolates(self):
        self.assertEqual(MODULE.percentile([1.0, 2.0, 3.0], 0.5), 2.0)
        self.assertAlmostEqual(MODULE.percentile([0.0, 10.0], 0.95), 9.5)

    def test_summary_retains_tail_and_noise(self):
        result = MODULE.summary([1_000_000, 2_000_000, 3_000_000])
        self.assertEqual(result["count"], 3)
        self.assertEqual(result["median_ms"], 2.0)
        self.assertEqual(result["p95_ms"], 2.9)
        self.assertEqual(result["mad_ms"], 1.0)

    def test_bootstrap_is_deterministic_and_paired(self):
        first = MODULE.bootstrap_median_delta_ci([1, 2, 3], [11, 12, 13])
        second = MODULE.bootstrap_median_delta_ci([1, 2, 3], [11, 12, 13])
        self.assertEqual(first, second)
        self.assertEqual(first["mcp_minus_cli_median_ms"], 0.00001)

    def test_semantic_projection_excludes_transport_only_fields(self):
        payload = {"schema": "pulp.trace-gpu-analysis.v1", "question": "gpu-health",
                   "verdict": "pass", "capture_complete": True,
                   "dominant_stage": "health-transition", "transport_debug": "not semantic"}
        self.assertNotIn("transport_debug", MODULE.semantic_projection(payload))

    def test_parse_analysis_rejects_unknown_verdict(self):
        with self.assertRaises(RuntimeError):
            MODULE.parse_analysis(
                {"schema": "pulp.trace-gpu-analysis.v1", "verdict": "ok"}, surface="test"
            )

    def test_mixed_commit_inventory_is_limited_to_a2t_manifest_paths(self):
        sql = ".agents/skills/trace-sql/pulp_gpu_startup_breakdown.sql"
        changed = [sql, "tools/cli/gpu_probe/src/dpr_measurement_session.cpp"]
        self.assertEqual(MODULE.path_limited_changed_paths(changed, {sql}), [sql])

    def test_scope_inventory_rejects_moving_ref(self):
        with self.assertRaises(ValueError):
            MODULE.a2t_scope_inventory(Path("/repo"), "HEAD")

    def test_scope_manifest_matches_authoritative_current_path_contract(self):
        head = MODULE._git_text(ROOT, "rev-parse", "HEAD")
        manifest = MODULE._load_a2t_scope_manifest(ROOT, head)
        paths, accepted, external = MODULE.authoritative_a2t_scope_paths(ROOT, head)
        self.assertEqual(set(manifest["scope_paths"]), paths)
        self.assertEqual(
            len(accepted),
            manifest["accepted_plan_implementation"]["path_count"],
        )
        self.assertEqual(
            [row["path"] for row in external],
            [
                "core/view/platform/mac/window_host_mac.mm",
                "inspect/src/control_gpu_health_provider.cpp",
            ],
        )
        self.assertEqual(
            external[0]["introducing_revision"],
            "b4ba22f1d700621366afdbc72bb8615336964cd1",
        )
        self.assertEqual(
            external[0]["owner_package"],
            "input-to-present-latency-tracing",
        )
        self.assertEqual(
            external[0]["scope_authority"]["kind"],
            "immutable-git-commit-boundary",
        )
        self.assertFalse(
            external[0]["scope_authority"][
                "producer_introduction_touched_authority_path"
            ]
        )
        self.assertEqual(
            external[1]["introducing_revision"],
            "8175bd483f5e4ca66989c9ad91a4d9ed5a864bb0",
        )
        self.assertTrue(
            external[1]["scope_authority"][
                "producer_introduction_touched_authority_path"
            ]
        )
        self.assertEqual(
            external[1]["owner_evidence_status"],
            "external-not-evaluated-by-a2t",
        )

    def test_scope_manifest_cannot_omit_independently_discovered_semantic_path(self):
        head = MODULE._git_text(ROOT, "rev-parse", "HEAD")
        path = ROOT / MODULE.A2T_SCOPE_MANIFEST_PATH
        manifest = json.loads(path.read_text())
        manifest["scope_paths"].remove(".claude/commands/trace.md")
        with mock.patch.object(MODULE.json, "loads", return_value=manifest):
            with self.assertRaisesRegex(ValueError, "independently discovered"):
                MODULE._load_a2t_scope_manifest(ROOT, head)

    def test_scope_manifest_cannot_omit_external_product_producer_authority(self):
        head = MODULE._git_text(ROOT, "rev-parse", "HEAD")
        with mock.patch.object(
            MODULE, "_non_a2t_producer_authorities", return_value={}
        ):
            with self.assertRaisesRegex(ValueError, "unclassified product producer"):
                MODULE.authoritative_a2t_scope_paths(ROOT, head)

    def test_external_product_producer_authority_rejects_signature_tamper(self):
        head = MODULE._git_text(ROOT, "rev-parse", "HEAD")
        manifest = json.loads((ROOT / MODULE.A2T_SCOPE_MANIFEST_PATH).read_text())
        manifest["non_a2t_product_producer_authorities"][0][
            "added_producer_signatures"
        ]["core/view/platform/mac/window_host_mac.mm"].pop()
        with mock.patch.object(MODULE.json, "loads", return_value=manifest):
            with self.assertRaisesRegex(ValueError, "signature delta does not match"):
                MODULE._non_a2t_producer_authorities(ROOT, head)

    def test_semantic_discovery_uses_exact_identifiers_not_incidental_symptoms(self):
        self.assertTrue(
            MODULE.has_a2t_semantic_identifier(
                'schema = "pulp.trace-gpu-analysis.v1"'
            )
        )
        self.assertTrue(
            MODULE.has_a2t_semantic_identifier(
                'TRACE_EVENT("gpu", "gpu_submit", "debug.gpu_evidence_id", id)'
            )
        )
        self.assertIn("core/render", MODULE.A2T_SEMANTIC_DISCOVERY_PATHS)
        self.assertFalse(
            MODULE.has_a2t_semantic_identifier(
                "GPU startup tracing can help explain a slow first frame."
            )
        )

    def test_product_producer_discovery_uses_actual_source_macro_form(self):
        source = (
            'PULP_TRACE_SCOPE_NAMED_ARGS("gpu", "gpu_health_transition", '
            '"gpu_evidence_id", value);'
        )
        self.assertTrue(MODULE.is_product_producer_source(source))
        wrapper = (
            '#define RECORD_GPU_STAGE(name) \\\n'
            '  PULP_TRACE_SCOPE_NAMED("gpu", name)\n'
        )
        with self.assertRaisesRegex(ValueError, "literal event name"):
            MODULE.product_producer_signatures(wrapper)
        literal_wrapper = (
            '#define RECORD_GPU_SUBMIT() \\\n'
            '  PULP_TRACE_SCOPE_NAMED("render", "gpu_submit")\n'
        )
        self.assertEqual(
            MODULE.product_producer_signatures(literal_wrapper),
            ('PULP_TRACE_SCOPE_NAMED("render", "gpu_submit");',),
        )
        self.assertTrue(
            MODULE.is_product_producer_source(
                'PULP_TRACE_SCOPE_NAMED("gpu", "pipeline_compile");'
            )
        )
        for stage in ("gpu_acquire", "gpu_submit", "gpu_present"):
            self.assertTrue(
                MODULE.is_product_producer_source(
                    f'PULP_TRACE_SCOPE_NAMED("render", "{stage}");'
                )
            )
        self.assertTrue(
            MODULE.is_product_producer_source(
                'if (enabled) PULP_TRACE_SCOPE_NAMED("gpu", "pipeline_compile");'
            )
        )
        self.assertTrue(
            MODULE.is_product_producer_source(
                'do { PULP_TRACE_COUNTER("gpu", "queued", count); } while (false);'
            )
        )
        self.assertFalse(
            MODULE.is_product_producer_source(
                'config.gpu_evidence_id = "gpu_evidence_id";'
            )
        )
        self.assertFalse(
            MODULE.is_product_producer_source(
                'PULP_TRACE_SCOPE_NAMED("state", "pointer_dispatch");'
            )
        )
        self.assertFalse(
            MODULE.is_product_producer_source(
                'PULP_TRACE_SCOPE_NAMED_ARGS("render", "frame", '
                '"description", "gpu_present");'
            )
        )
        async_pair = (
            'PULP_TRACE_BEGIN("gpu", "gpu_async_submit");\n'
            'PULP_TRACE_END("gpu");'
        )
        self.assertEqual(
            MODULE.product_producer_signatures(async_pair),
            ('PULP_TRACE_BEGIN("gpu", "gpu_async_submit");',),
        )
        self.assertFalse(
            MODULE.is_product_producer_source('PULP_TRACE_END("gpu");')
        )
        self.assertFalse(
            MODULE.is_product_producer_source(
                '// PULP_TRACE_SCOPE_NAMED("gpu", "not_a_call");\n'
                'const char* text = "PULP_TRACE_SCOPE_NAMED(\\\"gpu\\\", '
                '\\\"also_not_a_call\\\");";'
            )
        )
        self.assertEqual(
            MODULE.product_producer_signatures(
                'PULP_TRACE_SCOPE_NAMED("gpu", /* incidental */ "gpu_submit");'
            ),
            MODULE.product_producer_signatures(
                'PULP_TRACE_SCOPE_NAMED("gpu", "gpu_submit");'
            ),
        )
        self.assertEqual(
            MODULE.product_producer_signatures(
                'PULP_TRACE_SCOPE_NAMED("gpu", "gpu_submit") /* note */;'
            ),
            MODULE.product_producer_signatures(
                'PULP_TRACE_SCOPE_NAMED("gpu", "gpu_submit");'
            ),
        )
        with self.assertRaisesRegex(ValueError, "terminating semicolon"):
            MODULE.product_producer_signatures(
                'PULP_TRACE_SCOPE_NAMED("gpu", "gpu_submit") /* note */'
            )
        oversized = (
            'PULP_TRACE_SCOPE_NAMED("gpu", "'
            + ("x" * MODULE.MAX_PRODUCT_TRACE_CALL_BYTES)
            + '");'
        )
        with self.assertRaisesRegex(ValueError, "exceeds the bounded scan"):
            MODULE.product_producer_signatures(oversized)
        with self.assertRaisesRegex(ValueError, "adjacent string literals"):
            MODULE.product_producer_signatures(
                'PULP_TRACE_SCOPE_NAMED("render", "g" "pu_submit");'
            )
        with self.assertRaisesRegex(ValueError, "literal event name"):
            MODULE.product_producer_signatures(
                'PULP_TRACE_SCOPE_NAMED("render", GPU_STAGE_NAME);'
            )
        with self.assertRaisesRegex(ValueError, "dynamic trace call"):
            MODULE.product_producer_signatures(
                'PULP_TRACE_SCOPE_DYNAMIC("render", stage_name);'
            )

    def test_product_producer_delta_catches_gpu_call_without_evidence_literal(self):
        before = 'PULP_TRACE_SCOPE_NAMED("gpu", "gpu_submit");'
        after = before + '\nPULP_TRACE_SCOPE_NAMED("gpu", "pipeline_compile");'
        before_signatures = MODULE.product_producer_signatures(before)
        after_signatures = MODULE.product_producer_signatures(after)
        self.assertTrue(
            MODULE.has_added_product_producer(before_signatures, after_signatures)
        )
        replacement = MODULE.product_producer_signatures(
            'PULP_TRACE_SCOPE_NAMED("gpu", "pipeline_compile");'
        )
        self.assertEqual(len(before_signatures), len(replacement))
        self.assertTrue(
            MODULE.has_added_product_producer(before_signatures, replacement)
        )

    def test_product_producer_must_have_independent_package_authority(self):
        head = MODULE._git_text(ROOT, "rev-parse", "HEAD")
        path = "inspect/src/control_gpu_health_provider.cpp"
        with (
            mock.patch.object(MODULE, "_a3_authority_paths", return_value=(set(), "a" * 40)),
            mock.patch.object(MODULE, "_non_a2t_producer_authorities", return_value={}),
        ):
            with self.assertRaisesRegex(ValueError, "unclassified product producer"):
                MODULE.classify_non_a2t_product_producers(ROOT, head, {path})

    def test_product_producer_introduction_must_touch_package_authority(self):
        head = MODULE._git_text(ROOT, "rev-parse", "HEAD")
        path = "inspect/src/control_gpu_health_provider.cpp"
        with (
            mock.patch.object(
                MODULE, "_a3_authority_paths", return_value=({path}, "a" * 40)
            ),
            mock.patch.object(
                MODULE, "_producer_introduction",
                return_value=("b" * 40, "feat(gpu): unrelated producer"),
            ),
            mock.patch.object(MODULE, "_revision_changed_paths", return_value={path}),
            mock.patch.object(MODULE, "_non_a2t_producer_authorities", return_value={}),
        ):
            with self.assertRaisesRegex(ValueError, "unclassified product producer"):
                MODULE.classify_non_a2t_product_producers(ROOT, head, {path})


if __name__ == "__main__":
    unittest.main()
