#!/usr/bin/env python3
"""Download a URL to a file, riding out short GitHub / CDN outages.

One shared retry policy for every pinned-asset fetch in CI (Skia and V8
release zips, the wasm toolchain archives):

* up to 6 attempts, exponential backoff 5s, 10s, 20s, 40s, 60s (capped), each
  scaled by a random 0.75-1.25 jitter, so roughly 2-3 minutes in total;
* retry only on HTTP 408/425/429/500/502/503/504, timeouts, connection
  resets and truncated bodies; a numeric ``Retry-After`` overrides the
  schedule (capped);
* fail at once on every other status (a 403/404 is a bad pin or a revoked
  asset) and on a full disk, which retrying cannot fix.

This module verifies nothing about the bytes. Callers keep their own sha256
check, which stays the authority on integrity.

CLI: ``python3 tools/scripts/download_retry.py URL DEST``. The body is written
to ``DEST.part`` and renamed only once complete.
"""

from __future__ import annotations

import http.client
import random
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

RETRYABLE_HTTP_STATUS = frozenset({408, 425, 429, 500, 502, 503, 504})

# A bare OSError is deliberately NOT transient: it is what a full disk raises
# on the write side, and re-downloading hundreds of MiB to fill the same disk
# turns a clear error into a slow one.
TRANSIENT_ERRORS = (
    urllib.error.URLError,
    TimeoutError,
    ConnectionError,
    http.client.IncompleteRead,
)

DEFAULT_ATTEMPTS = 6
BACKOFF_BASE_SECONDS = 5.0
BACKOFF_CAP_SECONDS = 60.0
JITTER_RANGE = (0.75, 1.25)


def jitter_factor() -> float:
    """Multiplier applied to each nominal delay (patched in tests)."""
    return random.uniform(*JITTER_RANGE)


def backoff_seconds(attempt: int) -> float:
    """Delay after failed `attempt` (1-based): exponential, capped, jittered."""
    nominal = min(BACKOFF_BASE_SECONDS * (2 ** (attempt - 1)), BACKOFF_CAP_SECONDS)
    return nominal * jitter_factor()


def retry_after_seconds(error: BaseException, fallback: float) -> float:
    """Honour a numeric Retry-After header when the server sends one."""
    headers = getattr(error, "headers", None)
    if headers is None:
        return fallback
    raw = headers.get("Retry-After")
    if raw is None:
        return fallback
    try:
        seconds = float(str(raw).strip())
    except ValueError:
        # The header also permits an HTTP-date; use our own schedule instead.
        return fallback
    if seconds < 0:
        return fallback
    return min(seconds, BACKOFF_CAP_SECONDS)


def download(url: str, dest: Path, *, attempts: int = DEFAULT_ATTEMPTS,
             sleep=time.sleep) -> None:
    """Download `url` into `dest`, retrying transient failures.

    Each attempt reopens `dest` for writing, so a partial body is truncated
    rather than concatenated with the next one.
    """
    last_error: BaseException | None = None
    for attempt in range(1, attempts + 1):
        try:
            with urllib.request.urlopen(url) as resp, Path(dest).open("wb") as fp:
                while True:
                    chunk = resp.read(1024 * 1024)
                    if not chunk:
                        break
                    fp.write(chunk)
            return
        except urllib.error.HTTPError as err:
            if err.code not in RETRYABLE_HTTP_STATUS:
                raise
            last_error = err
            delay = retry_after_seconds(err, backoff_seconds(attempt))
        except TRANSIENT_ERRORS as err:
            last_error = err
            delay = backoff_seconds(attempt)

        if attempt == attempts:
            break
        print(
            f"  transient download failure ({last_error}); retrying in "
            f"{delay:.0f}s [attempt {attempt + 1} of {attempts}]",
            flush=True,
        )
        sleep(delay)

    assert last_error is not None
    print(f"  download failed after {attempts} attempts: {last_error}", flush=True)
    raise last_error


def read_bytes(url: str, *, attempts: int = DEFAULT_ATTEMPTS,
               sleep=time.sleep) -> bytes:
    """Fetch a small `url` into memory with the same policy as `download`."""
    import tempfile

    with tempfile.TemporaryDirectory() as td:
        part = Path(td) / "body"
        download(url, part, attempts=attempts, sleep=sleep)
        return part.read_bytes()


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print("usage: download_retry.py URL DEST", file=sys.stderr)
        return 2
    url, dest = argv[1], Path(argv[2])
    part = dest.with_name(dest.name + ".part")
    try:
        download(url, part)
        part.replace(dest)
    except (urllib.error.URLError, OSError, http.client.HTTPException) as err:
        print(f"download_retry: {url}: {err}", file=sys.stderr)
        part.unlink(missing_ok=True)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
