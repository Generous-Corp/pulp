# Local CI Retention And Pruning Plan

**Date:** 2026-04-04  
**Status:** reviewed with Claude / implementation in progress  
**Owner:** CI follow-up  

## Goal

Keep machine-global local-CI state operationally useful without letting
`~/Library/Application Support/Pulp/local-ci` grow without bound.

The immediate goal is not "delete everything aggressively." The goal is:

- keep the useful same-SHA prepared state that speeds up follow-up runs
- prune stale transfer artifacts that no longer provide value
- keep logs/results long enough to debug recent failures
- give the operator a safe manual cleanup path and a clear CLI surface

## Current Reality

Observed on one active development machine:

- `~/Library/Application Support/Pulp` = about `15G`
- `~/Library/Application Support/Pulp/local-ci` = about `15G`
- `~/Library/Application Support/Pulp/local-ci/bundles` = about `11G`
- `~/Library/Application Support/Pulp/local-ci/prepared/mac` = about `4.5G`
- `~/Library/Application Support/Pulp/local-ci/logs` = about `14M`
- `~/Library/Application Support/Pulp/local-ci/results` = about `472K`
- `~/Library/Application Support/Pulp/desktop-automation` = about `12M`

Counts at audit time:

- bundle files: `82`
- result files: `87`
- log directories: `83`

Bundle files are mostly about `135M` each, which strongly suggests "one bundle
per finished job" accumulation rather than an intended bounded cache.

Prepared mac state currently contains only two retained trees:

- `prepared/mac/full`
- `prepared/mac/smoke`

Those two trees are large, but they are not growing linearly with job count in
the same way bundles are.

## What The Code Currently Does

Relevant code:

- `tools/local-ci/local_ci.py`

Current behavior:

- queue metadata is trimmed to `KEEP_COMPLETED_JOBS = 25`
- result JSON files are written under `results/`
- per-job logs are written under `logs/`
- git bundles are written under `bundles/`
- prepared mac source/build/install trees are retained under `prepared/mac/...`

Important mismatch:

- queue metadata retention exists
- artifact retention does not appear to exist for `bundles/`, `logs/`,
  `results/`, or `prepared/`

So the queue can forget old completed jobs while their disk artifacts remain
forever.

## Problem Statement

Local CI currently behaves like a retention-free artifact store.

That creates three operational problems:

1. disk use scales with historical job count rather than recent debugging value
2. operators have no first-class visibility into what is safe to delete
3. prepared-state caching and accidental artifact buildup are mixed together

## Non-Goals

- deleting prepared state after every successful run
- removing debugging evidence needed for very recent failures
- inventing a complex quota manager before basic retention exists
- changing the semantics of same-SHA retest/resume in this first pass

## Retention Model

### 1. Bundles

Bundles are transfer artifacts, not primary evidence.

Policy:

- delete a bundle as soon as no pending/running job still references it
- keep a very small recent fallback window only if needed for crash recovery
- otherwise treat bundle retention as accidental, not desirable

Recommended v1 rule:

- default steady state: no completed-job bundles retained
- delete bundles once job completion is finalized and no pending/running job
  still references them

### 2. Prepared State

Prepared state is intentional performance cache.

Policy:

- keep at most one prepared tree per target and validation mode
- preserve explicit same-SHA reuse semantics
- expose an operator-visible way to clear prepared state deliberately

Recommended v1 rule:

- keep:
  - `prepared/<target>/full`
  - `prepared/<target>/smoke`
- allow explicit manual pruning
- use metadata-based staleness, not age-based staleness
- keep prepared state when its recorded SHA still matches a locally relevant
  branch/head
- if prepared metadata is missing or invalid, treat the tree as stale-but-warned
  rather than auto-deleting it immediately

### 3. Logs

Logs are useful evidence, but only for recent jobs.

Recommended v1 rule:

- retain logs only for jobs still present in queue history
- after queue trimming, delete orphaned log directories
- optionally keep a small fixed tail beyond queue retention, for example `25`
  to `50` jobs

### 4. Results

Results are small, structured evidence and more useful than raw bundles.

Recommended v1 rule:

