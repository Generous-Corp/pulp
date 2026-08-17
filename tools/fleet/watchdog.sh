#!/usr/bin/env bash
# Fleet drift watchdog — continuous convergence check with graduated escalation.
#
# WHY THIS RUNS ON THE HOST, NOT IN GITHUB ACTIONS
#
# Drift is a property of a host, and only that host can observe it. Whether
# /Volumes/Workshop is mounted, what a launchd plist declares, how much disk is
# left, whether the role file says dedicated-builder -- a GitHub-hosted runner
# is structurally blind to every one of those. A scheduled workflow that claimed
# to check them could only ever check the ones reachable over the API (the
# routing variables), and would report a comforting green for the rest.
#
# So the per-host watchdog is a launchd agent. The scheduled workflow
# (.github/workflows/fleet-drift-watchdog.yml) does the one thing a central
# vantage point CAN do and a host cannot: compare receipts ACROSS hosts to find
# epoch lag and missing hosts. Neither can do the other's job.
#
# ESCALATION LADDER (graduated; a human is never a rung)
#
#   0  verify                      compliant -> clear state, done
#   1  auto-apply                  fix what apply.sh is allowed to fix, re-verify
#   2  retry                       one backoff pass; catches transient causes
#                                  (disk freed, volume remounted, network back)
#   3  escalate to owner agent     the tag's owner in OWNERS.md
#   4  escalate to fallback chain  that tag's fallbacks, in order
#   5  hold                        issue stays open with evidence attached
#
# Rung 5 is a terminal state, and it is a correct one. There is no rung that
# wakes a person. An open issue carrying three receipts and one unobservable
# host is more useful than a 3am notification.
set -uo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/../.." && pwd)"
MANIFEST="${FLEET_MANIFEST:-$REPO_ROOT/planning/fleet/manifest.toml}"
STATE_DIR="${FLEET_STATE_DIR:-$HOME/.local/state/pulp-fleet}"
ISSUE_REPO="${FLEET_ISSUE_REPO:-Generous-Corp/infrastructure-issues}"
DRY_RUN=0

usage() {
  cat <<'EOF'
usage: watchdog.sh [--manifest PATH] [--state-dir PATH] [--issue-repo OWNER/REPO] [--dry-run]

  --dry-run   run the full ladder and print the escalation that WOULD happen,
              without applying, without opening or commenting on any issue.
              State is still read but not written.

exit: 0 compliant or auto-healed | 10 escalated | 20 holding | 3 usage/harness error
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    --manifest)   MANIFEST="${2:-}"; shift 2 || { usage >&2; exit 3; } ;;
    --state-dir)  STATE_DIR="${2:-}"; shift 2 || { usage >&2; exit 3; } ;;
    --issue-repo) ISSUE_REPO="${2:-}"; shift 2 || { usage >&2; exit 3; } ;;
    --dry-run)    DRY_RUN=1; shift ;;
    -h|--help)    usage; exit 0 ;;
    *) echo "watchdog.sh: unknown argument: $1" >&2; usage >&2; exit 3 ;;
  esac
done

[ -f "$MANIFEST" ] || { echo "watchdog.sh: manifest not found: $MANIFEST" >&2; exit 3; }
command -v python3 >/dev/null 2>&1 || { echo "watchdog.sh: python3 required" >&2; exit 3; }
python3 -c 'import tomllib' 2>/dev/null || python3 -c 'import tomli' 2>/dev/null || {
  echo "watchdog.sh: python3 needs tomllib (>= 3.11) or tomli (pip install --user tomli)" >&2; exit 3; }

log() { printf '[fleet-watchdog %s] %s\n' "$(date -u +%H:%M:%SZ)" "$*"; }

# ---------------------------------------------------------------------------
# Rung 0 — verify.
# ---------------------------------------------------------------------------
run_verify() {
  "$SCRIPT_DIR/verify.sh" --manifest "$MANIFEST" --json 2>/dev/null
}

