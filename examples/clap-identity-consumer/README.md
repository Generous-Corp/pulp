# Strict CLAP identity consumer

This standalone project uses only the installed `Pulp::host` SDK. It explicitly
loads a CLAP artifact, selects exactly one descriptor by its exact ID, prints the
canonical type record, and verifies detached receipt ownership and declared-only
receipt import. Loading executes plugin code in the calling process.

Configure with `cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/pulp-sdk
-DCMAKE_BUILD_TYPE=Release`, then build through the host's governed build command.
Run `clap-identity-consumer /absolute/path/to/plugin.clap exact.descriptor.id`.
The supplied plugin must not advertise `org.pulp.identity.absent`, which is the
consumer's negative selector control.

`PluginSlot::load` remains the legacy best-effort API. `load_exact` supports CLAP
only and refuses invalid, absent, duplicate or incompletely enumerated IDs. It
checks the initialized plugin's own descriptor before returning loaded evidence.
Names and filesystem paths never substitute for the descriptor ID. This bounded
API accepts IDs up to 4096 bytes, factories up to 1024 descriptors, and executable
artifacts up to 1 GiB; exceeding a bound refuses the strict load.

Artifact hashes describe observed executable bytes, not portable release identity,
state restoration, containment or audio equivalence. Serialized receipts and
scanner cache entries are unsigned declared captures; importing them cannot mint
loaded evidence. Only a successful strict load creates that evidence. Scanner
captures distinguish descriptor origin from an authored assertion without loading
or initializing individual plugin instances.