- keep recent result files for the same retention window as completed queue jobs
- optionally keep slightly more than queue history because they are cheap

## CLI Surface

Add a small, explicit operator surface instead of hidden background deletion.

### New command

`pulp ci-local cleanup`

Modes:

- `--dry-run`
- `--apply`
- `--include-prepared`
- `--keep-results N`
- `--keep-logs N`
- `--keep-bundles N`

Output should say:

- what would be deleted
- why it is considered stale
- how much space would be reclaimed

### Status visibility

`pulp ci-local status` should report local state footprint, at least:

- bundles size
- prepared size
- logs size
- results size

That makes retention drift visible before it becomes a surprise.

## Automatic Pruning

Automatic pruning should be conservative and tied to existing lifecycle points.

Recommended first insertion point:

- after `finalize_job(...)` for completed jobs
- use the same retained/pruned job-id set that queue trimming already computes

Recommended automatic actions:

- delete orphaned bundles for completed jobs outside retention window
- delete orphaned logs/results outside retention window
- do not auto-delete prepared state in the first pass unless it is clearly
  detached from current prepared metadata
- gate automatic pruning on a shipped and manually used `cleanup --dry-run`
  path first

## Manual Cleanup Today

If no `pulp ci-local` run is active, the safest current reclaim targets are:

- `~/Library/Application Support/Pulp/local-ci/bundles`
- `~/Library/Application Support/Pulp/local-ci/prepared/mac/full`
- `~/Library/Application Support/Pulp/local-ci/prepared/mac/smoke`

Suggested operator commands:

```bash
du -sh "$HOME/Library/Application Support/Pulp/local-ci"/*
rm -rf "$HOME/Library/Application Support/Pulp/local-ci/bundles"
rm -rf "$HOME/Library/Application Support/Pulp/local-ci/prepared/mac/full"
rm -rf "$HOME/Library/Application Support/Pulp/local-ci/prepared/mac/smoke"
```

These should remain documented as emergency/manual cleanup, not the main UX.

## Implementation Plan

### Phase 1: Audit And Metadata

- add a small retention inventory helper
- teach status output to report local state footprint
- define which artifacts are referenced by still-live jobs
- verify whether prepared-state SHA metadata already exists; if not, add it

Status:

- implemented on `feature/local-ci-cleanup`

### Phase 2: Safe Manual Cleanup Command

- add `pulp ci-local cleanup --dry-run`
- add `--apply`
- start with bundles/logs/results only

Status:

- implemented on `feature/local-ci-cleanup`
- explicit `--dry-run` supported even though cleanup is dry-run by default
- `--include-prepared` is available for deliberate prepared-cache cleanup

### Phase 3: Automatic Orphan Pruning

- prune stale bundles/logs/results after job completion
- keep prepared state untouched except for explicit cleanup
- wire artifact pruning to the same completed-job trimming policy instead of
  maintaining separate retention boundaries

Status:

- implemented on `feature/local-ci-cleanup`

### Phase 4: Prepared-State Policy

- make prepared-state retention explicit and inspectable
- add prepared-state pruning rules that preserve same-SHA reuse value

Status:

- partially implemented via explicit `cleanup --include-prepared`
- automatic prepared-state pruning remains deferred

## Success Criteria

- steady-state local-CI disk usage no longer grows linearly with total job count
- bundle count no longer tracks historical completed-job count indefinitely
- operators can see disk footprint from the CLI
- operators can reclaim space with a first-class dry-run cleanup command
- prepared-state reuse remains intact for same-SHA retest/resume workflows

## Claude Alignment

Claude review agreed with the phased shape of the plan and suggested these
specific corrections:

- bundle retention should be simplified to delete-on-finalize, not "keep two for
  24h"
- prepared-state retention should be metadata-based, not age-based
- queue-retention and artifact-retention should share one policy boundary
- `cleanup --dry-run` should ship and be exercised before automatic pruning lands

Claude also called out a likely implementation requirement:

- prepared-state SHA metadata may need to be added if it is not already present

## Related Follow-Up

Separate telemetry fix to track alongside this work:

- avoid truncating billing-period cost rollups in `cmd_status`; period totals
  should use full history, not the 5-run display slice