VERIFY_JSON="$(run_verify)"; VERIFY_RC=$?
if [ "$VERIFY_RC" -eq 3 ] || [ -z "$VERIFY_JSON" ]; then
  # A harness error is NOT drift. Escalating it as drift would send an agent to
  # fix a host that may be perfectly fine.
  log "verify.sh could not produce a verdict (rc=$VERIFY_RC) -- harness error, not drift"
  exit 3
fi

HOST="$(printf '%s' "$VERIFY_JSON" | python3 -c 'import json,sys; print(json.load(sys.stdin)["host"])')"
EPOCH="$(printf '%s' "$VERIFY_JSON" | python3 -c 'import json,sys; print(json.load(sys.stdin)["epoch"])')"
VERDICT="$(printf '%s' "$VERIFY_JSON" | python3 -c 'import json,sys; print(json.load(sys.stdin)["verdict"])')"

log "host=$HOST epoch=$EPOCH verdict=$VERDICT"

STATE_FILE="$STATE_DIR/${HOST}.watchdog.json"
[ "$DRY_RUN" -eq 1 ] || mkdir -p "$STATE_DIR"

if [ "$VERDICT" = "compliant" ]; then
  # Converging is only half the job. If this host has an open drift issue, the
  # issue is the only place the fleet-wide lifecycle gate can learn that the
  # host came back -- and that gate holds a rollout in `state:rolling-out` until
  # every declared host has posted a receipt marker. Clearing local state
  # without posting would leave the issue permanently short one receipt while
  # the host itself is fine, which is the exact "converged but unprovable"
  # failure the receipt rule exists to prevent.
  FLEET_HOST="$HOST" FLEET_EPOCH="$EPOCH" FLEET_STATE_FILE="$STATE_FILE" \
  FLEET_ISSUE_REPO="$ISSUE_REPO" FLEET_DRY="$DRY_RUN" python3 <<'PY' || true
import json, os, shutil, subprocess, sys

state_file = os.environ["FLEET_STATE_FILE"]
host, epoch = os.environ["FLEET_HOST"], os.environ["FLEET_EPOCH"]
dry = os.environ["FLEET_DRY"] == "1"
try:
    with open(state_file, encoding="utf-8") as fh:
        issue = json.load(fh).get("issue")
except Exception:
    issue = None
if not issue:
    sys.exit(0)

# The marker is deliberately NOT wrapped in a fence: the gate strips fenced
# blocks before counting, so a fenced marker is invisible to it.
#
# `verdict` is optional upstream -- a receipt that omits it is taken at its
# word. We always cite it anyway, because we always know it. A bare receipt
# asks the gate to trust us; a verdict-bearing one lets the gate CHECK us, and
# is rejected outright if this code ever posts a marker on a path where the
# host was not actually clean. Strictly more falsifiable for one attribute.
body = (f'<!-- infra-receipt host="{host}" epoch="{epoch}" verdict="compliant" -->\n\n'
        f"`{host}` re-verified **compliant** against fleet manifest epoch "
        f"{epoch}. Receipt marker above; posted automatically by "
        f"`tools/fleet/watchdog.sh` on convergence.")
if dry:
    print(f"  DRY RUN: would post receipt for {host} to issue #{issue}")
    sys.exit(0)
exe = shutil.which("ghapp")
if not exe:
    print("  ghapp not on PATH -- receipt NOT posted; issue still shows this "
          "host as missing", file=sys.stderr)
    sys.exit(0)
try:
    subprocess.run([exe, "issue", "comment", str(issue), "--repo",
                    os.environ["FLEET_ISSUE_REPO"], "--body", body],
                   check=True, capture_output=True, text=True, timeout=120)
    print(f"  posted receipt for {host} to issue #{issue}")
except Exception as exc:
    print(f"  receipt post failed: {exc}", file=sys.stderr)
PY
  log "compliant -- clearing escalation state"
  [ "$DRY_RUN" -eq 1 ] || rm -f "$STATE_FILE"
  exit 0
fi

# ---------------------------------------------------------------------------
# Rung 1 — auto-apply, then re-verify.
# ---------------------------------------------------------------------------
if [ "$DRY_RUN" -eq 1 ]; then
  log "rung 1: would run apply.sh (dry-run)"
  "$SCRIPT_DIR/apply.sh" --manifest "$MANIFEST" --dry-run >/dev/null 2>&1 || true
