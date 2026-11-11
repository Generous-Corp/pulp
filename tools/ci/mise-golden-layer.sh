#!/usr/bin/env bash
# mise-golden-layer.sh — install Pulp's pinned toolchain into a booted golden VM.
#
#   tools/ci/mise-golden-layer.sh <manifest.toml> <guest-ip> <receipt.json>
#
# Called by `tart-provision.sh manifest` for a manifest that declares [mise]
# (.shipyard/vm-image.mise.toml). Every tool is pinned and fetched only here, at
# bake time, so a job in the golden never fetches one mid-build:
#   1. the host's pinned mise binary is copied to the same home-relative path in
#      the guest and refused unless its sha256 is the manifest's, on both sides;
#   2. this checkout's .mise.toml, .mise.toolchain.toml and mise.toolchain.lock
#      (at a clean, recorded commit) go to ~/.pulp-toolchain in the guest, and
#      `MISE_ENV=toolchain mise install --locked` installs from the checksummed
#      lock;
#   3. rustup never self-updates, and the manifest's Rust is installed BY NAME,
#      with its components and targets;
#   4. tools/toolchain/check_env.py runs in the guest against this checkout's
#      expected.json, with the pinned tools active, and the bake is refused on
#      any difference (Xcode included): the golden holds the manifest's set or
#      it is not a golden;
#   5. the guest's own report of what it now holds, the check's included, is
#      written to <receipt.json>.
# It changes no PATH and no runner setting: a lane uses these tools only when it
# opts in (MISE_ENV=toolchain, mise exec).
#
# Guest commands run over ssh as tart-provision.sh's do. MISE_LAYER_GUEST=local
# runs them in a local shell instead (tests only), with MISE_LAYER_GUEST_HOME as
# the guest's home.
set -euo pipefail

MANIFEST="${1:?usage: mise-golden-layer.sh <manifest.toml> <guest-ip> <receipt.json>}"
IP="${2:?guest ip}"
RECEIPT="${3:?receipt path}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
VM_USER="${PULP_VM_USER:-admin}"
SSH_KEY_PUB="${PULP_VM_SSH_KEY_PUB:-$HOME/.ssh/id_ed25519.pub}"
SSH_KEY_PRIV="${SSH_KEY_PUB%.pub}"
SSH_OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=10 -o BatchMode=yes)

die(){ printf 'mise-golden-layer: %s\n' "$*" >&2; exit 1; }
note(){ printf 'mise-golden-layer: %s\n' "$*" >&2; }

guest(){ # run a shell command in the guest
  if [ "${MISE_LAYER_GUEST:-}" = local ]; then HOME="${MISE_LAYER_GUEST_HOME:?}" bash -c "$1"
  else ssh "${SSH_OPTS[@]}" -i "$SSH_KEY_PRIV" "$VM_USER@$IP" "$1"; fi
}
to_guest(){ # copy host file $1 to guest home-relative path $2
  if [ "${MISE_LAYER_GUEST:-}" = local ]; then
    mkdir -p "$(dirname "${MISE_LAYER_GUEST_HOME:?}/$2")" && cp -p "$1" "$MISE_LAYER_GUEST_HOME/$2"
  else
    guest "mkdir -p \"\$HOME/$(dirname "$2")\""
    scp -q "${SSH_OPTS[@]}" -i "$SSH_KEY_PRIV" "$1" "$VM_USER@$IP:$2"
  fi
}
sha256_of(){ shasum -a 256 "$1" | awk '{print $1}'; }

# The [mise] table, read with the same python3 tart-provision.sh uses.
MISE_VARS="$(python3 - "$MANIFEST" <<'PY'
import shlex, sys, tomllib
m = tomllib.load(open(sys.argv[1], "rb")).get("mise")
if not isinstance(m, dict):
    sys.exit("manifest has no [mise] table")
for key in ("version", "path", "sha256", "rust"):
    if not isinstance(m.get(key), str) or not m[key]:
        sys.exit("[mise]." + key + " must be set")
if not m["path"].startswith("~/"):
    sys.exit("[mise].path must be home-relative (~/...), the same on host and guest")
values = {
    "ML_VERSION": m["version"],
    "ML_PATH": m["path"][2:],
    "ML_SHA": m["sha256"],
    "ML_RUST": m["rust"],
    "ML_COMPONENTS": " ".join(m.get("rust_components", [])),
    "ML_TARGETS": " ".join(m.get("rust_targets", [])),
}
for key, value in values.items():
    print(key + "=" + shlex.quote(value))
PY
)" || die "cannot read [mise] from $MANIFEST"
eval "$MISE_VARS"

# 0. A recorded commit: the bake must come from committed toolchain files.
COMMIT="$(git -C "$ROOT" rev-parse HEAD)"
if [ -n "$(git -C "$ROOT" status --porcelain -- .mise.toml .mise.toolchain.toml mise.toolchain.lock tools/toolchain/expected.json tools/toolchain/check_env.py)" ]; then
  die "the toolchain files differ from $COMMIT; bake from a clean checkout"
fi

# 1. The pinned mise binary, by digest on both sides.
HOST_MISE="$HOME/$ML_PATH"
[ -f "$HOST_MISE" ] || die "the pinned mise is not at $HOST_MISE on this host"
got="$(sha256_of "$HOST_MISE")"
[ "$got" = "$ML_SHA" ] || die "host $HOST_MISE has sha256 $got, the manifest pins $ML_SHA"
to_guest "$HOST_MISE" "$ML_PATH"
guest_sha="$(guest "chmod 755 \"\$HOME/$ML_PATH\" && shasum -a 256 \"\$HOME/$ML_PATH\" | awk '{print \$1}'")"
[ "$guest_sha" = "$ML_SHA" ] || die "guest mise has sha256 $guest_sha, the manifest pins $ML_SHA"
reported="$(guest "MISE_DISABLE_UPDATE_WARNING=1 \"\$HOME/$ML_PATH\" --version" | awk '{print $1}')"
[ "$reported" = "$ML_VERSION" ] || die "guest mise reports $reported, the manifest pins $ML_VERSION"
note "mise $ML_VERSION in the guest, sha256 $ML_SHA"

