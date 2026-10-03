# Matched native Metal audio comparator

This test-only comparator is the smallest native control needed before a
Dawn-versus-Metal scheduling comparison. It runs the same 32-frame, two-channel
complex multiply through ordinary Metal and Metal 4 on one device.

Both modes use two persistent `MTLResourceStorageModeShared` buffers per flight
slot, a prebuilt compute pipeline, and a fixed two-slot in-flight ring. The
ordinary path uses a `MTLCommandQueue`; Metal 4 uses one command allocator and
argument table per slot plus a persistent residency set. Completion is observed
by a non-realtime semaphore callback in both modes. No audio callback is
involved.

Each sequence records CPU encode start/end, commit start/return, completion
observation, and provider GPU start/end where Metal supplies them. CPU times use
`steady_clock_ns`; GPU values are Metal host seconds converted to nanoseconds and
must not be compared to CPU timestamps without clock-domain calibration.

The executable is `pulp-test-native-metal-audio-comparator`. It emits one JSON
receipt per mode and returns CTest skip (77) when Metal or Metal 4 is
unavailable. A passing receipt proves correctness, ordered two-flight retirement,
and timestamp availability. It does not establish realtime suitability or a
backend performance verdict. A Dawn receipt must match the kernel, geometry,
flight depth, host condition, and completion policy before comparing tails.
