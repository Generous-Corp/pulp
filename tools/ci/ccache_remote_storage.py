#!/usr/bin/env python3
"""Point ccache at the runner host's remote storage, read-write only for trusted events.

A self-hosted runner whose host serves a shared ccache sets
PULP_CCACHE_REMOTE_STORAGE in the job environment (for example
`http://10.0.0.2:8080/pulp-windows-x64`). The VM itself keeps nothing between
jobs, so that storage is the only place a compile result can outlive one.

Writes are restricted because a cache shared with untrusted code is a
poisoning path: an object a pull request's build stores would be served to
every later build that hashes the same inputs. Only code already on `main`
writes:

- `push` and `schedule` on `refs/heads/main`;
- `workflow_dispatch` on `refs/heads/main` (operator-run against main);
- `merge_group` (the queue builds main plus already-reviewed entries).

Everything else (pull requests, dispatches on other branches) gets the same
storage with ccache's `|read-only` attribute: it may hit, never store.

With PULP_CCACHE_REMOTE_STORAGE unset or empty this prints that the storage is
not configured and writes nothing, so a runner without one behaves exactly as
before.

Usage (in a workflow step):
    python3 tools/ci/ccache_remote_storage.py --github-env "$GITHUB_ENV"

Exit 0 on success (including "not configured"); 2 when the configured value
is unusable, so a malformed URL fails the step rather than silently writing.
"""
from __future__ import annotations

import argparse
import os
import re
import sys
from pathlib import Path

TRUSTED_REF = "refs/heads/main"
# ccache's remote storage URL. Attributes after `|` are ccache's own syntax;
# an operator may supply some (for example `|connect-timeout=500`) but must
# not supply `read-only`, which this script owns.
_URL = re.compile(r"^(http|https|redis|file)://[^\s|]+(\|[A-Za-z0-9_.=-]+)*$")


def writes_allowed(event: str, ref: str) -> bool:
    """Whether a job for `event` on `ref` may store objects in the shared cache."""
    if event == "merge_group":
        return True
    if event in ("push", "schedule", "workflow_dispatch"):
        return ref == TRUSTED_REF
    return False


def remote_storage_setting(url: str, event: str, ref: str) -> str:
    """The CCACHE_REMOTE_STORAGE value for this job (read-only unless trusted)."""
    if not _URL.match(url):
        raise ValueError(f"PULP_CCACHE_REMOTE_STORAGE is not a usable ccache URL: {url!r}")
    attributes = url.split("|")[1:]
    if any(a.split("=", 1)[0] == "read-only" for a in attributes):
        raise ValueError("PULP_CCACHE_REMOTE_STORAGE must not carry |read-only; "
                         "this script decides it from the event")
    return url if writes_allowed(event, ref) else f"{url}|read-only"


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--github-env", required=True,
                        help="the $GITHUB_ENV file to append CCACHE_REMOTE_STORAGE to")
    args = parser.parse_args(argv)

    url = os.environ.get("PULP_CCACHE_REMOTE_STORAGE", "").strip()
    if not url:
        print("ccache remote storage: not configured (PULP_CCACHE_REMOTE_STORAGE unset)")
        return 0
    event = os.environ.get("GITHUB_EVENT_NAME", "")
    ref = os.environ.get("GITHUB_REF", "")
    try:
        setting = remote_storage_setting(url, event, ref)
    except ValueError as error:
        print(f"::error::{error}", file=sys.stderr)
        return 2
    mode = "read-only" if setting.endswith("|read-only") else "read-write"
    with Path(args.github_env).open("a", encoding="utf-8") as env_file:
        env_file.write(f"CCACHE_REMOTE_STORAGE={setting}\n")
    print(f"ccache remote storage: {mode} (event={event or '?'} ref={ref or '?'})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
