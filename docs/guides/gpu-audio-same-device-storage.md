# Same-device storage experiment

This private experiment separates storage transport from changes in device
ownership, kernels or completion policy. It does not change the public SDK or
select staged storage for product code.

The earlier GPU-NAM M1/M5 comparison changed several things together: staged
upload/readback versus imported allocations, synchronous versus asynchronous
observation, and shared versus per-channel devices. Those results describe
complete architectures. They cannot assign all measured savings to payload
copies.

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
Full P4 still needs authentic audio callback timing, contention and reliability
evidence. No shipping default or realtime guarantee follows from this probe.

## Bounded source-linked screening, September 27, 2026

On the M5, the lifecycle probe returned the same physical device through
shared → staged → shared. Eight blocks per phase matched direct convolution
within 2.75e-9. Runtime WriteBuffer/CopyBuffer/MapAsync counts were 0/8/0 per
operation. All host allocations and slot lifetimes balanced.

An independent review caught a retirement ordering hazard: a successful mapping
callback does not imply its buffer still exists when the serialized owner later
consumes the callback. The native regression now holds owner consumption until
real mapping success, retires the actual buffer, and requires failure only after
the physical barrier. It cannot publish readable output or reuse that poisoned
device. A separate cancelled-map control and a failed-first-drain ownership
control pass. Removing the tracker's pending-readback quarantine makes its named
CPU regression fail, confirming that the test detects the missing barrier.

Six alternating paired trials used one device, the same convolution graph,
128-frame blocks, two-block lead and a 50 µs sleeping/notification worker. Each
trial measured 750 callbacks after 32 warmup callbacks. All six selected GPU
output for all 750 measured callbacks, with no measured fallback, rejected
admission or callback deadline miss. Maximum output error was 4.20e-9.

| Trial order | Shared process CPU, seconds | Staged process CPU, seconds |
| --- | ---: | ---: |
| Shared then staged | 0.161514 | 0.177472 |
| Staged then shared | 0.166489 | 0.181109 |
| Shared then staged | 0.163899 | 0.176812 |

Shared mode recorded zero transfers; staged mode recorded 783 of each transfer
operation per trial. The roughly 8% process-CPU difference is a short screening
observation, not a stable speedup claim. Unlike the earlier NAM comparison, this
holds physical device ownership, graph and requested completion policy fixed.
It does not attribute the much larger earlier difference solely to copying.

These were ordinary-thread paced callbacks on a host also running builds. They
used precomputed reference fallback, not a continuously executing CPU shadow.
There are no GPU timestamps, runtime completion-policy fallback counters or
quiet-host/thermal receipts in this campaign. No installed-SDK, authentic audio
callback, long-tail reliability or shipping-default acceptance follows from it.
`performance_verdict` remains `unassigned`.

Raw rows contain callback sequence, scheduled/start/end/deadline times,
admission, selected delivery and numerical error. The independent verifier
checks all 4,704 rows, reconstructed counters, fixed scheduling, shared transfer
absence and unchanged device ownership. Its planted-owner, planted-copy,
duplicated-sequence and false-GPU-summary controls must fail. Preserve those
CSVs and the command/source/provider/binary receipts with the research note.

To reproduce after configuring exact-provider GPU tests, reserve the GPU and run
`pulp-gpu-same-device-storage-probe`. Run
`pulp-gpu-same-device-paired-probe /new/output/directory` with stdout saved to a
separate log, then `python3 test/verify_gpu_same_device_storage.py
/new/output/directory /path/to/log`. The paired executable refuses an existing
output directory. CPU verifier controls use `python3 -m unittest discover -s
test -p test_verify_gpu_same_device_storage.py`.
