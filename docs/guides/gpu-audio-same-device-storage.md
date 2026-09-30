# Same-device storage experiment

This internal test harness separates storage transport from changes in device
ownership, kernels or completion policy. It does not change the public SDK or
select staged storage for product code.

Here one authenticated Dawn provider retains its actual device across trials.
`reconfigure_storage_kind()` accepts a change only after every program and slot
has physically drained and been released. A lost device, failed retirement or
failed drain cannot be relabeled as a fresh trial. `device_owner_generation()`
is a process-local lifetime token minted at device creation, not an adapter ID.
The private session's `release_to_owner()` retains ownership on a failed barrier.

Both modes use the same FFT convolution program and slot scheduler. Imported
mode wraps persistent allocations. Staged mode uploads into storage buffers,
copies output into a readback buffer, maps it, copies into the CPU output slot
and unmaps. A successful queue callback alone cannot publish staged output:
readback must have completed and been consumed. Its retained mapping future is
serviced through ProcessEvents and is not mixed into timed queue-future waits.
Transfer counters distinguish these operations from program preparation.

The opt-in `pulp-gpu-same-device-storage-probe` first exercises a shared, staged,
shared sequence with a 257-tap FIR, 1024-point FFT and stereo 128-frame blocks.
It compares against direct convolution, checks unchanged device identity and
balanced allocations, and verifies exact steady-state transfer counts. Cancelled
mapping must produce failure without readable output; a failed drain must keep
its owner and prevent later device reuse. The native gate requires the exact
configured provider and never treats provider absence as success.

The scoped build is source-linked evidence, not installed-SDK acceptance.
Lifecycle correctness alone is not a performance result. A paced paired campaign
must additionally keep lead, worker cadence, warmup, capacity and input fixed,
alternate trial order, and record every scheduled input, terminal and delivery.
Realtime qualification still needs authentic audio callback timing, contention
and reliability evidence. No shipping default or realtime guarantee follows from this probe.

## Retirement and readback

A successful mapping callback does not imply that its buffer still exists when
the serialized owner consumes the callback. Retirement invalidates that handle.
The native ordering control waits for real mapping success, retires the buffer,
and requires a failed terminal only after the physical barrier. It must neither
publish readable output nor permit reuse of the poisoned device.

## Running the comparison

After configuring exact-provider GPU tests, reserve the GPU and run
`pulp-gpu-same-device-storage-probe`. For the paired campaign, run
`pulp-gpu-same-device-paired-probe /new/output/directory` with stdout saved to a
separate log, then run:

```sh
python3 test/verify_gpu_same_device_storage.py /new/output/directory /path/to/log
python3 -m unittest discover -s test -p test_verify_gpu_same_device_storage.py
```

The paired executable refuses an existing output directory. It alternates six
trials on one device with fixed graph, lead, worker cadence and warmup. Raw rows
record callback sequence, scheduled/start/end/deadline times, admission, selected
delivery and numerical error. The verifier reconstructs the counters and checks
fixed scheduling, transfer counts and device ownership. Its planted-owner,
planted-copy, duplicate-sequence and false-GPU-summary controls must fail.

These are ordinary-thread paced callbacks using precomputed reference fallback,
not a continuously executing CPU shadow. They provide no GPU timestamps or
quiet-host/thermal attestation. Retain the raw rows, host observations, exact
source/provider/binary hashes and commands alongside any interpretation.
`performance_verdict` remains `unassigned`; passing this harness does not prove
installed-SDK or realtime reliability.
