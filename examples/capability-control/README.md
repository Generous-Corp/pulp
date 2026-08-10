# Capability-control examples

This directory is the single source for the installed CLI and MCP examples.
`control-examples.json` owns the intent, exact operation IDs, inputs, and tool
names. `generate_examples.py` produces:

- `generated/cli-walkthrough.sh`, which invokes only the installed `pulp`;
- `generated/mcp-tools.jsonl`, one copyable MCP call per matching example.

Regenerate after editing the corpus, then check that the committed outputs are
fresh:

```sh
python3 examples/capability-control/generate_examples.py
python3 examples/capability-control/generate_examples.py --check
```

## Run from an installed SDK

Release/SDK installation places the generated files under
`share/pulp/capability-control/`. On a clean machine, with no Pulp source
checkout, set the prefix where Pulp was installed:

```sh
PULP_SDK_PREFIX=/path/to/pulp-sdk
EXAMPLES="$PULP_SDK_PREFIX/share/pulp/capability-control"

"$EXAMPLES/cli-walkthrough.sh" inventory
INSTANCE_ID=exact-broker-id "$EXAMPLES/cli-walkthrough.sh" status
T1_INSTANCE_ID=exact-broker-id "$EXAMPLES/cli-walkthrough.sh" t1-state-read
```

The T1 gesture example requests the `develop` grant profile. The broker owns
the consent decision; the script cannot approve itself. Parameter ID `1` is a
placeholder and must be replaced in the source corpus for a real product.

T0 is deliberately less generic. An offline-job integration must first use a
trusted launcher to publish its input and issue a render grant. The example
therefore requires all three opaque values rather than accepting a path:

```sh
T0_INSTANCE_ID=exact-job-id \
T0_GRANT_ID=broker-grant-id \
T0_INPUT_ARTIFACT_ID=broker-input-id \
  "$EXAMPLES/cli-walkthrough.sh" t0-offline-render
```

This refusal to accept a filename or mint a grant in the script is intentional.
It keeps input resolution and authority with the trusted T0 integration.

The JSONL file is data, not a shell script. Replace `${INSTANCE_ID}`-style
placeholders in the `arguments` object before sending a tool call. MCP metadata
and client UI confirmation are not consent; broker policy remains authoritative.
