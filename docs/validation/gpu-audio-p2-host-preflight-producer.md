# P2 host-preflight producer

`tools/scripts/gpu_audio_p2_host_preflight.py` is the host-side producer for
the `pulp.gpu-audio.p2.host-preflight.v1` input consumed by the P2 campaign
driver. It records and hashes the raw observations instead of claiming that a
green memory-pressure check proves a quiet host.

The collector samples:

- `tools/scripts/host_vitals.sh --json` for pressure and one-minute load;
- `ps` for process and `WindowServer` CPU contention;
- `pmset -g therm` for thermal/performance warnings; and
- `ioreg -r -c IOAccelerator -l` for active GPU work queues.

The receipt passes only when all observations are available, the source tree is
clean and bound to an immutable commit, load is at most half the reported CPU
count, no sampled process exceeds the contention threshold, WindowServer is
present and below its UI threshold, thermal warnings are absent, and the GPU
reports zero busy work queues. Otherwise it writes a `blocked` receipt and
exits non-zero. A blocked receipt is useful evidence that the host is not
admitted; it cannot be supplied to the campaign driver as a pass.

Run on a reserved host with:

```sh
python3 tools/scripts/gpu_audio_p2_host_preflight.py \
  --output /absolute/path/to/p2-host-preflight.json
```

The current M5 Studio observation is expected to block while indexing and
other work drive load above the quiet threshold. This producer does not prove
provider identity, model provenance, or realtime behavior; those remain
separate campaign prerequisites.
