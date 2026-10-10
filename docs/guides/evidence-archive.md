# Replay evidence archive

`tools/ci/evidence_archive.py` is a bounded, idempotent nightly collector for the local-model replay program. It belongs in Pulp `tools/ci` because it records Pulp CI facts while also accepting Shipyard and tartci normalized event streams; the envelope is storage-neutral and can move to a repo-backed corpus unchanged.

The Atelier destination is a **scratch cache**, not a backup or source of truth. Facts are retained for 180 days and the redacted session-error index for 30 days. Records are append-only JSONL by collection day and carry `schema`, `record_id`, `source`, `collected_at`, `observed_at`, `kind`, and a bounded `payload`. Duplicate records are ignored on repeat runs and old records are aged out.

## Backfill and nightly run

Run without installing anything:

```bash
python3 tools/ci/evidence_archive.py --root /Volumes/Atelier/pulp-replay-evidence --repo Generous-Corp/pulp --host-root /var/log/shipyard --sessions-root /Users/danielraffel/.codex/sessions
```

For a reviewable fixture/backfill, pass `--github-json` with an API response and explicit `--now`. The collector uses `ghapp api` with one paginated Actions-runs request when no fixture is supplied; it never stores raw logs.

Daniel's host-change commands, after review and explicit OK:

```bash
mkdir -p /Volumes/Atelier/pulp-replay-evidence
cp /ABSOLUTE/PATH/TO/pulp/tools/launchd/com.generouscorp.pulp.evidence-archive.plist ~/Library/LaunchAgents/
launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/com.generouscorp.pulp.evidence-archive.plist
launchctl kickstart -k gui/$(id -u)/com.generouscorp.pulp.evidence-archive
```

To remove it: `launchctl bootout gui/$(id -u) ~/Library/LaunchAgents/com.generouscorp.pulp.evidence-archive.plist` and delete the plist plus `/Volumes/Atelier/pulp-replay-evidence` when desired. The plist is deliberately not installed by this change.

## Where this goes next

When the experiment graduates, keep this JSONL envelope unchanged and publish it from a dedicated private repository or a versioned SQLite artifact with a small query CLI. That gives replay jobs immutable commits, reviewable retention changes, and fast indexed queries without coupling the collector to today's scratch drive.

## Size estimate

At 2 KiB per CI/PR/host fact and 1 KiB per session error, 10,000 facts plus 5,000 session errors per month is about 25 MiB/month before filesystem overhead. The rolling windows therefore stay comfortably below 200 MiB; actual counts are emitted by each run and recorded in the goal STATUS.
