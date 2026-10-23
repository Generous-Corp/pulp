#!/usr/bin/env bash
# Provide the Rust that experimental/pulp-rs/rust-toolchain.toml names, before
# anything builds pulp-rs.
#
#   tools/ci/provide_pinned_rust.sh                        # fail when it cannot
#   tools/ci/provide_pinned_rust.sh --fallback-to-default  # hosted runners only
#
# The toolchain file is generated from tools/toolchain/manifest.toml and names
# an exact Rust with its components and targets. rustup reads it inside the
# crate, so a runner whose image has only some other toolchain fails the cargo
# build when RUSTUP_AUTO_INSTALL=0 (the build legs) or downloads it mid-build
# otherwise. The common path is offline: the image has the pin by name, or its
# `stable` is the same release with every part and is selected by name. Only
# otherwise does it download the pin, before the build, with spaced retries.
#
# Which branch ran is logged as `rust: branch=<name>`, and every run writes a
# JSON receipt (pinned, actual, branch, reason, attempts) to
# $RUNNER_TEMP/pulp-rust-toolchain.json, a step summary line, and step outputs
# `branch` and `receipt`, so a build that did not use the pin can be found:
#   present      the image has the pin by name with every part
#   stable-alias the image's `stable` IS the pinned release with every part:
#                RUSTUP_TOOLCHAIN=stable for the job, no download (rustup
#                refuses `toolchain link` for an official name such as 1.97.1)
#   install      rustup downloaded the pin (or the parts it lacked)
#   fallback     the download failed on a GitHub-hosted runner; the job builds
#                with the runner's stable (or default), loudly
#
# Fail closed: a self-hosted runner (the required gate's VMs) never falls back,
# and neither does a pull request that changes experimental/pulp-rs. Its image
# must carry the pin by name or as an identical stable. The fallback exists
# only until tartci bakes the pinned Rust by name into the live golden; the
# change that follows removes --fallback-to-default from build.yml, so every
# leg then fails closed (decisions contract row 29).
#
# It never changes the default toolchain. Runs on macOS /bin/bash 3.2.
set -euo pipefail

fallback=0
case "${1:-}" in
  --fallback-to-default) fallback=1 ;;
  "") ;;
  *) echo "usage: $0 [--fallback-to-default]" >&2; exit 2 ;;
esac

root="$(cd "$(dirname "$0")/../.." && pwd)"
file="$root/experimental/pulp-rs/rust-toolchain.toml"
receipt="${RUNNER_TEMP:-${TMPDIR:-/tmp}}/pulp-rust-toolchain.json"
attempts=0

# One receipt line per run: the summary, the step outputs and a JSON file.
record() {  # <branch> <actual toolchain> <actual rustc> <reason>
  local pin_value="${pin:-unknown}"
  printf '{"pinned": "%s", "branch": "%s", "toolchain": "%s", "rustc": "%s", "reason": "%s", "attempts": %d, "runner_environment": "%s"}\n' \
    "$pin_value" "$1" "$2" "$3" "$4" "$attempts" "${RUNNER_ENVIRONMENT:-unknown}" > "$receipt"
  echo "rust: receipt $receipt: $(cat "$receipt")"
  if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
    echo "Rust toolchain: branch \`$1\`, pinned \`$pin_value\`, built with \`$2\` (\`$3\`). $4" >> "$GITHUB_STEP_SUMMARY"
  fi
  if [ -n "${GITHUB_OUTPUT:-}" ]; then
    { echo "branch=$1"; echo "receipt=$receipt"; } >> "$GITHUB_OUTPUT"
  fi
}

rustup_bin="$(command -v rustup || true)"
if [ -z "$rustup_bin" ] && [ -x "$HOME/.cargo/bin/rustup" ]; then
  rustup_bin="$HOME/.cargo/bin/rustup"
fi
if [ -z "$rustup_bin" ]; then
  cargo_bin="$(command -v cargo || true)"
  if [ -n "$cargo_bin" ]; then
    # A cargo without rustup (Homebrew rust, a distro package) ignores
    # rust-toolchain.toml entirely and builds with whatever it is.
    have="$(rustc --version 2>/dev/null || echo unknown)"
    pin="$(sed -n 's/^channel = "\(.*\)"$/\1/p' "$file" 2>/dev/null || true)"
    echo "::warning title=toolchain::cargo at $cargo_bin is not rustup's, so it ignores rust-toolchain.toml: pulp-rs builds with $have, not the pinned ${pin:-Rust}"
    record no-rustup "$cargo_bin" "$have" "cargo without rustup ignores the pin"
  else
    echo "rust: no rustup and no cargo on this runner; CMake builds pulp-rs only when it finds a working cargo"
  fi
  exit 0
