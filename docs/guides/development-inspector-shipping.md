# Shipping a Development Inspector Endpoint

Release artifacts are inspector-free by default. `PULP_ENABLE_INSPECTOR=ON`
only makes optional SDK components available; it does not link a listener,
discovery publisher, server, registration, or runtime evaluator into an
ordinary `pulp_add_plugin` target.

An intentionally inspectable developer edition must declare both the product
decision and its exact capabilities:

```cmake
pulp_add_plugin(MyDeveloperEdition
    FORMATS Standalone
    SHIP_INSPECTOR
    INSPECTOR_CAPABILITIES
        session.describe state.read ui.read diagnostics.read logs.read
        capture.image telemetry.stream)
```

This declaration links the inspector-capable standalone component and embeds a
retained capability marker plus `<target>.inspector-capabilities.json`. It does
not activate the endpoint: the product still owns runtime profile selection,
and the default remains off. Once activated, the manifest is the maximum runtime
grant set. The effective grants are its intersection with the selected runtime
profile, so neither a broader profile nor a broader manifest widens the other.

`runtime.eval` is arbitrary execution in the product process. The generic
shipping override never implies it. A target that truly needs it must declare
the capability and the distinct unsafe acknowledgement:

```cmake
    SHIP_INSPECTOR
    SHIP_INSPECTOR_RUNTIME_EVAL
    INSPECTOR_CAPABILITIES session.describe session.control runtime.eval
```

Packaging repeats the review boundary. Use `--ship-inspector` for an endpoint
declared by the build manifest. If and only if that manifest includes
`runtime.eval`, also pass `--ship-inspector-runtime-eval`. Manifest/flag
mismatches fail before packaging. `pulp validate --json`, `pulp ship check
--json`, and `pulp ship package --json` include the capability report; package
also writes `artifacts/inspector-capability-package-input.json`.

Every standalone build runs a manifest-versus-binary scanner using retained,
Pulp-specific shipping and capability markers. Intentional artifacts fail if
their endpoint or high-risk evaluator marker is missing, or if the evaluator
appears without its separate acknowledgement. Generic class or symbol names
are not treated as proof because unrelated product code may use the same text.