else
  log "rung 1: auto-applying"
  "$SCRIPT_DIR/apply.sh" --manifest "$MANIFEST" >/dev/null 2>&1 || true
  VERIFY_JSON="$(run_verify)"; VERIFY_RC=$?
  VERDICT="$(printf '%s' "$VERIFY_JSON" | python3 -c 'import json,sys; print(json.load(sys.stdin)["verdict"])' 2>/dev/null || echo error)"
  if [ "$VERDICT" = "compliant" ]; then
    log "rung 1 healed the host -- clearing state"
    rm -f "$STATE_FILE"
    exit 0
  fi
fi

# ---------------------------------------------------------------------------
# Rungs 2-5 — escalation ladder, driven by consecutive failures at this epoch.
# ---------------------------------------------------------------------------
FLEET_VERIFY_JSON="$VERIFY_JSON" \
FLEET_STATE_FILE="$STATE_FILE" \
FLEET_MANIFEST="$MANIFEST" \
FLEET_ISSUE_REPO="$ISSUE_REPO" \
FLEET_DRY_RUN="$DRY_RUN" \
FLEET_REPO_ROOT="$REPO_ROOT" \
python3 - "$SCRIPT_DIR" <<'PY'
import datetime, json, os, shutil, subprocess, sys
sys.path.insert(0, sys.argv[1])
import fleet_lib as F

verify = json.loads(os.environ["FLEET_VERIFY_JSON"])
state_file = os.environ["FLEET_STATE_FILE"]
dry = os.environ.get("FLEET_DRY_RUN") == "1"
issue_repo = os.environ["FLEET_ISSUE_REPO"]
manifest = F.load_manifest(os.environ["FLEET_MANIFEST"])

host, epoch = verify["host"], verify["epoch"]
drifted, unobs = verify["drifted"], verify["unobservable"]
now = datetime.datetime.now(datetime.timezone.utc)

# --- state ---------------------------------------------------------------
state = {"epoch": epoch, "attempts": 0, "rung": 1, "issue": None, "first_seen": now.isoformat(timespec="seconds")}
if os.path.exists(state_file):
    try:
        prev = json.load(open(state_file))
        # A NEW epoch resets the ladder. Drift against a value that only just
        # changed is expected -- the host has not been given a chance to
        # converge yet, and escalating it as a failure would fire on every
        # legitimate manifest change.
        if prev.get("epoch") == epoch:
            state = prev
    except Exception:
        pass

state["attempts"] = state.get("attempts", 0) + 1
attempts = state["attempts"]

# --- every tag this watchdog can file under ------------------------------
# Single declaration on purpose: `roster_check.py` parses this block by name and
# asserts each literal is in OWNERS.md's allowlist. That is what turns their
# next rename into a failed check instead of an issue stranded on an invalid
# tag -- which is exactly how `infra-m3-state` would have failed had it ever
# been filed under.
TAGS = {
    "disk": "infra-disk",
    "host_state": "infra-host-state",
    "routing": "infra-routing",
    "mixed": "infra-fleet-drift",
}

# --- which surfaces are in trouble, and therefore which tag --------------
# The tag IS the routing: OWNERS.md gives every tag its own chain, and their
# router derives the single `owner:` label from the tag alone. So naming the
# surface precisely is what puts the issue in front of the agent who owns it. A
# blanket tag would route every disk floor and every `PULP_*` move to
# `agent:fleet-ops` no matter who is actually responsible.
KIND_TAGS = {"disk_floor_gb": TAGS["disk"]}
host_kinds = {k["name"]: k.get("kind", "")
              for k in manifest["hosts"].get(host, {}).get("keys", [])}
surfaces = set()
implicated = set()
for name in drifted + unobs:
    if name in host_kinds:
        surfaces.add(f"host:{host}")
        # dir / file_content / launchd_env are all per-machine state.
        implicated.add(KIND_TAGS.get(host_kinds[name], TAGS["host_state"]))
    else:
        surfaces.add("routing")
        implicated.add(TAGS["routing"])

