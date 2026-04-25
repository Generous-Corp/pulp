#!/usr/bin/env bash
# Stub pulp-cpp used by the sandbox harness to verify the Rust binary's
# fallthrough delegation (see experimental/pulp-rs/src/main.rs commit
# 4ba25715). The stub prints its argv verbatim, tags the payload with
# "STUB_PULP_CPP" so tests can assert they reached it (not the real
# C++ binary), and exits with a deterministic non-zero code so the
# Rust binary's exit-code propagation can be verified.
#
# The real C++ binary prints "Build directory not found" and exits
# non-zero when invoked outside a project; the stub is deliberately
# distinguishable from that output.
set -u
printf 'STUB_PULP_CPP argv:'
for arg in "$@"; do
    printf ' %s' "$arg"
done
printf '\n'
# Exit 42 so it's trivially distinguishable from 0 / 1 / 2.
exit 42
