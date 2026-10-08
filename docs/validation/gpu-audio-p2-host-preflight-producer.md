# P2 host-preflight producer

`tools/scripts/gpu_audio_p2_host_preflight.py` is the host-side producer for
the `pulp.gpu-audio.p2.host-preflight.v1` input consumed by the P2 campaign
driver. It records and hashes the raw observations instead of claiming that a
green memory-pressure check proves a quiet host.

The collector samples:

- `tools/scripts/host_vitals.sh --json` for pressure and one-minute load;
- `ps` for process and `WindowServer` CPU contention;
- `pmset -g therm` for historical thermal/performance warning fields; and
- `ioreg -r -c IOAccelerator -l` for diagnostic accelerator registry fields.

The receipt passes only when all observations are available, the source tree is
clean and bound to an immutable commit, load is finite and at most half the
reported CPU count, no sampled process has a non-finite or over-threshold CPU
value, WindowServer is present and below its UI threshold, and supported
current thermal and GPU-idle telemetry is available. `pmset` warning absence
is retained as an observation but is not current thermal proof. Likewise,
IORegistry `busy 0` is not authenticated execution-queue evidence, so this
producer reports GPU status as `unknown` and blocks until a supported queue or
utilization source is wired. Otherwise it writes a `blocked` receipt and exits
non-zero. A blocked receipt is useful evidence that the host is not admitted;
it cannot be supplied to the campaign driver as a pass.

Run on a reserved host with:

```sh
python3 tools/scripts/gpu_audio_p2_host_preflight.py \
  --output /absolute/path/to/p2-host-preflight.json
```

The current M5 Studio observation is expected to block while indexing and
other work drive load above the quiet threshold. This producer does not prove
provider identity, model provenance, or realtime behavior; those remain
separate campaign prerequisites.