# Exactly one `infra-*` tag per issue -- their rule, and also the only way the
# derived `owner:` label can be unambiguous. One kind drifted: its own tag is
# the precise answer. Several did: no single tag is more true than the others,
# and drift spanning kinds IS fleet drift, whose chain starts at
# `agent:fleet-ops`. An empty set lands here too, which is the right default.
tag = implicated.pop() if len(implicated) == 1 else TAGS["mixed"]

# --- how many rungs the ladder has ---------------------------------------
# OWNERS.md routes every tag through exactly four roles -- one owner and three
# fallbacks. That fixed width is a property of their TABLE, not of any one
# chain, so the ladder is four escalating passes then hold whichever tag we
# file under.
#
# Deliberately a checked constant rather than a copy of the role names. The
# manifest used to mirror one tag's chain so the body could name a target; that
# mirror could only ever be right for one tag, and now that the tag is chosen by
# surface it would name the wrong owner for three of the four. `roster_check.py`
# asserts this width against the live table, so a schema change fails loudly
# instead of stranding the tail of the ladder the way the last hardcoded count
# did.
ROUTED_ROLES_PER_TAG = 4

# --- rung selection ------------------------------------------------------
# 2 = retry (transient), 3 = owner agent, 4 = fallback chain, 5 = hold.
# Rung 3 spends the owner; each rung-4 pass spends one fallback, so the last
# attempt that can still escalate is ROUTED_ROLES_PER_TAG + 1.
if attempts == 1:
    rung, action = 2, "retry"
elif attempts == 2:
    rung, action = 3, "owner"
elif attempts <= ROUTED_ROLES_PER_TAG + 1:
    rung, action = 4, "fallback"
else:
    rung, action = 5, "hold"
state["rung"] = rung

# --- no escalation target is named here ----------------------------------
# The tag routes. Their router derives one `owner:` label from it and walks that
# tag's own fallbacks; anything this script wrote into the body would be a
# second, independently-rotting answer to the same question, and the body's copy
# is the one that rots. The rung is local pacing -- how many cycles this drift
# has survived -- not a claim about who was notified.

unverified = [k["key"] for k in verify["keys"]
              if k["state"] != "ok" and not k["declared_verified"]]

summary = {
    "host": host, "epoch": epoch, "verdict": verify["verdict"],
    "drifted": drifted, "unobservable": unobs, "surfaces": sorted(surfaces),
    "attempts": attempts, "rung": rung, "action": action, "tag": tag,
    "declared_unverified": unverified,
}
print(json.dumps(summary, indent=2))

if action == "retry":
    print(f"rung 2: transient retry (attempt {attempts}); no escalation yet")
elif action == "hold":
    print(f"rung 5: HOLDING. Ladder exhausted after {attempts} attempts. "
          f"Issue stays open with evidence. No human is paged -- by design.")
else:
    print(f"rung {rung}: escalating via tag `{tag}`; "
          f"OWNERS.md resolves the owner and fallbacks for it")

# --- issue body ----------------------------------------------------------
# Grammar is owned by Generous-Corp/infrastructure-issues (.github/scripts/
# lifecycle.py). Two things it parses out of this body:
#
#   "### Affected hosts" followed by a comma-separated list -- the set of hosts
#   whose receipts the lifecycle gate will demand before the issue may claim
#   `state:verifying` or `state:resolved`.
#
#   <!-- infra-receipt host="..." epoch="..." --> -- one per converged host.
#   MUST sit outside any ``` fence: the gate strips fenced blocks first, so that
#   a documented example cannot let an issue self-certify. Our Evidence section
#   below IS fenced, which is why the marker is emitted before it and never
#   inside it.
title = f"[fleet-drift] {host} @ epoch {epoch}: {verify['verdict']}"
lines = [
    f"Host `{host}` is **{verify['verdict']}** against fleet manifest epoch {epoch}.",
    "",
    "### Affected hosts",
    "",
    host,
    "",
    f"- attempts at this epoch: {attempts}",
    f"- escalation rung: {rung} ({action})",
    f"- surfaces: {', '.join(sorted(surfaces)) or 'none'}",
    (f"- routing tag: `{tag}` (OWNERS.md resolves owner + fallbacks from it)"
     if action in ("owner", "fallback") else
     "- routing tag: _(none yet -- still retrying locally)_" if action == "retry" else
     f"- routing tag: `{tag}`; holding -- ladder exhausted, no human to hand to"),
    "",
]
# No receipt marker is emitted here on purpose. This body is only ever built for
# a host that is drifted or unobservable, so a marker would be a false receipt.
# The marker is posted as a follow-up comment when the host later verifies
# compliant -- see the compliant branch near the top of this script.
if unobs:
    lines += ["### Unobservable (probe failed -- NOT known-good)", ""]
    lines += [f"- `{k}`" for k in unobs]
    lines += ["", "> An unobservable key is not a passing key. This host cannot be "
              "declared converged while any key here is unreadable.", ""]
