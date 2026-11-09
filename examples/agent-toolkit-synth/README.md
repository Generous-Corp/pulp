# Agent Toolkit Synth

A one-voice sine synth that lets an agent work with it through the capability
broker. It binds the agent tool kit, so a client can list, load, and save
presets, read and set the patch, randomize it from a seed, play notes offline,
and analyze a rendered test signal. Each call goes through the broker's grants,
consent, schema checks, and durable receipts.

The interesting files are:

- `CMakeLists.txt` writes the tool kit's vendor operation declaration with
  `pulp_control_agent_toolkit_declaration()` and passes it to
  `CONTROL_VENDOR_OPERATIONS`. `CONTROL_CAPABILITIES` lists the four vendor
  tiers the declaration uses.
- `agent_operations.cpp` installs the handler factory. It is listed under
  `CONTROL_VENDOR_SOURCES`, so it is built only into the controlled Standalone
  and the host the broker launches.

Build and call it:

```sh
pulp build --examples --target AgentToolkitSynth_Standalone
pulp control instances --json
pulp control grant-request --instance <instance-id> --profile observe
pulp control call --instance <instance-id> dev.com.example/patch.schema@1
pulp control grant-request --instance <instance-id> --operation dev.com.example/notes.play@1
pulp control call --instance <instance-id> dev.com.example/notes.play@1 \
    --params '{"duration_seconds":1,"notes":[{"note":60,"start_seconds":0,"duration_seconds":0.5}]}'
```

Over MCP, use `pulp_control_vendor_call` with the same operation ids. See
[Vendor operations](../../docs/reference/capability-control.md#vendor-operations)
for the tiers and grant rules.
