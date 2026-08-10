#!/bin/sh
# Generated from control-examples.json. Run with an installed `pulp` on PATH.
set -eu

case "${1:-inventory}" in
  inventory)
    # management: List broker-owned live instances before selecting one exact ID.
    pulp control instances --json
    ;;
  status)
    # management: Explain the authority terms for one exact live instance.
    : "${INSTANCE_ID:?set INSTANCE_ID before running status}"
    pulp control status --instance "${INSTANCE_ID}" --explain --json
    ;;
  t0-offline-render)
    # T0: Render a launcher-trusted input through an exact offline-job registration.
    : "${T0_INSTANCE_ID:?set T0_INSTANCE_ID before running t0-offline-render}"
    : "${T0_GRANT_ID:?set T0_GRANT_ID before running t0-offline-render}"
    : "${T0_INPUT_ARTIFACT_ID:?set T0_INPUT_ARTIFACT_ID before running t0-offline-render}"
    pulp control call --instance "${T0_INSTANCE_ID}" dev.pulp.render/offline@1 --grant "${T0_GRANT_ID}" --params "{\"input_artifact_id\":\"${T0_INPUT_ARTIFACT_ID}\",\"max_frames\":48000,\"timeout_ms\":5000}" --timeout-ms 10000 --json
    ;;
  t1-state-read)
    # T1: Read the bounded visible parameter catalog from one exact standalone instance.
    : "${T1_INSTANCE_ID:?set T1_INSTANCE_ID before running t1-state-read}"
    pulp control call --instance "${T1_INSTANCE_ID}" dev.pulp.state/read@1 --profile inspect-readonly --params '{"include_catalog":true,"include_sensitive":false}' --json
    ;;
  t1-parameter-gesture)
    # T1: Apply one normalized parameter gesture after broker-owned develop consent.
    : "${T1_INSTANCE_ID:?set T1_INSTANCE_ID before running t1-parameter-gesture}"
    pulp control call --instance "${T1_INSTANCE_ID}" dev.pulp.state/parameter-gesture@1 --profile develop --params '{"parameter_id":1,"normalized_value":0.5,"idempotency_key":"phase11-example-gesture"}' --json
    ;;
  revoke)
    # management: Revoke a broker-issued grant owned by this authenticated client.
    : "${GRANT_ID:?set GRANT_ID before running revoke}"
    pulp control revoke --grant "${GRANT_ID}" --json
    ;;
  *)
    echo "usage: $0 {inventory|status|t0-offline-render|t1-state-read|t1-parameter-gesture|revoke}" >&2
    exit 2
    ;;
esac