if drifted:
    lines += ["### Drifted", ""] + [f"- `{k}`" for k in drifted] + [""]
if unverified:
    lines += ["### Declared but UNVERIFIED", "",
              "These manifest values were never read off a real host. Suspect the "
              "**manifest**, not the host, before changing anything:", ""]
    lines += [f"- `{k}`" for k in unverified] + [""]
lines += ["### Evidence", "", "```json", json.dumps(verify, indent=2)[:4000], "```", "",
          "_Filed by `tools/fleet/watchdog.sh`. No human is in this escalation chain._",
          "",
          "> This issue is durable queryable state, not delivery. GitHub Actions "
          "cannot currently run in this repo (account billing), so filing this "
          "notified nobody and no tag validation ran. The watchdog advances its "
          "own escalation ladder on its own timer and never waits for a reply, "
          "so a silent issue does not stall it -- but do not read the existence "
          "of this issue as evidence that anyone has seen it."]
body = "\n".join(lines)

if dry:
    print("\n--- DRY RUN: would file/update this issue ---")
    print(f"repo:  {issue_repo}")
    print(f"title: {title}")
    print(body[:900] + ("\n...[truncated]" if len(body) > 900 else ""))
    sys.exit(10 if action in ("owner", "fallback") else (20 if action == "hold" else 0))

# Which labels are ours to set at all: `state:*` is a separate vocabulary --
# `lifecycle.py` records that it "belongs to the fleet-convergence system", i.e.
# to this watchdog -- while `owner:`, `esc:` and `ack:` belong to theirs and we
# never set them.
#
# What this writes has to be byte-identical to what `sync_labels.py` over there
# would have written -- same colour, same text. This map is only ever reached
# when a declared label is MISSING, which is exactly the state their reconcile
# pass has not covered; if the wording differs, the backstop stops being a
# backstop and becomes the thing that puts a `drifted` line in their next
# `--check`, on the recovery path, when something is already wrong.
#
# So these are not our words to choose. `sync_labels.declared()` is the
# authority: tag descriptions name the KIND and point at OWNERS.md rather than
# restating a routing decision, and `state:open` is read from the README's
# lifecycle table, which is why it is not paraphrased here. `roster_check.py`
# imports that function and asserts every pair below still matches, so their
# next wording change fails a check instead of arriving as drift we caused.
_TAG_DESC = "Infrastructure failure kind; routing lives in OWNERS.md"
LABEL_SPECS = {
    "infra-disk": ("0e8a16", _TAG_DESC),
    "infra-host-state": ("0e8a16", _TAG_DESC),
    "infra-routing": ("0e8a16", _TAG_DESC),
    "infra-fleet-drift": ("0e8a16", _TAG_DESC),
    "state:open": ("ededed", "Filed, not yet picked up"),
}


