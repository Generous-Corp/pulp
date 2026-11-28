#!/usr/bin/env python3
"""Review items: the exceptions a guard leaves for the standing reviewer.

Two guards used to fail until the author wrote its own exception into a commit:
`hotspot_edit_guard.py` (`Serialized-Hotspot:`) and `header_fanout_guard.py`
(`Fanout-Grow:`). They exist so that a serialized hotspot regeneration or a
header's fan-out growth is visible in review. Here the exception is the
reviewer's instead: each guard reports a stable review item id, and the
standing reviewer's exact-head verdict must accept every id. Ids:

  hotspot:<registered path>            a serialized hotspot was changed
  fanout:<header path>:+<added TUs>    a tracked header grew past its ceiling
  ledger:<ledger path>                 the branch changed a guard's ledger

Both ledgers are read from the base ref, never from the working tree or the
pull request. Anything the guards refuse outright (a fragment-mode hotspot
edit, a tracked header missing at the head) is a hard failure: `check`
refuses the head whatever the verdict says.

The reviewer accepts an item with one line per id in its verdict comment,
above the final `verdict: APPROVE head:<sha>` line:

  accept-review-item: <id>

Only the newest qualifying APPROVE comment naming the exact head counts. A
comment qualifies when all of these hold, read from the base branch's
`.shipyard/config.toml` `[auto_merge]` table (fail closed when a key is
missing): it is by the fleet App (`shipyard-local[bot]`, a Bot account); its
last non-blank line is `verdict: APPROVE head:<head>`; it has a canonical
`ghapp` session stamp, no stamp-like variant, and the stamp's session matches
one of `reviewer_stamp_sessions`; a `reviewer:` line names one of
`reviewer_signers`; and it was never edited. Lines inside fenced code blocks
or `>` quotes are not acceptances. These are the rules Shipyard's
`review-verdict` uses to count an App APPROVE, so an acceptance is exactly as
hard to forge as the approval. An item a `Serialized-Hotspot:` or
`Fanout-Grow:` commit line on the branch already accepts needs nothing more.

  review_items.py items --base REF --head REV [--root DIR] [--json PATH]
      List the head's review items, as a reviewer runs it before posting a
      verdict. Each `accept-review-item:` line it prints is pasted as is.

  review_items.py check --repo O/R --pr N --head SHA --base REF [--root DIR]
                        [--comments-json FILE] [--post-status]
                        [--posted-marker FILE] [--target-url URL]
      Refuse the head unless the verdict accepts every review item. The
      `review-verdict` workflow runs it from the base branch after Shipyard's
      rule has allowed the head; with `--post-status` it posts a `failure`
      `review-verdict` status on the head, so Shipyard's `success` status
      cannot satisfy the required context while an item is unaccepted.

Exit codes: 0 every item accepted (or none); 1 an item is unaccepted or the
guards refuse the head outright; 2 anything could not be read or posted. The
workflow decides what each means for its job.
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import tempfile
import tomllib
from pathlib import Path
from typing import Any, Callable

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import header_fanout_guard  # noqa: E402
import hotspot_edit_guard  # noqa: E402

CONTEXT = "review-verdict"
DESCRIPTION_LIMIT = 140
FLEET_APP_LOGIN = "shipyard-local[bot]"
UNATTRIBUTED = "unattributed"

ACCEPT_LINE = re.compile(r"^[ \t]*accept-review-item:[ \t]*(\S+)[ \t]*$")
APPROVE_MARKER = re.compile(r"^[ \t]*verdict:[ \t]*APPROVE[ \t]+head:[ \t]*([0-9a-f]{40})[ \t]*$")
# The stamp `ghapp` writes, and anything that only looks like one; same
# patterns as Shipyard's queue-arm guard.
SESSION_STAMP = re.compile(r"<!-- session: ([!-.0-~]+)/([!-~]+?)(?: agent: ([!-~]+?))? -->")
STAMP_VARIANT = re.compile(r"<!--\s*session\s*:", re.IGNORECASE)
SIGNER_LINE = re.compile(r"^[ \t]*reviewer:[ \t]*([A-Za-z0-9._-]+)[ \t]*$", re.MULTILINE)
FENCE = re.compile(r"^[ \t]*(```|~~~)")


class ReviewItemError(RuntimeError):
    """Something the decision needs could not be read."""


# ---------------------------------------------------------------------------
# Items
# ---------------------------------------------------------------------------


# The ledgers the trusted check reads from the default branch. A pull request
# that changes one carries the review item `ledger:<path>`: the change decides
# what the guards measure, so a reviewer must see it.
LEDGERS = ("tools/scripts/hotspot_edit_guard.json", "tools/scripts/header_fanout_guard.json")
FANOUT_LEDGER = "tools/scripts/header_fanout_guard.json"


def _git(root: Path, *args: str) -> str:
    proc = subprocess.run(["git", "-C", str(root), *args], capture_output=True, text=True,
                          encoding="utf-8")
    if proc.returncode != 0:
        raise ReviewItemError(f"git {' '.join(args)}: {proc.stderr.strip()}")
    return proc.stdout


def ledger_items(root: Path, base: str, head: str) -> list[dict[str, Any]]:
    """One review item per guard ledger the range changes."""
    anchor = _git(root, "merge-base", base, head).strip()
    changed = set(_git(root, "diff", "--name-only", "--no-renames", anchor, head, "--",
                       *LEDGERS).splitlines())
    return [{"id": f"ledger:{path}", "kind": "guard-ledger", "path": path, "accepted_by": None}
            for path in LEDGERS if path in changed]


def head_ledger_headers(root: Path, head: str) -> set[str] | None:
    """The tracked header paths in the head's own fan-out ledger, or None when
    the head has no readable ledger."""
    proc = subprocess.run(["git", "-C", str(root), "show", f"{head}:{FANOUT_LEDGER}"],
                          capture_output=True, text=True, encoding="utf-8")
    if proc.returncode != 0:
        return None
    try:
        return {h.path for h in header_fanout_guard.parse_config(json.loads(proc.stdout))}
    except (ValueError, json.JSONDecodeError):
        return None


def base_ledgers(root: Path, base: str, into: Path) -> dict[str, Path]:
    """Both ledgers as the base ref holds them, written under `into`. Never the
    working tree: a checkout of the pull request would hold its own copies."""
    out: dict[str, Path] = {}
    for path in LEDGERS:
        target = into / Path(path).name
        target.write_text(_git(root, "show", f"{base}:{path}"), encoding="utf-8")
        out[path] = target
    return out


def collect_items(root: Path, base: str, head: str) -> tuple[list[dict[str, Any]], list[str]]:
    """(review items, hard failures) for `merge-base(base, head)..head`, with
    both ledgers read from the base ref.

    A hard failure is anything the guards refuse outright: a fragment-mode
    hotspot edit, or a tracked header missing at the head. No verdict accepts
    one. A header the head removes together with its ledger entry is not a
    failure: the ledger change is a review item instead."""
    try:
        with tempfile.TemporaryDirectory(prefix="review-items-") as tmp:
            ledgers_at_base = base_ledgers(root, base, Path(tmp))
            hotspot_items, refusals = hotspot_edit_guard.review_items_for_range(
                root, base, head, ledgers_at_base[LEDGERS[0]])
            fanout_items, fanout_failures = header_fanout_guard.review_items_for_range(
                root, base, head, ledgers_at_base[FANOUT_LEDGER])
            missing = header_fanout_guard.missing_tracked_headers(
                root, head, ledgers_at_base[FANOUT_LEDGER])
    except (OSError, ValueError, RuntimeError) as exc:
        raise ReviewItemError(f"cannot compute review items: {exc}") from exc
    ledgers = ledger_items(root, base, head)
    still_tracked = head_ledger_headers(root, head)
    dropped = {path for path in missing
               if still_tracked is not None and path not in still_tracked
               and any(item["path"] == FANOUT_LEDGER for item in ledgers)}
    failures = [f"hotspot edit refused: {spot['path']}" for spot, _ in refusals]
    failures += [f for f in fanout_failures if f.split(":", 1)[0] not in dropped]
    return hotspot_items + fanout_items + ledgers, failures


def pending(items: list[dict[str, Any]]) -> list[str]:
    """Ids no commit trailer accepts."""
    return [item["id"] for item in items if item.get("accepted_by") is None]


# ---------------------------------------------------------------------------
# Verdicts
# ---------------------------------------------------------------------------


def reviewer_policy(config_text: str) -> dict[str, list[str]]:
    """`{signers, sessions}` from the base config's `[auto_merge]` table."""
    try:
        section = tomllib.loads(config_text).get("auto_merge") or {}
    except tomllib.TOMLDecodeError as exc:
        raise ReviewItemError(f"the base .shipyard/config.toml is unreadable: {exc}") from exc
    policy: dict[str, list[str]] = {}
    for key, name in (("reviewer_signers", "signers"), ("reviewer_stamp_sessions", "sessions")):
        values = section.get(key)
        if (not isinstance(values, list) or not values
                or not all(isinstance(v, str) and v.strip() for v in values)):
            raise ReviewItemError(f"the base .shipyard/config.toml has no usable [auto_merge] {key}; "
                                  "no verdict can accept a review item")
        policy[name] = [v.strip().lower() for v in values]
    return policy


