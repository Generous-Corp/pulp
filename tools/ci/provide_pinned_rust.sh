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
# otherwise. The common path is offline: the image has the pin by name, or its
# `stable` is the same release with every part and is selected by name. Only
# otherwise does it download the pin, before the build, with spaced retries.
#
# It never changes the default toolchain. With --fallback-to-default a runner
# that cannot download the pin writes RUSTUP_TOOLCHAIN=stable (or its default)
# to $GITHUB_ENV with a warning naming both versions, so a network failure
# costs the pin for one run rather than the job. Runs on macOS /bin/bash 3.2.
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

# Which branch ran is logged as `rust: branch=<name>`, so a golden that drifts
# from the pin shows up in the job log:
#   present      the image has the pin by name with every part: nothing to do
#   stable-alias the image's `stable` IS the pinned release with every part:
#                RUSTUP_TOOLCHAIN=stable for the job, no download
#   install      rustup downloads the pin (or the parts it lacks)
#   fallback     the download failed; the runner's own toolchain, with a warning
# rustup refuses `toolchain link` for an official name such as 1.97.1, so a
# verified-identical `stable` is selected by name instead of aliased.

# Captured first, then searched with a here-string: grep -q closing a pipe
# early would fail the producer under pipefail.
parts_missing() {  # <toolchain>: the components and targets it lacks
  local have out=""
  have="$({ "$rustup_bin" component list --installed --toolchain "$1"
            "$rustup_bin" target list --installed --toolchain "$1"; } 2>/dev/null | tr -d '\r' || true)"
  for part in ${components//,/ } ${targets//,/ }; do
    grep -q -x -e "$part" -e "$part-.*" <<<"$have" || out="$out $part"
  done
  printf '%s' "$out"
}
rustc_version() {  # <toolchain>: its rustc release, or nothing
  "$rustup_bin" run "$1" rustc --version 2>/dev/null | awk 'NR == 1 {print $2}' || true
}

toolchains="$("$rustup_bin" toolchain list 2>/dev/null | tr -d '\r' || true)"
missing=""
if grep -q "^${pin}-" <<<"$toolchains"; then
  missing="$(parts_missing "$pin")"
  if [ -z "$missing" ]; then
    echo "rust: branch=present: $pin is installed with ${components:-no components} and ${targets:-no extra targets}"
    exit 0
  fi
elif grep -q "^stable-" <<<"$toolchains" && [ -n "${GITHUB_ENV:-}" ]; then
  stable="$(rustc_version stable)"
  stable_missing="$(parts_missing stable)"
  if [ "$stable" = "$pin" ] && [ -z "$stable_missing" ]; then
    echo "RUSTUP_TOOLCHAIN=stable" >> "$GITHUB_ENV"
    echo "rust: branch=stable-alias: the image's stable is rustc $stable, the pin, with every pinned part; RUSTUP_TOOLCHAIN=stable, no download"
    exit 0
  fi
  echo "rust: the image's stable is rustc ${stable:-unknown}${stable_missing:+ lacking$stable_missing}; the pin is $pin"
fi

args=(toolchain install "$pin" --profile minimal --no-self-update)
[ -z "$components" ] || args+=(--component "$components")
[ -z "$targets" ] || args+=(--target "$targets")
delay="${PULP_RUST_INSTALL_RETRY_DELAY_SECS:-20}"
for attempt in 1 2 3; do
  if "$rustup_bin" "${args[@]}"; then
    echo "rust: branch=install: installed $pin${missing:+ (was missing:$missing)}"
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
if grep -q "^stable-" <<<"$toolchains"; then
  chosen=stable
else
  chosen="$("$rustup_bin" default 2>/dev/null | awk 'NR == 1 {print $1}' || true)"
fi
have="$( [ -n "$chosen" ] && rustc_version "$chosen" )"
echo "rust: branch=fallback"
echo "::warning title=toolchain::could not install the pinned Rust $pin; this run builds pulp-rs with ${chosen:-no toolchain} (rustc ${have:-unknown}), not $pin. Bake $pin into the image by name."
if [ -n "$chosen" ] && [ "$chosen" != "none" ] && [ -n "${GITHUB_ENV:-}" ]; then
  echo "RUSTUP_TOOLCHAIN=$chosen" >> "$GITHUB_ENV"
fi