def ensure_labels(exe, repo, wanted):
    """Return the subset of `wanted` that is safe to pass to `--label`.

    A label named in OWNERS.md's allowlist is not necessarily a label that
    EXISTS in the repo. `sync_labels.py` over there materializes the whole
    declared set, but it is run by hand -- every workflow in that repo is
    killed pre-start by the account's billing state -- so the allowlist and
    the live set can drift apart between runs.

    That matters because a missing label must never cost us the issue. Filing
    is the whole point of the escalation ladder; a perfectly-good drift report
    dropped because a label lookup missed is a worse outcome than the same
    report filed with a thinner label set. So: create what is missing, and
    drop -- never fail on -- whatever could not be created.

    What gets created has to be indistinguishable from what their sync would
    have created, or this backstop becomes a drift source on the one path it
    exists to cover. `LABEL_SPECS` carries that wording and `roster_check.py`
    holds it against their `declared()`.
    """
    try:
        proc = subprocess.run([exe, "api", f"repos/{repo}/labels?per_page=100",
                               "--jq", ".[].name"],
                              check=True, capture_output=True, text=True, timeout=60)
        live = set(proc.stdout.split())
    except Exception as exc:
        # Cannot enumerate: assume nothing exists rather than guess wrong. An
        # issue with no labels still routes by hand; a failed create does not.
        print(f"label lookup failed ({exc}); filing without labels", file=sys.stderr)
        return []
    usable = []
    for name in wanted:
        if name in live:
            usable.append(name)
            continue
        colour, desc = LABEL_SPECS.get(name, ("ededed", ""))
        try:
            subprocess.run([exe, "api", f"repos/{repo}/labels", "-f", f"name={name}",
                            "-f", f"color={colour}", "-f", f"description={desc}"],
                           check=True, capture_output=True, text=True, timeout=60)
            print(f"created missing label `{name}`")
            usable.append(name)
        except Exception as exc:
            print(f"could not create label `{name}` ({exc}); filing without it",
                  file=sys.stderr)
    return usable


# --- file or update the issue -------------------------------------------
if action in ("owner", "fallback", "hold"):
    exe = shutil.which("ghapp")
    if not exe:
        print("ghapp not on PATH -- cannot escalate; leaving state for the next run",
              file=sys.stderr)
    else:
        try:
            if state.get("issue"):
                subprocess.run([exe, "issue", "comment", str(state["issue"]),
                                "--repo", issue_repo, "--body", body],
                               check=True, capture_output=True, text=True, timeout=120)
                print(f"commented on issue #{state['issue']}")
            else:
                # Exactly one `infra-*` routing tag, resolved above from which
                # kind of key drifted. One tag is not a style preference -- the
                # validator reports "more than one tag" as a problem, because
                # two tags mean two escalation chains and no unambiguous owner.
                #
                # The tag names the FAILURE KIND, never the host: a disk floor
                # is `infra-disk` on any machine. The affected machine travels
                # in the body's `### Affected hosts` field, which is also the
                # denominator their rollout gate counts receipts against.
                #
                # `state:open` and not `state:investigating`: the watchdog has
                # evidence but no per-host root cause, which is the bar for
                # leaving open. `owner:` / `esc:` are system-owned -- the router
                # reconciles them and removes anything we set, so we never do.
                labels = ensure_labels(exe, issue_repo, [tag, "state:open"])
                cmd = [exe, "issue", "create", "--repo", issue_repo,
                       "--title", title, "--body", body]
                if labels:
                    cmd += ["--label", ",".join(labels)]
                proc = subprocess.run(cmd, check=True, capture_output=True,
                                      text=True, timeout=120)
                url = proc.stdout.strip().splitlines()[-1] if proc.stdout.strip() else ""
                if url.rstrip("/").split("/")[-1].isdigit():
                    state["issue"] = int(url.rstrip("/").split("/")[-1])
                print(f"opened issue: {url}")
        except subprocess.CalledProcessError as exc:
            print(f"escalation failed: {(exc.stderr or '')[:300]}", file=sys.stderr)
        except Exception as exc:
            print(f"escalation failed: {exc}", file=sys.stderr)

state["last_run"] = now.isoformat(timespec="seconds")
os.makedirs(os.path.dirname(state_file), exist_ok=True)
with open(state_file, "w") as fh:
    json.dump(state, fh, indent=2, sort_keys=True)

sys.exit(10 if action in ("owner", "fallback") else (20 if action == "hold" else 0))
PY
RC=$?
case "$RC" in
  0|10|20) exit "$RC" ;;
  *) echo "watchdog.sh: escalation engine exited $RC" >&2; exit 3 ;;
esac