def glob_match(pattern: str, text: str) -> bool:
    """`*` any run within one `/` segment, `?` one character, `**` whole
    segments; the same matching Shipyard applies to stamp sessions."""
    def segment(pat: str, txt: str) -> bool:
        if not pat:
            return not txt
        if pat[0] == "*":
            return any(segment(pat[1:], txt[i:]) for i in range(len(txt) + 1))
        return bool(txt) and pat[0] in ("?", txt[0]) and segment(pat[1:], txt[1:])

    def segments(pats: list[str], parts: list[str]) -> bool:
        if not pats:
            return not parts
        if pats[0] == "**":
            return any(segments(pats[1:], parts[i:]) for i in range(len(parts) + 1))
        return bool(parts) and segment(pats[0], parts[0]) and segments(pats[1:], parts[1:])

    return segments(pattern.split("/"), text.split("/"))


def approve_head(body: str) -> str | None:
    """The head a comment's last non-blank line approves, or None."""
    lines = [line for line in body.splitlines() if line.strip()]
    match = APPROVE_MARKER.match(lines[-1]) if lines else None
    return match.group(1) if match else None


def disqualification(comment: dict[str, Any], head: str, policy: dict[str, list[str]]) -> str | None:
    """Why `comment` cannot accept review items for `head`, or None when it can."""
    user = comment.get("user") or {}
    body = comment.get("body") if isinstance(comment.get("body"), str) else ""
    if user.get("login") != FLEET_APP_LOGIN or user.get("type") != "Bot":
        return f"posted by {user.get('login')!r}, not the fleet App {FLEET_APP_LOGIN}"
    if approve_head(body) != head.lower():
        return "its last line is not `verdict: APPROVE head:<this head>`"
    if STAMP_VARIANT.search(SESSION_STAMP.sub("", body)):
        return "it carries a non-canonical session stamp"
    stamps = SESSION_STAMP.findall(body)
    if not stamps:
        return "it has no ghapp session stamp"
    session = stamps[-1][1]
    if session == UNATTRIBUTED:
        return "its session stamp is unattributed"
    if not any(glob_match(pattern, session.lower()) for pattern in policy["sessions"]):
        return f"its stamp session {session} is not a reviewer session"
    signed = [name.lower() for name in SIGNER_LINE.findall(body)]
    if not any(name in policy["signers"] for name in signed):
        return "it has no `reviewer:` line naming a listed signer"
    if comment.get("updated_at") not in (None, comment.get("created_at")):
        return "it was edited after it was posted; post a new verdict instead"
    return None


