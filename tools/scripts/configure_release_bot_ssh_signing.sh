#!/usr/bin/env bash
set -euo pipefail

: "${RELEASE_BOT_SSH_SIGNING_KEY:?RELEASE_BOT_SSH_SIGNING_KEY secret is required for signed release-bot commits/tags}"
: "${RELEASE_BOT_NAME:=pulp-release-bot}"
: "${RELEASE_BOT_EMAIL:=25807+danielraffel@users.noreply.github.com}"

key_dir="${RUNNER_TEMP:-/tmp}/pulp-release-bot-signing"
key_path="${key_dir}/ssh_signing_key"

mkdir -p "${key_dir}"
chmod 700 "${key_dir}"
printf '%s\n' "${RELEASE_BOT_SSH_SIGNING_KEY}" > "${key_path}"
chmod 600 "${key_path}"

ssh-keygen -y -f "${key_path}" > /dev/null

git config --global gpg.format ssh
git config --global --unset-all gpg.ssh.program || true
git config --global user.signingkey "${key_path}"
git config --global user.name "${RELEASE_BOT_NAME}"
git config --global user.email "${RELEASE_BOT_EMAIL}"
git config --global commit.gpgsign true
git config --global tag.gpgSign true

echo "::notice::Configured SSH signing for ${RELEASE_BOT_NAME} <${RELEASE_BOT_EMAIL}>."
