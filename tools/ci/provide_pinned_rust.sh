#!/usr/bin/env bash
# Install the Rust that experimental/pulp-rs/rust-toolchain.toml names, before
# anything builds pulp-rs.
#
#   tools/ci/provide_pinned_rust.sh                        # fail when it cannot
#   tools/ci/provide_pinned_rust.sh --fallback-to-default  # warn, use the default
#
# The toolchain file is generated from tools/toolchain/manifest.toml and names
# an exact Rust with its components and targets. rustup reads it inside the
# crate, so a runner whose image has only some other toolchain fails the cargo
# build when RUSTUP_AUTO_INSTALL=0 (the build legs) or downloads it mid-build
# otherwise. This installs it, or the components and targets it lacks, up
# front with spaced retries, and does nothing when the image already has it.
#
# It never changes the default toolchain. With --fallback-to-default a runner
# that cannot download the pin writes RUSTUP_TOOLCHAIN=<its default> to
# $GITHUB_ENV with a warning, so a network failure costs the pin for one run
# rather than the job. Runs on macOS /bin/bash 3.2.
set -euo pipefail

fallback=0
case "${1:-}" in
  --fallback-to-default) fallback=1 ;;
  "") ;;
  *) echo "usage: $0 [--fallback-to-default]" >&2; exit 2 ;;
esac

root="$(cd "$(dirname "$0")/../.." && pwd)"
file="$root/experimental/pulp-rs/rust-toolchain.toml"
rustup_bin="$(command -v rustup || true)"
if [ -z "$rustup_bin" ] && [ -x "$HOME/.cargo/bin/rustup" ]; then
  rustup_bin="$HOME/.cargo/bin/rustup"
fi
if [ -z "$rustup_bin" ] || [ ! -f "$file" ]; then
  echo "rust: no rustup on this runner; CMake builds pulp-rs only when it finds a working cargo"
  exit 0
fi

pin="$(sed -n 's/^channel = "\(.*\)"$/\1/p' "$file")"
components="$(sed -n 's/^components = \[\(.*\)\]$/\1/p' "$file" | tr -d '" ')"
targets="$(sed -n 's/^targets = \[\(.*\)\]$/\1/p' "$file" | tr -d '" ')"
if [ -z "$pin" ]; then
  echo "::error::$file names no channel" >&2
  exit 1
fi

missing=""
# Captured first, then searched with a here-string: grep -q closing a pipe
# early would fail the producer under pipefail.
toolchains="$("$rustup_bin" toolchain list 2>/dev/null | tr -d '\r' || true)"
if grep -q "^${pin}-" <<<"$toolchains"; then
  installed="$({ "$rustup_bin" component list --installed --toolchain "$pin"
                 "$rustup_bin" target list --installed --toolchain "$pin"; } 2>/dev/null | tr -d '\r' || true)"
  for part in ${components//,/ } ${targets//,/ }; do
    grep -q -x -e "$part" -e "$part-.*" <<<"$installed" || missing="$missing $part"
  done
  if [ -z "$missing" ]; then
    echo "rust: $pin is installed with ${components:-no components} and ${targets:-no extra targets}"
    exit 0
  fi
fi

args=(toolchain install "$pin" --profile minimal --no-self-update)
[ -z "$components" ] || args+=(--component "$components")
[ -z "$targets" ] || args+=(--target "$targets")
delay="${PULP_RUST_INSTALL_RETRY_DELAY_SECS:-20}"
for attempt in 1 2 3; do
  if "$rustup_bin" "${args[@]}"; then
    echo "rust: installed $pin${missing:+ (was missing:$missing)}"
    exit 0
  fi
  if [ "$attempt" -lt 3 ]; then
    echo "::warning::rustup ${args[*]} attempt $attempt failed; retrying in $(( delay * attempt ))s"
    sleep $(( delay * attempt ))
  fi
done

if [ "$fallback" = 0 ]; then
  echo "::error::could not install the pinned Rust $pin (rustup ${args[*]})" >&2
  exit 1
fi
default="$("$rustup_bin" default 2>/dev/null | awk 'NR == 1 {print $1}' || true)"
echo "::warning title=toolchain::could not install the pinned Rust $pin; this run builds pulp-rs with the runner's default toolchain (${default:-none}). Bake $pin into the image by name."
if [ -n "$default" ] && [ "$default" != "none" ] && [ -n "${GITHUB_ENV:-}" ]; then
  echo "RUSTUP_TOOLCHAIN=$default" >> "$GITHUB_ENV"
fi