def accepted_ids(body: str) -> set[str]:
    """Ids the body accepts, outside fenced code blocks. A `>` quote never
    matches, since the line must start with the key."""
    found: set[str] = set()
    fenced = False
    for line in body.splitlines():
        if FENCE.match(line):
            fenced = not fenced
            continue
        if fenced:
            continue
        match = ACCEPT_LINE.match(line)
        if match:
            found.add(match.group(1))
    return found


def deciding_verdict(comments: list[dict[str, Any]], head: str, policy: dict[str, list[str]]
                     ) -> tuple[dict[str, Any] | None, list[str]]:
    """(the newest qualifying APPROVE for `head`, why each other APPROVE of it
    did not qualify)."""
    qualifying: list[dict[str, Any]] = []
    rejected: list[str] = []
    for comment in comments:
        body = comment.get("body") if isinstance(comment.get("body"), str) else ""
        if approve_head(body) != head.lower():
            continue
        why = disqualification(comment, head, policy)
        if why is None:
            qualifying.append(comment)
        else:
            rejected.append(f"comment {comment.get('id')}: {why}")
    if not qualifying:
        return None, rejected
    newest = max(qualifying, key=lambda c: (str(c.get("created_at") or ""), int(c.get("id") or 0)))
    return newest, rejected


def unaccepted(items: list[dict[str, Any]], comments: list[dict[str, Any]], head: str,
               policy: dict[str, list[str]]) -> tuple[list[str], str]:
    """(ids still unaccepted, an explanation)."""
    needed = pending(items)
    if not needed:
        return [], "every review item is accepted by a commit trailer" if items else "no review items"
    verdict, rejected = deciding_verdict(comments, head, policy)
    if verdict is None:
        why = "; ".join(rejected) if rejected else "no APPROVE names this head"
        return needed, f"no qualifying reviewer verdict names head {head[:12]} ({why})"
    accepted = accepted_ids(verdict.get("body") or "")
    missing = [item for item in needed if item not in accepted]
    return missing, f"verdict comment {verdict.get('id')} decides head {head[:12]}"


