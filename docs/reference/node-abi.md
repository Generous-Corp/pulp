# Node ABI

Pulp exposes two SDK-facing C++ node surfaces:

- `pulp::format::Processor`, implemented by plug-ins
- `pulp::host::PluginSlot`, implemented by host-side format loaders

Both surfaces include `pulp/runtime/node_abi.hpp`, which defines
`PULP_NODE_ABI_VERSION` and `pulp_node_abi_version()`. Version `1` is the
current node ABI generation.

## Compatibility Contract

Pulp's node ABI is a source-compatibility contract. Plug-ins and host nodes are
expected to be recompiled with the Pulp SDK they ship against. The version
constant lets code, tests, and generated artifacts state the node interface
generation they were built for.

This is not a claim of stable C++ binary ABI across arbitrary compilers,
standard libraries, compiler flags, or struct layouts. A truly stable binary
node ABI would need a dedicated C shim. Pulp does not expose that shim today.

## Virtual Method Policy

Within a node ABI generation, virtual methods on `Processor` and `PluginSlot`
are append-only:

- do not reorder existing virtual methods
- do not remove existing virtual methods
- do not change an existing virtual method signature
- add new virtual methods only after the current virtual-method tail

`tools/scripts/node_abi_gate.py` enforces this in CI by comparing the current
virtual declarations against the PR base. A middle insert, removal, reorder, or
signature change fails; appending at the tail passes.

## Optional Capabilities

New optional node behavior should prefer additive capability bits over new
virtual methods. `PluginDescriptor::node_capabilities` carries the forward
compatible capability field, while legacy `supports_mpe` and `supports_ump`
remain valid for source compatibility.

Use `PluginDescriptor::effective_capabilities()` when adapter or host code
needs the effective value. It ORs the legacy flags with the node capability
field so both declaration styles behave the same.
