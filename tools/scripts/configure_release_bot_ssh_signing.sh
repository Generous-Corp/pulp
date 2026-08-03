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

# Write the bot identity into a config file this script OWNS, and point git at
# it, rather than into whatever `--global` resolves to.
#
# On a hosted runner those are the same file and nothing changes. On a
# self-hosted runner — which is where Pulp's required macOS lane runs — they
# are not: `--global` is the human's ~/.gitconfig, so a release job would
# replace their name, email and signing key with the bot's, in every repo on
# the machine, permanently. That happened: a developer's commits silently
# stopped signing because user.signingkey pointed at this script's RUNNER_TEMP
# key, and kept failing after the temp dir was cleaned up.
#
# GIT_CONFIG_GLOBAL is exported so later steps in the same job inherit it.
bot_config="${key_dir}/gitconfig"
: > "${bot_config}"
export GIT_CONFIG_GLOBAL="${bot_config}"
if [ -n "${GITHUB_ENV:-}" ]; then
    echo "GIT_CONFIG_GLOBAL=${bot_config}" >> "${GITHUB_ENV}"
fi

git config --file "${bot_config}" gpg.format ssh
git config --file "${bot_config}" user.signingkey "${key_path}"
git config --file "${bot_config}" user.name "${RELEASE_BOT_NAME}"
git config --file "${bot_config}" user.email "${RELEASE_BOT_EMAIL}"
git config --file "${bot_config}" commit.gpgsign true
git config --file "${bot_config}" tag.gpgSign true

echo "::notice::Configured SSH signing for ${RELEASE_BOT_NAME} <${RELEASE_BOT_EMAIL}>."
