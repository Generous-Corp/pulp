#!/usr/bin/env bash
# Install apt packages, refreshing the package index and retrying when a fetch fails.
#
#   sudo tools/ci/apt-install-retry.sh patchelf doxygen
#
# A mirror that publishes a security update while a job runs leaves the index
# fetched by `apt-get update` pointing at .debs that now 404. The next update
# fixes it, so a failed install is followed by `apt-get update` and another
# attempt: up to PULP_APT_RETRY_ATTEMPTS attempts (default 3), sleeping
# PULP_APT_RETRY_DELAY_SECS x attempt between them (default 5).
#
# The first attempt runs `apt-get update` too, so callers need no separate
# update. The final exit status is the last install's, so a real failure (an
# unknown package, a dead mirror) still fails the step. Set
# PULP_APT_RETRY_OPTS to extra apt-get install options (for example
# --no-install-recommends).
set -euo pipefail

if [ "$#" -eq 0 ]; then
  echo "usage: apt-install-retry.sh <package> [package...]" >&2
  exit 2
fi

attempts="${PULP_APT_RETRY_ATTEMPTS:-3}"
delay="${PULP_APT_RETRY_DELAY_SECS:-5}"
case "$attempts" in ''|*[!0-9]*|0) echo "apt-install-retry: PULP_APT_RETRY_ATTEMPTS must be a positive integer" >&2; exit 2 ;; esac
case "$delay" in ''|*[!0-9]*) echo "apt-install-retry: PULP_APT_RETRY_DELAY_SECS must be a non-negative integer" >&2; exit 2 ;; esac

# shellcheck disable=SC2206
opts=(${PULP_APT_RETRY_OPTS:-})

attempt=1
while :; do
  status=0
  apt-get update || status=$?
  if [ "$status" -eq 0 ]; then
    DEBIAN_FRONTEND=noninteractive apt-get install -y --fix-missing ${opts[@]+"${opts[@]}"} "$@" || status=$?
  fi
  if [ "$status" -eq 0 ]; then
    exit 0
  fi
  if [ "$attempt" -ge "$attempts" ]; then
    echo "::error::apt install of $* failed after $attempts attempts (exit $status)" >&2
    exit "$status"
  fi
  echo "apt-install-retry: attempt $attempt/$attempts failed (exit $status); refreshing index and retrying" >&2
  sleep $((delay * attempt))
  attempt=$((attempt + 1))
done
