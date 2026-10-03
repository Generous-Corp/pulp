#!/usr/bin/env bash
# Install the hash-pinned visual-analysis Python set into the interpreter a
# configured build's ctest will launch.
#
#   tools/ci/install_visual_python_deps.sh [BUILD_DIR]   (default: $PULP_BUILD_DIR)
#
# Every workflow job that runs ctest calls this after configuring, so the
# non-skippable visual-python-deps-present check is answerable wherever it is
# registered rather than red on the jobs the install misses.
set -euo pipefail
PULP_BUILD_DIR="${1:-${PULP_BUILD_DIR:?pass the build directory or set PULP_BUILD_DIR}}"
# Seven ctest registrations import numpy, Pillow or scikit-image and
# skip themselves when one of those is missing. A ctest SKIP reads
# exactly like a PASS, so a lane that quietly lost a wheel reports
# green without running the checks it was built for. Installing the
# declared set on every platform that runs ctest keeps the
# non-skippable visual-python-deps-present check answerable
# everywhere, rather than red on the platforms the install misses.
#
# The interpreter must be the one CMake configured, not whatever this
# shell resolves: ctest invokes Python3_EXECUTABLE, so installing into
# a different interpreter would leave every one of those tests
# skipping while this step reported success.
#
# find_package(Python3 COMPONENTS Interpreter) records the interpreter
# it resolved in the INTERNAL cache entry _Python3_EXECUTABLE and
# leaves the public Python3_EXECUTABLE a normal variable, so the
# public name reaches the cache only when a configure passes
# -DPython3_EXECUTABLE. The local Shipyard lane passes it; this
# workflow does not. Read the internal entry first — it holds the
# value the public variable expanded to when test/cmake registered
# these tests, which is exactly what ctest launches — and fall back to
# the public entry for a configure that pinned one explicitly.
cache="$PULP_BUILD_DIR/CMakeCache.txt"
py="$(sed -n 's/^_Python3_EXECUTABLE:[^=]*=//p' "$cache" | head -1)"
if [ -z "$py" ]; then
  py="$(sed -n 's/^Python3_EXECUTABLE:[^=]*=//p' "$cache" | head -1)"
fi
if [ -z "$py" ]; then
  echo "No Python3 interpreter is recorded in $cache" >&2
  echo "(looked for _Python3_EXECUTABLE, then Python3_EXECUTABLE)." >&2
  echo "The visual-analysis tests resolve their interpreter from that cache entry," >&2
  echo "so installing into any other interpreter would leave them skipping." >&2
  exit 1
fi
# Satisfied already? Then do not touch the network. `--upgrade` asks the
# index for a newer wheel even when the requirement is met, so it turned
# a reachable-PyPI assumption into a precondition of the REQUIRED macos
# gate: on 2026-09-23 an ephemeral VM whose egress refused CONNECT to
# pypi.org ("Tunnel connection failed: 403 Forbidden") failed this step
# at 21 of 41 and killed every merge_group batch that landed on that
# host, while the same batch passed on a host whose VMs could reach it.
# A required gate must not depend on a third-party service being
# reachable, and the declared set is a floor, not a latest-wins pin.
if "$py" -m pip install --dry-run --no-index --quiet \
     -r tools/motion/visual/requirements.txt >/dev/null 2>&1; then
  echo "visual-analysis dependencies already satisfied in $py; no index fetch"
else
  # Install the hash-pinned resolution, not the ranges: the same bytes
  # on every run, so a new upstream release cannot change a gate verdict
  # and a tampered or truncated download fails the hash check.
  lock=tools/motion/visual/requirements.lock
  echo "Installing the pinned visual-analysis set ($lock) into $py"
  # PEP 668 hosts (Homebrew, Debian) refuse a plain --user install and
  # mark themselves with EXTERNALLY-MANAGED beside the stdlib. Ask once
  # rather than retrying every install, so a network failure is not
  # paid twice.
  pep668=()
  if "$py" -c 'import os, sys, sysconfig; sys.exit(0 if os.path.exists(os.path.join(sysconfig.get_path("stdlib"), "EXTERNALLY-MANAGED")) else 1)'; then
    pep668=(--break-system-packages)
  fi
  pip_user() {
    "$py" -m pip install ${pep668[@]+"${pep668[@]}"} --user --require-hashes -r "$lock" "$@"
  }
  # A tartci gate guest may carry a read-only host wheelhouse. Installing
  # from it needs no network at all, so the egress relay and PyPI stop
  # being able to fail the required gate. It is an accelerator, not an
  # authority: the hashes above still decide what is acceptable.
  wheelhouse="${TARTCI_PIP_WHEELHOUSE:-}"
  if [ -n "$wheelhouse" ] && [ -d "$wheelhouse" ]; then
    if pip_user --no-index --find-links "$wheelhouse"; then
      echo "Installed from the host wheelhouse $wheelhouse"
      exit 0
    fi
    echo "::warning::host wheelhouse $wheelhouse could not satisfy $lock; falling back to the package index"
  fi
  # pip already retries a dropped connection; these attempts cover a
  # refusal or an outage that outlasts pip's own short backoff.
  delay="${PULP_PIP_RETRY_DELAY_SECS:-20}"
  for attempt in 1 2 3; do
    if pip_user; then
      exit 0
    fi
    if [ "$attempt" -lt 3 ]; then
      echo "::warning::pip install attempt $attempt failed; retrying in $(( delay * attempt ))s"
      sleep $(( delay * attempt ))
    fi
  done
  echo "::error::could not install $lock after 3 attempts; the visual-analysis ctests would skip" >&2
  exit 1
fi
# Whether the set was pre-provisioned or installed here, the
# non-skippable `visual-python-deps-present` ctest is what proves it.
# This step only has to stop being a network-dependent tripwire.
