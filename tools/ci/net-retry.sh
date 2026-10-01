#!/usr/bin/env bash
# Run one network command with bounded, spaced retries.
#
#   tools/ci/net-retry.sh python3 -m pip install --quiet 'pyyaml==6.0.2'
#   tools/ci/net-retry.sh git fetch --no-tags origin "$BASE" "$HEAD"
#
# pip and git already retry a dropped connection a few times within one call.
# What neither survives is a resolve failure or a refusal that lasts longer than
# its own short backoff, which is what a CI VM sees when its DNS or egress relay
# blips. This covers that window: up to PULP_NET_RETRY_ATTEMPTS attempts
# (default 3), sleeping PULP_NET_RETRY_DELAY_SECS x attempt between them
# (default 20, so 20 s then 40 s), the same spacing build.yml's pip and cargo
# fetch steps use.
#
# Wrap only the network command, never a whole step: a retry must not re-run a
# check whose failure is a verdict. The command's own exit status is returned
# after the last attempt, so a real failure still fails the step. Messages name
# only the program, never its arguments, so a credential in a URL is not echoed.
set -uo pipefail

if [ "$#" -eq 0 ]; then
  echo "usage: net-retry.sh <command> [args...]" >&2
  exit 2
fi

attempts="${PULP_NET_RETRY_ATTEMPTS:-3}"
delay="${PULP_NET_RETRY_DELAY_SECS:-20}"
case "$attempts" in ''|*[!0-9]*|0) echo "net-retry: PULP_NET_RETRY_ATTEMPTS must be a positive integer" >&2; exit 2 ;; esac
case "$delay" in ''|*[!0-9]*) echo "net-retry: PULP_NET_RETRY_DELAY_SECS must be a non-negative integer" >&2; exit 2 ;; esac

attempt=1
while :; do
  "$@"
  status=$?
  if [ "$status" -eq 0 ]; then
    exit 0
  fi
  if [ "$attempt" -ge "$attempts" ]; then
    echo "::error::$1 failed after $attempts attempts (exit $status)"
    exit "$status"
  fi
  wait_secs=$(( delay * attempt ))
  echo "::warning::$1 attempt $attempt of $attempts failed (exit $status); retrying in ${wait_secs}s"
  sleep "$wait_secs"
  attempt=$(( attempt + 1 ))
done
