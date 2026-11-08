#!/usr/bin/env bash
# Run a command with no network, so anything it tries to download fails
# instead of silently succeeding.
#
#   tools/ci/run_without_network.sh <command> [args...]
#   tools/ci/run_without_network.sh --probe
#
# Linux: a fresh network namespace (`unshare --net`, through sudo), then the
# command runs as the calling user with its environment. The namespace has
# only a down loopback, so name lookups and connections fail.
# macOS: `sandbox-exec` with outbound IP and unix-socket connections denied
# (name lookups go through a unix socket to mDNSResponder, so they fail too).
#
# --probe is the control: it runs `curl` against Pulp's own repository URL
# inside the same wrapper and exits 0 only if that FAILS, and checks the same
# curl succeeds outside it (any HTTP answer counts; nothing is kept). A
# wrapper that does not actually block the network, or a host with no network
# to begin with, therefore fails the probe rather than passing every check
# that relies on it.
set -euo pipefail

blocked() {
    case "$(uname -s)" in
    Linux)
        # Keep PATH and the caller's identity inside the namespace.
        sudo --preserve-env unshare --net -- \
            sudo --preserve-env -u "$(id -un)" env "PATH=$PATH" "$@"
        ;;
    Darwin)
        sandbox-exec -p '(version 1)(allow default)(deny network-outbound (remote ip))(deny network-outbound (remote unix-socket))' "$@"
        ;;
    *)
        echo "run_without_network: unsupported platform $(uname -s)" >&2
        return 2
        ;;
    esac
}

if [ "${1:-}" = "--probe" ]; then
    url="https://github.com/Generous-Corp/pulp"
    if ! curl -sS --max-time 15 -o /dev/null "$url"; then
        echo "run_without_network: PROBE INVALID: $url is unreachable even with the network allowed" >&2
        exit 2
    fi
    if blocked curl -sS --max-time 15 -o /dev/null "$url"; then
        echo "run_without_network: PROBE FAILED: $url was reachable inside the wrapper" >&2
        exit 1
    fi
    echo "run_without_network: probe ok: $url reachable outside, blocked inside"
    exit 0
fi

if [ "$#" -eq 0 ]; then
    echo "usage: $0 <command> [args...] | --probe" >&2
    exit 2
fi
blocked "$@"