fi
if [ ! -f "$file" ]; then
  echo "rust: no $file in this checkout; nothing pinned"
  exit 0
fi

pin="$(sed -n 's/^channel = "\(.*\)"$/\1/p' "$file")"
components="$(sed -n 's/^components = \[\(.*\)\]$/\1/p' "$file" | tr -d '" ')"
targets="$(sed -n 's/^targets = \[\(.*\)\]$/\1/p' "$file" | tr -d '" ')"
if [ -z "$pin" ]; then
  echo "::error::$file names no channel" >&2
  exit 1
fi

# Output is captured first, then searched with a here-string: grep -q closing
# a pipe early would fail the producer under pipefail.
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
    record present "$pin" "$(rustc_version "$pin")" "the image has the pin by name"
    exit 0
  fi
elif grep -q "^stable-" <<<"$toolchains" && [ -n "${GITHUB_ENV:-}" ]; then
  stable="$(rustc_version stable)"
  stable_missing="$(parts_missing stable)"
  if [ "$stable" = "$pin" ] && [ -z "$stable_missing" ]; then
    echo "RUSTUP_TOOLCHAIN=stable" >> "$GITHUB_ENV"
    echo "rust: branch=stable-alias: the image's stable is rustc $stable, the pin, with every pinned part; RUSTUP_TOOLCHAIN=stable, no download"
    record stable-alias stable "$stable" "the image's stable is the pinned release with every part"
    exit 0
  fi
  echo "rust: the image's stable is rustc ${stable:-unknown}${stable_missing:+ lacking$stable_missing}; the pin is $pin"
fi

args=(toolchain install "$pin" --profile minimal --no-self-update)
[ -z "$components" ] || args+=(--component "$components")
[ -z "$targets" ] || args+=(--target "$targets")
delay="${PULP_RUST_INSTALL_RETRY_DELAY_SECS:-20}"
for attempt in 1 2 3; do
  attempts="$attempt"
  if "$rustup_bin" "${args[@]}"; then
    echo "rust: branch=install: installed $pin${missing:+ (was missing:$missing)}"
    record install "$pin" "$(rustc_version "$pin")" "downloaded before the build"
    exit 0
  fi
  if [ "$attempt" -lt 3 ]; then
    echo "::warning::rustup ${args[*]} attempt $attempt failed; retrying in $(( delay * attempt ))s"
    sleep $(( delay * attempt ))
  fi
done

# Why a fallback is refused here, if it is.
refusal=""
if [ "$fallback" = 0 ]; then
  refusal="no --fallback-to-default"
elif [ "${RUNNER_ENVIRONMENT:-}" != "github-hosted" ]; then
  refusal="a self-hosted runner (${RUNNER_ENVIRONMENT:-unknown}) must carry the pin; it never falls back"
elif [ -n "${GITHUB_BASE_REF:-}" ] \
    && git -C "$root" rev-parse --verify --quiet "origin/$GITHUB_BASE_REF" >/dev/null \
    && [ -n "$(git -C "$root" diff --name-only "origin/$GITHUB_BASE_REF...HEAD" -- experimental/pulp-rs 2>/dev/null)" ]; then
  refusal="this pull request changes experimental/pulp-rs, which must build with the pin"
fi
if [ -n "$refusal" ]; then
  record failed none none "could not install $pin after $attempts attempts; $refusal"
  echo "::error title=toolchain::could not install the pinned Rust $pin after $attempts attempts (rustup ${args[*]}); $refusal" >&2
  exit 1
fi

if grep -q "^stable-" <<<"$toolchains"; then
  chosen=stable
else
  chosen="$("$rustup_bin" default 2>/dev/null | awk 'NR == 1 {print $1}' || true)"
fi
have=""
[ -z "$chosen" ] || have="$(rustc_version "$chosen")"
echo "rust: branch=fallback"
echo "::warning title=toolchain::could not install the pinned Rust $pin; this run builds pulp-rs with ${chosen:-no toolchain} (rustc ${have:-unknown}), not $pin. Bake $pin into the image by name."
record fallback "${chosen:-none}" "${have:-unknown}" "download failed after $attempts attempts on a GitHub-hosted runner"
if [ -n "$chosen" ] && [ "$chosen" != "none" ] && [ -n "${GITHUB_ENV:-}" ]; then
  echo "RUSTUP_TOOLCHAIN=$chosen" >> "$GITHUB_ENV"
fi