# ---------------------------------------------------------------------------
# GitHub
# ---------------------------------------------------------------------------


def gh_json(args: list[str]) -> Any:
    """`gh api` output; `--paginate` pages concatenated into one list."""
    proc = subprocess.run(["gh", "api", *args], capture_output=True, text=True, encoding="utf-8")
    if proc.returncode != 0:
        raise ReviewItemError(f"gh api {' '.join(args)}: {proc.stderr.strip()}")
    decoder = json.JSONDecoder()
    text, index, pages = proc.stdout, 0, []
    while index < len(text):
        if text[index].isspace():
            index += 1
            continue
        page, index = decoder.raw_decode(text, index)
        pages.append(page)
    if len(pages) == 1:
        return pages[0]
    if all(isinstance(page, list) for page in pages):
        return [entry for page in pages for entry in page]
    raise ReviewItemError(f"gh api {' '.join(args)}: unexpected output")


def describe_status(missing: list[str], prefix: str = "unaccepted review items: ") -> str:
    text = prefix + ", ".join(missing)
    return text if len(text) <= DESCRIPTION_LIMIT else text[: DESCRIPTION_LIMIT - 1] + "…"


# Set by main(): a file written once a failure status is confirmed posted, so
# the workflow's fallback knows whether it still has to refuse the head.
POSTED_MARKER: Path | None = None


def post_failure(repo: str, head: str, description: str, target_url: str | None,
                 api: Callable[[list[str]], Any] | None = None) -> None:
    api = api or gh_json
    args = ["-X", "POST", f"repos/{repo}/statuses/{head}", "-f", "state=failure",
            "-f", f"context={CONTEXT}", "-f", f"description={description}"]
    if target_url:
        args += ["-f", f"target_url={target_url}"]
    api(args)
    if POSTED_MARKER is not None:
        POSTED_MARKER.write_text(f"{head}\n", encoding="utf-8")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def cmd_items(a: argparse.Namespace) -> int:
    root = Path(a.root).resolve()
    try:
        items, failures = collect_items(root, a.base, a.head)
    except ReviewItemError as exc:
        print(f"review-items: ERROR: {exc}", file=sys.stderr)
        return 2
    if a.json:
        Path(a.json).write_text(json.dumps({"schema": "pulp-review-items/v1", "items": items},
                                           indent=1, sort_keys=True) + "\n", encoding="utf-8")
    for failure in failures:
        print(f"review-items: hard failure, no verdict can accept it: {failure}")
    if not items:
        print(f"review-items: none for {a.head} against {a.base}")
    # Each acceptance line is printed bare, so it can be pasted into a verdict
    # as is; what it is goes on the line before it.
    for item in items:
        if item["accepted_by"]:
            print(f"review-items: {item['id']} ({item['kind']}) is accepted by a commit trailer")
            continue
        print(f"review-items: {item['kind']}; review it, then paste the next line into the verdict:")
        print(f"accept-review-item: {item['id']}")
    return 1 if failures else 0