# 2. The locked toolchain. MISE_LOCKED and MISE_AUTO_INSTALL hold even if mise
# stops applying the trusted project's [settings]: environment settings
# never depend on trust, so no guest command can resolve or install a tool.
for f in .mise.toml .mise.toolchain.toml mise.toolchain.lock; do
  to_guest "$ROOT/$f" ".pulp-toolchain/$f"
done
guest "cd \"\$HOME/.pulp-toolchain\" && MISE_TRUSTED_CONFIG_PATHS=\"\$PWD\" MISE_LOCKED=1 MISE_AUTO_INSTALL=0 MISE_ENV=toolchain MISE_DISABLE_UPDATE_WARNING=1 MISE_YES=1 \"\$HOME/$ML_PATH\" install --locked" \
  || die "mise install --locked failed in the guest"

# 3. Rust by name, and rustup never self-updates.
rust_args="--profile minimal"
for c in $ML_COMPONENTS; do rust_args="$rust_args --component $c"; done
for t in $ML_TARGETS; do rust_args="$rust_args --target $t"; done
guest "R=\"\$HOME/.cargo/bin/rustup\"; [ -x \"\$R\" ] || { echo 'no rustup in the guest at ~/.cargo/bin/rustup' >&2; exit 1; }; \"\$R\" set auto-self-update disable && \"\$R\" toolchain install $ML_RUST $rust_args" \
  || die "rustup toolchain install $ML_RUST failed in the guest"
guest "\"\$HOME/.cargo/bin/rustup\" toolchain list | grep -q '^$ML_RUST-'" \
  || die "rustup lists no toolchain named $ML_RUST after installing it"

# 4. The same check a host runs, in the guest, with the pinned tools active.
# RUSTUP_TOOLCHAIN names the pinned Rust: the golden's rustup default may be
# another, and cargo selects by rust-toolchain.toml, not by the default.
CHECK_ENV="${MISE_LAYER_CHECK_ENV:-$ROOT/tools/toolchain/check_env.py}"
[ -f "$CHECK_ENV" ] || die "no toolchain check at $CHECK_ENV"
to_guest "$CHECK_ENV" ".pulp-toolchain/check_env.py"
to_guest "$ROOT/tools/toolchain/expected.json" ".pulp-toolchain/expected.json"
set +e
check_json="$(guest "cd \"\$HOME/.pulp-toolchain\" && RUSTUP_TOOLCHAIN='$ML_RUST' MISE_TRUSTED_CONFIG_PATHS=\"\$PWD\" MISE_LOCKED=1 MISE_AUTO_INSTALL=0 MISE_ENV=toolchain MISE_DISABLE_UPDATE_WARNING=1 \"\$HOME/$ML_PATH\" exec -- python3 check_env.py --root \"\$PWD\" --expected expected.json --fleet-path --require-tools --format json")"
check_rc=$?
set -e
printf '%s\n' "$check_json" | python3 -c "import json,sys; json.load(sys.stdin)" 2>/dev/null \
  || die "the guest toolchain check printed no JSON (exit $check_rc)"
if [ "$check_rc" -ne 0 ]; then
  printf '%s\n' "$check_json" >&2
  die "the guest differs from tools/toolchain/manifest.toml (check_env exit $check_rc); this golden is not the pinned set"
fi
note "the guest matches tools/toolchain/manifest.toml"

# 5. The guest's own report.
lock_sha="$(sha256_of "$ROOT/mise.toolchain.lock")"
check_tmp="$(mktemp)"; trap 'rm -f "$check_tmp"' EXIT
printf '%s\n' "$check_json" > "$check_tmp"
guest "cd \"\$HOME/.pulp-toolchain\" && { printf '{\"commit\": \"%s\", \"lock_sha256\": \"%s\", \"mise_sha256\": \"%s\", \"mise_version\": \"%s\", \"rust\": \"%s\",\n' '$COMMIT' '$lock_sha' '$ML_SHA' '$ML_VERSION' '$ML_RUST'; printf ' \"mise_ls\": '; MISE_TRUSTED_CONFIG_PATHS=\"\$PWD\" MISE_LOCKED=1 MISE_AUTO_INSTALL=0 MISE_ENV=toolchain MISE_DISABLE_UPDATE_WARNING=1 \"\$HOME/$ML_PATH\" ls --current --json; printf ',\n \"rustup_toolchains\": \"%s\",\n' \"\$(\"\$HOME/.cargo/bin/rustup\" toolchain list | tr '\n' ';')\"; printf ' \"xcode\": \"%s\"}\n' \"\$(xcodebuild -version 2>/dev/null | tr '\n' ' ')\"; }" > "$RECEIPT" \
  || die "could not write the receipt"
python3 - "$RECEIPT" "$check_tmp" <<'PY' || die "the receipt is not valid JSON: $RECEIPT"
import json, sys
receipt = json.load(open(sys.argv[1]))
receipt["toolchain_check"] = json.load(open(sys.argv[2]))
with open(sys.argv[1], "w") as handle:
    json.dump(receipt, handle, indent=1)
    handle.write("\n")
PY
note "receipt: $RECEIPT"
