#!/usr/bin/env python3
"""Regression contract for the bounded Subrouter-backed recovery worker."""

from pathlib import Path
import unittest

import yaml


ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = ROOT / ".github" / "workflows" / "shipyard-recovery-worker.yml"
CONFIG = ROOT / "tools" / "shipyard" / "recovery.config.toml"
REPAIR_CONFIG = ROOT / "tools" / "shipyard" / "repair.config.toml"


class RecoveryWorkerWorkflowTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.text = WORKFLOW.read_text(encoding="utf-8")
        cls.doc = yaml.load(cls.text, Loader=yaml.BaseLoader)
        cls.config = CONFIG.read_text(encoding="utf-8")
        cls.repair_config = REPAIR_CONFIG.read_text(encoding="utf-8")

    def test_pilot_is_manual_unique_label_and_deduplicated(self) -> None:
        self.assertEqual(set(self.doc["on"]), {"workflow_dispatch"})
        job = self.doc["jobs"]["luna-triage"]
        self.assertIn("shipyard-recovery-pool", job["runs-on"])
        self.assertEqual(job["timeout-minutes"], "45")
        group = self.doc["concurrency"]["group"]
        for key in (
            "pr_number",
            "expected_head",
            "blocker_fingerprint",
            "assignment_epoch",
            "dispatch_attempt",
        ):
            self.assertIn(f"inputs.{key}", group)

    def test_status_mutation_is_explicitly_off_by_default(self) -> None:
        inputs = self.doc["on"]["workflow_dispatch"]["inputs"]
        self.assertIn("dispatch_attempt", inputs)
        self.assertNotIn("worker", inputs)
        self.assertNotIn("runner_name", inputs)
        self.assertEqual(inputs["publish_status"]["default"], "false")
        self.assertEqual(inputs["attempt_repair"]["default"], "false")
        self.assertIn("always() && inputs.publish_status", self.text)
        self.assertIn("context='shipyard/recovery-dispatch'", self.text)
        self.assertIn("completed attempt=${{ inputs.dispatch_attempt }}", self.text)
        self.assertIn("description=\"c=${classification}", self.text)
        self.assertEqual(self.text.count('[ "${#description}" -le 140 ]'), 1)
        self.assertEqual(self.text.count('[ "${#dispatch_description}" -le 140 ]'), 1)

    def test_actual_pool_runner_identity_is_fenced_before_checkout(self) -> None:
        steps = self.doc["jobs"]["luna-triage"]["steps"]
        self.assertEqual(steps[0]["name"], "Derive fenced recovery worker identity")
        for worker in ("m3", "m5", "m1"):
            self.assertIn(f"shipyard-recovery-{worker}-*", steps[0]["run"])
        self.assertIn("unfenced recovery runner name", steps[0]["run"])
        self.assertEqual(steps[1]["name"], "Install pinned Codex CLI in the disposable VM")
        self.assertEqual(
            steps[2]["name"], "Install pinned Claude fallback CLI in the disposable VM"
        )
        self.assertEqual(steps[3]["name"], "Checkout trusted recovery harness")

    def test_disposable_vm_installs_integrity_pinned_codex(self) -> None:
        job = self.doc["jobs"]["luna-triage"]
        self.assertEqual(job["env"]["CODEX_RECOVERY_VERSION"], "0.147.0")
        self.assertTrue(job["env"]["CODEX_RECOVERY_PACKAGE_INTEGRITY"].startswith("sha512-"))
        self.assertTrue(
            job["env"]["CODEX_RECOVERY_DARWIN_ARM64_INTEGRITY"].startswith("sha512-")
        )
        install = job["steps"][1]["run"]
        self.assertIn("npm view", install)
        self.assertIn("--global --ignore-scripts --prefix", install)
        self.assertIn('"codex-cli ${CODEX_RECOVERY_VERSION}"', install)
        self.assertIn('echo "$install_root/bin" >> "$GITHUB_PATH"', install)

    def test_admin_secret_exists_only_in_lease_steps(self) -> None:
        steps = self.doc["jobs"]["luna-triage"]["steps"]
        secret_steps = [
            step["name"]
            for step in steps
            if "SUBROUTER_SESSION_LEASE_ADMIN_TOKEN" in str(step)
        ]
        self.assertEqual(
            secret_steps,
            [
                "Acquire model-bound Subrouter lease",
                "Release Subrouter lease",
                "Acquire Claude fallback Subrouter lease",
                "Release Claude fallback Subrouter lease",
                "Acquire Sol-medium Subrouter lease",
                "Release Sol-medium Subrouter lease",
                "Acquire Claude fallback repair lease",
                "Release Claude fallback repair lease",
            ],
        )
        self.assertIn("chmod 600", self.text)
        self.assertNotIn("session-lease.json\n", self.text.split("path: |", 1)[1])

    def test_recovery_url_is_a_non_secret_repository_variable(self) -> None:
        # Four primary-lane lease steps plus four fallback-lane lease steps.
        self.assertEqual(
            self.text.count("vars.SUBROUTER_RECOVERY_BASE_URL"),
            8,
        )
        self.assertNotIn("secrets.SUBROUTER_RECOVERY_BASE_URL", self.text)

    def test_exact_head_and_dispatch_are_revalidated_before_model(self) -> None:
        self.assertIn("shipyard_recovery_worker.py", self.text)
        self.assertIn("commits/${EXPECTED_HEAD}/statuses", self.text)
        self.assertIn("for _ in $(seq 1 12)", self.text)
        self.assertIn('[ "$fenced" = 1 ]', self.text)
        self.assertIn("persist-credentials: false", self.text)
        self.assertIn("--strict-config exec", self.text)

    def test_admin_lease_is_minted_before_untrusted_head_checkout(self) -> None:
        lease = self.text.index("- name: Acquire model-bound Subrouter lease")
        checkout = self.text.index("- name: Checkout immutable PR head without credentials")
        triage = self.text.index("- name: Run one ephemeral Luna low triage")
        self.assertLess(lease, checkout)
        self.assertLess(checkout, triage)

    def test_profile_is_luna_low_ephemeral_read_only_and_lean(self) -> None:
        self.assertIn('model = "gpt-5.6-luna"', self.config)
        self.assertIn('model_reasoning_effort = "low"', self.config)
        self.assertIn('sandbox_mode = "read-only"', self.config)
        self.assertIn('env_key = "SUBROUTER_RECOVERY_TOKEN"', self.config)
        for disabled in ("plugins", "apps", "browser_use", "memories", "multi_agent", "goals"):
            self.assertIn(f"{disabled} = false", self.config)
        self.assertIn("--ephemeral", self.text)
        self.assertIn("--sandbox read-only", self.text)
        self.assertIn("--output-schema", self.text)

    def test_sol_repair_is_one_opt_in_fenced_post_triage_attempt(self) -> None:
        self.assertIn('model = "gpt-5.6-sol"', self.repair_config)
        self.assertIn('model_reasoning_effort = "medium"', self.repair_config)
        self.assertIn('sandbox_mode = "workspace-write"', self.repair_config)
        self.assertIn("network_access = false", self.repair_config)
        self.assertIn(
            "steps.triage_result.outputs.classification == 'needs_sol_fix'", self.text
        )
        self.assertEqual(self.text.count("Run one ephemeral Sol-medium repair"), 1)
        self.assertIn("--force-with-lease=\"refs/heads/${head_ref}:${EXPECTED_HEAD}\"", self.text)
        self.assertIn("Shipyard-Recovery-For", self.text)
        self.assertIn("shipyard/steward-handoff", self.text)

    def test_fallback_cli_is_integrity_pinned_like_the_primary(self) -> None:
        job = self.doc["jobs"]["luna-triage"]
        self.assertEqual(job["env"]["CLAUDE_FALLBACK_VERSION"], "2.1.233")
        self.assertTrue(
            job["env"]["CLAUDE_FALLBACK_PACKAGE_INTEGRITY"].startswith("sha512-")
        )
        self.assertTrue(
            job["env"]["CLAUDE_FALLBACK_DARWIN_ARM64_INTEGRITY"].startswith("sha512-")
        )
        install = job["steps"][2]["run"]
        self.assertIn("npm view", install)
        self.assertIn("--global --ignore-scripts --prefix", install)
        self.assertIn('"${CLAUDE_FALLBACK_VERSION} (Claude Code)"', install)
        # The platform package must be installed by name and bin/claude linked
        # straight at its binary. The base package alone leaves a wrapper that
        # resolves the native binary in a postinstall step, which
        # --ignore-scripts skips, so it exits "claude native binary not
        # installed" — this failed a live recovery job on 2026-08-16.
        self.assertIn(
            '"@anthropic-ai/claude-code-darwin-arm64@${CLAUDE_FALLBACK_VERSION}"',
            install,
        )
        self.assertIn("claude-code-darwin-arm64/claude", install)
        self.assertIn('ln -sf "$native" "$install_root/bin/claude"', install)
        self.assertIn('[ -x "$native" ]', install)

    def test_fallback_is_reactive_bounded_and_never_preempts_the_primary(self) -> None:
        steps = {step["name"]: step for step in self.doc["jobs"]["luna-triage"]["steps"]}
        # The fallback may only run after the primary lane actually failed, so a
        # healthy Codex account is never bypassed.
        self.assertEqual(
            steps["Acquire Claude fallback Subrouter lease"]["if"],
            "steps.triage.outcome == 'failure'",
        )
        self.assertEqual(
            steps["Acquire Claude fallback repair lease"]["if"],
            "steps.repair_prompt.outcome == 'success' && steps.repair.outcome == 'failure'",
        )
        # Exactly one additional attempt per lane.
        self.assertEqual(self.text.count("Run one ephemeral Claude fallback triage"), 1)
        self.assertEqual(self.text.count("Run one ephemeral Claude fallback repair"), 1)
        # A failed primary must not abort the job before the fallback can run.
        self.assertTrue(steps["Run one ephemeral Luna low triage"]["continue-on-error"])
        self.assertTrue(steps["Run one ephemeral Sol-medium repair"]["continue-on-error"])

    def test_fallback_lease_is_model_unbound_and_provider_explicit(self) -> None:
        # Subrouter rejects any request whose body model differs from a bound
        # lease, and the client issues background calls on a small fast model.
        # An empty model skips that check, but then the provider must be stated
        # or Subrouter defaults the lease back to codex.
        for step in ("fallback-lease-request.json", "fallback-repair-lease-request.json"):
            body = self.text.split(step)[0].rsplit("jq -n", 1)[-1]
            self.assertIn('provider:"claude"', body)
            self.assertNotIn("--arg model", body)
        self.assertEqual(self.text.count('.assignment.provider == "claude"'), 2)
        self.assertEqual(self.text.count("ANTHROPIC_AUTH_TOKEN | length > 0"), 2)

    def test_fallback_context_is_deliberately_minimal(self) -> None:
        # A default Claude Code launch loads plugin and MCP context that dwarfs
        # the bounded recovery prompt, so every optional surface stays off.
        self.assertEqual(self.text.count("--strict-mcp-config"), 2)
        self.assertEqual(self.text.count('--mcp-config \'{"mcpServers":{}}\''), 2)
        self.assertEqual(self.text.count('--settings \'{"disableAllHooks":true}\''), 2)
        self.assertEqual(self.text.count("--no-session-persistence"), 2)
        self.assertIn("--permission-mode plan", self.text)
        self.assertIn("--permission-mode acceptEdits", self.text)
        # Both lanes are confined to the untrusted worktree and receive their
        # bounded prompt over stdin, never argv, where any local process could
        # read the untrusted evidence it carries.
        self.assertEqual(self.text.count('< "$RUNNER_TEMP/recovery-input/prompt.txt"'), 2)
        self.assertEqual(
            self.text.count('< "$RUNNER_TEMP/recovery-input/repair-prompt.txt"'), 2
        )
        self.assertNotIn('"$(cat "$RUNNER_TEMP/recovery-input/prompt.txt")"', self.text)
        self.assertNotIn("--add-dir", self.text)
        # The CLI's --json-schema validator cannot resolve the committed
        # schema's "$schema" draft/2020-12 key and rejects the file
        # verbatim, which failed a live recovery job on 2026-08-16. Strip
        # exactly that key; the fenced validator still checks the payload
        # against the full schema afterwards.
        self.assertEqual(self.text.count('del(."$schema")'), 2)
        self.assertNotIn(
            '--json-schema "$(cat "$GITHUB_WORKSPACE/control/tools/shipyard/',
            self.text,
        )

    def test_fallback_output_is_revalidated_by_a_fenced_checker(self) -> None:
        # The Claude CLI validates its own structured output upstream, which
        # leaves no trusted local proof, so the worker re-checks the payload
        # with a validator that lives inside the recovery fence and therefore
        # cannot be weakened by a fenced repair model.
        self.assertIn(
            "control/tools/scripts/shipyard_recovery_result_check.py", self.text
        )
        self.assertTrue(
            Path("tools/scripts/shipyard_recovery_result_check.py").exists()
        )
        forbidden = Path("tools/scripts/shipyard_recovery_repair.py").read_text()
        self.assertIn('"tools/scripts/shipyard_recovery_"', forbidden)

    def test_the_publisher_will_not_push_a_repair_the_judgement_check_refuses(self) -> None:
        # Every other fence bounds how much a repair changed. On 2026-08-17 all
        # of them held while the lane deleted the atomics from ParamValue, so
        # the publisher -- the step that actually applies and pushes -- must
        # also weigh WHAT changed, and must record why when it refuses.
        self.assertIn(
            "control/tools/scripts/shipyard_recovery_judgement.py", self.text
        )
        self.assertIn("judgement_rc=$?", self.text)
        self.assertIn('if [ "$judgement_rc" = 3 ]; then', self.text)
        self.assertIn("shipyard/recovery-judgement", self.text)
        self.assertIn("needs_human", self.text)
        self.assertTrue(
            Path("tools/scripts/shipyard_recovery_judgement.py").exists()
        )
        # It must live inside the fence, or the model it constrains can edit it.
        forbidden = Path("tools/scripts/shipyard_recovery_repair.py").read_text()
        self.assertIn('"tools/scripts/shipyard_recovery_"', forbidden)

    def test_resolved_lane_drives_outputs_and_commit_trailers(self) -> None:
        outputs = self.doc["jobs"]["luna-triage"]["outputs"]
        self.assertEqual(
            outputs["triage_classification"],
            "${{ steps.triage_result.outputs.classification }}",
        )
        self.assertEqual(
            outputs["repair_outcome"], "${{ steps.repair_result.outputs.outcome }}"
        )
        # The audit trail must name the lane that actually produced the patch.
        # The trailers are one contiguous paragraph built with printf, not one
        # -m per trailer; see the parseable-block test below.
        self.assertIn('"Agent: ${repair_agent}"', self.text)
        self.assertIn('"Model: ${repair_model}"', self.text)
        self.assertIn("unknown repair provider", self.text)
        self.assertNotIn('-m "Agent: codex"', self.text)

    def test_both_lanes_failing_is_a_hard_error(self) -> None:
        # continue-on-error must not silently downgrade a total model outage to
        # a green job with no classification.
        self.assertIn("both triage lanes failed", self.text)
        self.assertIn("both repair lanes failed", self.text)

    def test_patch_is_diffed_against_the_commit_not_the_index(self) -> None:
        """`git add --intent-to-add` fully stages a DELETION, so a
        worktree-vs-index diff drops deleted and renamed files from both the
        patch and the declared path list — and the publisher's parity check
        still passes, because both lists are identically wrong. That would ship
        a repair that fails to delete what the model deleted."""
        self.assertIn("git -C worktree diff HEAD --name-only", self.text)
        self.assertIn("git -C worktree diff HEAD --binary --full-index", self.text)
        self.assertNotIn("git -C worktree diff --name-only", self.text)
        self.assertNotIn("git -C worktree diff --binary --full-index", self.text)
        # Reset to the fenced head, not HEAD, so a model that committed cannot
        # reset the index to its own commit and yield an empty patch.
        self.assertIn('git -C worktree reset --mixed --quiet "$EXPECTED_HEAD"', self.text)

    def test_parity_lists_are_sorted_under_a_pinned_locale(self) -> None:
        """The worker and the publisher run on different hosts and the parity
        check is a plain `diff -u` of two sorted lists. glibc/BSD collation
        orders `-`, `_`, and case differently from C, so an unpinned sort
        rejects a correct patch purely on locale."""
        self.assertEqual(self.text.count("LC_ALL=C sort -u"), 2)
        self.assertNotIn("--name-only | sort -u", self.text)

    def test_whitespace_is_a_warning_on_both_sides(self) -> None:
        """Demoting only the worker's check moves the rejection later, after
        the lease and model are already spent, instead of removing it."""
        self.assertIn(
            'git -C worktree diff HEAD --check || echo "warning:', self.text
        )
        self.assertIn(
            'git -C worktree diff --cached --check || echo "warning:', self.text
        )

    def test_fallback_repair_starts_from_a_clean_fenced_worktree(self) -> None:
        """The primary lane edits the same worktree and can fail AFTER editing.
        Without a reset, its partial edits are bundled into the fallback's patch
        and attributed to Claude by the commit trailers."""
        self.assertIn('git -C worktree reset --hard --quiet "$EXPECTED_HEAD"', self.text)
        self.assertIn("git -C worktree clean -qfd", self.text)

    def test_untrusted_project_settings_are_not_loaded(self) -> None:
        """cwd is inside the untrusted PR head. A PR-supplied settings file
        granting Bash would hand the model a shell with the lease token
        exported, and this lane has no OS sandbox."""
        self.assertEqual(self.text.count("--setting-sources user"), 2)

    def test_recovery_trailers_are_one_parseable_block(self) -> None:
        """Separate -m flags become separate paragraphs, so
        `git interpret-trailers --parse` sees only the last one."""
        self.assertIn('git -C worktree commit -m "$recovery_message"', self.text)
        self.assertNotIn('-m "Shipyard-Recovery-Epoch:', self.text)
        self.assertNotIn('-m "Agent: ${repair_agent}"', self.text)

    def test_a_fixed_repair_that_never_pushed_is_not_reported_success(self) -> None:
        """`repair_push` skipping leaves outcome `skipped`, not `failure`; a
        bare `= failure` test would report a clean dispatch while the authorised
        repair evaporated."""
        self.assertIn(
            '{ [ "${{ needs.luna-triage.outputs.repair_outcome }}" = fixed ] && '
            '[ "${{ steps.repair_push.outcome }}" != success ]; }',
            self.text,
        )

    def test_push_credential_is_minted_only_after_models_exit(self) -> None:
        model_end = self.text.index("Release Sol-medium Subrouter lease")
        token = self.text.index("Mint post-model Shipyard repair token")
        push = self.text.index("Fence, commit, and push repaired exact head")
        self.assertLess(model_end, token)
        self.assertLess(token, push)
        self.assertIn("permission-contents: write", self.text)
        publisher = self.doc["jobs"]["publish-recovery-result"]
        self.assertEqual(publisher["runs-on"], "ubuntu-latest")
        model_job = str(self.doc["jobs"]["luna-triage"])
        self.assertNotIn("SHIPYARD_APP_PRIVATE_KEY", model_job)
        self.assertNotIn("git push", model_job)

    def test_artifact_excludes_prompt_logs_and_lease_material(self) -> None:
        upload = next(
            step
            for step in self.doc["jobs"]["luna-triage"]["steps"]
            if step.get("name") == "Publish sanitized recovery artifact"
        )
        paths = upload["with"]["path"]
        self.assertIn("assignment.json", paths)
        self.assertIn("checks.json", paths)
        self.assertIn("recovery-result.json", paths)
        for forbidden in (
            "prompt.txt",
            "failed-checks.txt",
            "session-lease",
            # The fallback lane mints its own lease and writes a raw model
            # envelope; neither may ride out in the artifact.
            "fallback-lease",
            "recovery-result-fallback",
            "repair-result-fallback",
        ):
            self.assertNotIn(forbidden, paths)
        # Every lease file the job can create must be removed by its release
        # step, so a failed run leaves no broker credential on the VM.
        for lease in (
            "session-lease.json",
            "fallback-lease.json",
            "repair-lease.json",
            "fallback-repair-lease.json",
        ):
            self.assertIn(f'rm -f "$RUNNER_TEMP/{lease}"', self.text)


if __name__ == "__main__":
    unittest.main()
