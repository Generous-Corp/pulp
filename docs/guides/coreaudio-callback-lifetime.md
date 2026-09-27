# CoreAudio callback and teardown lifetime

The macOS device backend gives each native render registration a stable heap
context. Entry atomically acquires permission to access the device; departure
releases it. A single closed-bit/count word prevents admission from racing the
control thread's drain. The render callback adds no lock or allocation for this
protocol. A closed callback clears the supplied output, or returns directly for
an input-only callback with no output buffer.

`stop_checked()` closes admission and waits for every admitted callback before
clearing the user function. Native stop failure therefore cannot cause concurrent
function destruction. It returns the failing native stage/status and preserves
uncertain native running/handle ownership. The existing void `stop()` delegates
and logs failure. Native running state can remain true although user processing
has stopped. Restart requires successful stop and installs a fresh context;
a closed context never becomes live again.

The drain runs on a control thread and may wait indefinitely for a stuck user
callback. It cannot safely return early: the callback may hold references to
objects the caller destroys immediately after stop. Never call start, stop,
close or destruction from the render callback or a workgroup-change callback.
Same-object teardown from an admitted native callback is detected and terminates
immediately rather than deadlocking or returning unsafe success; defer teardown
to a control thread.
Lifecycle calls are control-thread operations. `callback_workgroup()` also takes
that control-thread switch mutex: obtain the borrowed handle before starting
render/auxiliary work, never query it inside the render callback. Public open,
start, stop, close and workgroup-quiesce calls serialize through the lifecycle
mutex; internal failure cleanup uses the corresponding already-locked helpers. The default-device
listener uses the internal switch path, not the external stop/close drain of its
own listener context.

Device default-change/overload and AudioSystem device/default listeners also enter a stable context before
accessing the device. Close drains listener admission before acquiring the
switch mutex, so a listener already waiting for that mutex can finish. Render
admission is drained separately. Existing auxiliary workgroup detachment must
complete before the retained workgroup reference can be released.

`close_checked()` checks native stop, uninitialize, disposal and listener removal
separately. A failed stage retains its native ownership for retry; successful
stages are not repeated. Open/restart cannot overwrite that uncertain ownership.
If the wrapper is destroyed first, remaining native ownership moves to a
process-owned quarantine. `retry_coreaudio_native_retirements()` retries these
records on a control thread. Their callback contexts are already closed and
cannot access the destroyed wrapper, buffers or captured user data.

Small closed native contexts remain process-owned even after successful cleanup.
This deliberately does not assume listener removal has a synchronous guarantee
against an already-dispatched late callback. The tradeoff is a small allocation
per callback generation/device lifetime, not per audio block. Large input buffers,
user callables and workgroup references are not retained by these tombstones.
A failed native disposal can retain a native unit until retry or process exit;
that is reported failure, not successful resource cleanup.

`pulp-test-coreaudio-native-lifetime` injects native statuses through a private
operations table and invokes production stop/close/destructor paths without an
audio device. It covers paused admitted callbacks, late render/listener entry
after destruction, each native failure stage, retry, fresh restart, null output,
failed start and listener/mutex ordering. A focused source-linked pass is not a
physical-device or packaged standalone lifecycle acceptance result.

Quarantine retains data, not executable code. The backend framework/library that
contains the native trampolines and operations must remain loaded until process
exit while any retained native callback registration exists. This does not make
arbitrary `dlclose` safe. Plugin-only host tests that never open a Pulp physical
device exercise a separate lifetime contract.

AudioSystem property notifications retain immutable callable snapshots while
keeping owner admission through invocation. External callback clear/replacement
closes and drains all retired registration generations before returning, including
one previously self-cleared. Reentrant self-clear/replacement closes admission
without waiting for itself or other self-clearing deliveries; a later external
clear still drains those generations. Setter mutexes are never held across drain.
This covers raw-reference captures used by AudioDeviceManager. Direct calls to
the base `AudioSystem::fire_device_change()` retain their existing cross-platform
contract and are not native registrations governed by this barrier.

Checked close also accounts for the current device's timing-listener removals;
they participate in retained native retry instead of disappearing into a void
cleanup result. System property callbacks may acquire the snapshot mutex, but
render callback admission/departure remain lock-free and allocation-free.