def cmd_check(a: argparse.Namespace) -> int:
    root = Path(a.root).resolve()
    head = a.head.lower()

    def fail_closed(message: str) -> int:
        print(f"review-items: ERROR: {message}", file=sys.stderr)
        if a.post_status:
            try:
                post_failure(a.repo, head, describe_status(["could not be judged"]), a.target_url)
            except ReviewItemError as exc:
                print(f"review-items: ERROR: {exc}", file=sys.stderr)
        return 2

    def refuse(what: list[str], why: str) -> int:
        print(f"review-items: head {head[:12]}: {why}: {', '.join(what)}")
        if a.post_status:
            try:
                post_failure(a.repo, head, describe_status(what, "refused by the guards: "),
                             a.target_url)
            except ReviewItemError as exc:
                print(f"review-items: ERROR: {exc}", file=sys.stderr)
                return 2
            print(f"review-items: posted a failure {CONTEXT} status on {head}")
        return 1

    if not re.fullmatch(r"[0-9a-f]{40}", head):
        return fail_closed(f"--head must be a full commit sha, not {a.head!r}")
    try:
        items, failures = collect_items(root, a.base, head)
        if failures:
            return refuse(failures, "the guards refuse this head outright; no verdict accepts it")
        needed = pending(items)
        if not needed:
            print(f"review-items: head {head[:12]}: "
                  + ("every review item is accepted by a commit trailer" if items else "no review items"))
            return 0
        config = Path(a.config) if a.config else root / ".shipyard/config.toml"
        policy = reviewer_policy(config.read_text(encoding="utf-8"))
        if a.comments_json:
            comments = json.loads(Path(a.comments_json).read_text(encoding="utf-8"))
        else:
            pull = gh_json([f"repos/{a.repo}/pulls/{a.pr}"])
            live = str(((pull or {}).get("head") or {}).get("sha") or "").lower()
            if live != head:
                raise ReviewItemError(f"PR #{a.pr}'s head is {live[:12] or 'unreadable'}, not {head[:12]}")
            comments = gh_json(["--paginate", f"repos/{a.repo}/issues/{a.pr}/comments?per_page=100"])
        if not isinstance(comments, list):
            raise ReviewItemError("the pull request's comments are unreadable")
        missing, why = unaccepted(items, comments, head, policy)
    except (OSError, ValueError, ReviewItemError) as exc:
        return fail_closed(str(exc))
    if not missing:
        print(f"review-items: head {head[:12]}: every review item is accepted ({why}): "
              + ", ".join(needed))
        return 0
    print(f"review-items: head {head[:12]}: {why}; unaccepted: {', '.join(missing)}")
    print("review-items: the standing reviewer accepts each one with a line "
          "`accept-review-item: <id>` in a new exact-head verdict; a push cannot clear this.")
    if a.post_status:
        try:
            post_failure(a.repo, head, describe_status(missing), a.target_url)
        except ReviewItemError as exc:
            print(f"review-items: ERROR: {exc}", file=sys.stderr)
            return 2
        print(f"review-items: posted a failure {CONTEXT} status on {head}")
    return 1


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(allow_abbrev=False, description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="command", required=True)
    for name in ("items", "check"):
        p = sub.add_parser(name, allow_abbrev=False)
        p.add_argument("--root", default=str(HERE.parents[1]),
                       help="checkout whose ledgers are read (the base branch)")
        p.add_argument("--base", default="origin/main")
        p.add_argument("--head", required=name == "check", default="HEAD")
    sub.choices["items"].add_argument("--json", default=None)
    check = sub.choices["check"]
    check.add_argument("--repo", required=True)
    check.add_argument("--pr", required=True, type=int)
    check.add_argument("--config", default=None, help="default <root>/.shipyard/config.toml")
    check.add_argument("--comments-json", default=None,
                       help="read the pull request's comments from this file instead of the API")
    check.add_argument("--post-status", action="store_true")
    check.add_argument("--target-url", default=None)
    check.add_argument("--posted-marker", default=None,
                       help="write this file once a failure status is confirmed posted")
    a = ap.parse_args(argv[1:])
    global POSTED_MARKER
    POSTED_MARKER = Path(a.posted_marker) if getattr(a, "posted_marker", None) else None
    return cmd_items(a) if a.command == "items" else cmd_check(a)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
