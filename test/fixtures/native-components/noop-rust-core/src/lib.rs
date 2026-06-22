//! Reference no-op native DSP core proving the Pulp native-component C ABI from
//! Rust. TEST FIXTURE ONLY — not a shipped crate, not the `pulp-rust-sys` the
//! spec generates with bindgen for real cores. To stay self-contained (no
//! libclang/bindgen build dependency in the opt-in test lane), it hand-mirrors
//! the handful of `#[repr(C)]` structs it actually exposes and types every
//! pass-through struct pointer as a raw pointer (identical ABI). The C++ side
//! validates the descriptor/param fields it reads, so any layout drift fails
//! loudly rather than silently.
//!
//! Must match core/native-components/include/pulp/native_components/native_core.h.

#![allow(non_camel_case_types)]
// The exported entry returns a pointer to a struct whose fields reference other
// repr(C) types kept private to the fixture. The C side is the only consumer, so
// the "private interface" is intentional, not a leak.
#![allow(private_interfaces)]

use core::ffi::{c_char, c_void};

const PULP_NATIVE_CORE_ABI_VERSION: u32 = 1;
const PULP_NATIVE_OK: i32 = 0;

// `*const c_char` makes a struct non-Sync, so wrap statics that hold raw
// pointers. The pointed-to data is always 'static (string literals), and the
// host only ever reads it, so sharing is sound.
#[repr(transparent)]
struct Sync_<T>(T);
unsafe impl<T> Sync for Sync_<T> {}

#[repr(C)]
struct pulp_native_descriptor_v1 {
    size: u32,
    abi_version: u32,
    id: *const c_char,
    id_len: usize,
    name: *const c_char,
    name_len: usize,
    vendor: *const c_char,
    vendor_len: usize,
    plugin_version: u32,
    capabilities: u32,
    default_input_bus_count: u32,
    default_output_bus_count: u32,
    latency_frames: u32,
    tail_frames: u32,
}

#[repr(C)]
struct pulp_native_param_v1 {
    size: u32,
    abi_version: u32,
    id: *const c_char,
    id_len: usize,
    id_hash: u64,
    name: *const c_char,
    name_len: usize,
    min_value: f64,
    max_value: f64,
    default_value: f64,
    flags: u32,
    step_count: u32,
}

// The vtable. Struct-pointer parameters are typed as raw pointers (same ABI as
// the concrete `const pulp_native_*` pointers in the header) so the fixture
// need not mirror every process/state struct.
#[repr(C)]
struct pulp_native_core_v1 {
    size: u32,
    abi_version: u32,
    descriptor: extern "C" fn() -> *const pulp_native_descriptor_v1,
    parameters: extern "C" fn(*mut u32) -> *const pulp_native_param_v1,
    create: extern "C" fn(*const c_void, *mut *mut c_void) -> i32,
    destroy: extern "C" fn(*mut c_void),
    prepare: extern "C" fn(*mut c_void, *const c_void) -> i32,
    release: extern "C" fn(*mut c_void),
    set_bus_layout: extern "C" fn(*mut c_void, *const c_void) -> i32,
    resume: extern "C" fn(*mut c_void) -> i32,
    suspend: extern "C" fn(*mut c_void) -> i32,
    reset: extern "C" fn(*mut c_void),
    process: extern "C" fn(*mut c_void, *const c_void) -> i32,
    save_state: extern "C" fn(*mut c_void, *mut c_void) -> i32,
    free_state: extern "C" fn(*mut c_void, *mut c_void),
    load_state: extern "C" fn(*mut c_void, *const c_void) -> i32,
    report_latency: extern "C" fn(*mut c_void) -> u32,
    report_tail: extern "C" fn(*mut c_void) -> u32,
    editor_command:
        extern "C" fn(*mut c_void, *const u8, usize, *mut *mut u8, *mut usize) -> i32,
    free_editor_reply: extern "C" fn(*mut c_void, *mut u8, usize),
}

// Capability bits (subset we advertise).
const CAP_STATE: u32 = 1 << 7;

const ID: &[u8] = b"com.pulp.example.noop-rust";
const NAME: &[u8] = b"Noop Rust Core";
const VENDOR: &[u8] = b"Pulp";
const PARAM_ID: &[u8] = b"gain";
// FNV-1a/64("gain"); see native_core.hpp param_id_hash. Pinned so a host that
// recomputes the hash agrees with this fixture.
const PARAM_GAIN_HASH: u64 = 0x8AE8_7E72_043D_203E;

static DESC: Sync_<pulp_native_descriptor_v1> = Sync_(pulp_native_descriptor_v1 {
    size: core::mem::size_of::<pulp_native_descriptor_v1>() as u32,
    abi_version: PULP_NATIVE_CORE_ABI_VERSION,
    id: ID.as_ptr() as *const c_char,
    id_len: ID.len(),
    name: NAME.as_ptr() as *const c_char,
    name_len: NAME.len(),
    vendor: VENDOR.as_ptr() as *const c_char,
    vendor_len: VENDOR.len(),
    plugin_version: 1,
    capabilities: CAP_STATE,
    default_input_bus_count: 1,
    default_output_bus_count: 1,
    latency_frames: 0,
    tail_frames: 0,
});

static PARAMS: Sync_<[pulp_native_param_v1; 1]> = Sync_([pulp_native_param_v1 {
    size: core::mem::size_of::<pulp_native_param_v1>() as u32,
    abi_version: PULP_NATIVE_CORE_ABI_VERSION,
    id: PARAM_ID.as_ptr() as *const c_char,
    id_len: PARAM_ID.len(),
    id_hash: PARAM_GAIN_HASH,
    name: core::ptr::null(),
    name_len: 0,
    min_value: -60.0,
    max_value: 12.0,
    default_value: 0.0,
    flags: 0b11, // automatable | modulatable
    step_count: 0,
}]);

// A trivial heap instance so create/destroy exercise real ownership across the
// boundary (decision 9: per-instance handle, no shared mutable globals).
struct NoopInstance {
    active: bool,
}

extern "C" fn descriptor() -> *const pulp_native_descriptor_v1 {
    &DESC.0
}

extern "C" fn parameters(out_count: *mut u32) -> *const pulp_native_param_v1 {
    if !out_count.is_null() {
        unsafe { *out_count = PARAMS.0.len() as u32 };
    }
    PARAMS.0.as_ptr()
}

extern "C" fn create(_host: *const c_void, out: *mut *mut c_void) -> i32 {
    if out.is_null() {
        return 2; // INVALID_ARGUMENT
    }
    let inst = Box::new(NoopInstance { active: false });
    unsafe { *out = Box::into_raw(inst) as *mut c_void };
    PULP_NATIVE_OK
}

extern "C" fn destroy(inst: *mut c_void) {
    if !inst.is_null() {
        unsafe { drop(Box::from_raw(inst as *mut NoopInstance)) };
    }
}

extern "C" fn prepare(_inst: *mut c_void, _cfg: *const c_void) -> i32 {
    PULP_NATIVE_OK
}
extern "C" fn release(_inst: *mut c_void) {}
extern "C" fn set_bus_layout(_inst: *mut c_void, _layout: *const c_void) -> i32 {
    PULP_NATIVE_OK
}
extern "C" fn resume(inst: *mut c_void) -> i32 {
    if let Some(i) = unsafe { (inst as *mut NoopInstance).as_mut() } {
        i.active = true;
    }
    PULP_NATIVE_OK
}
extern "C" fn suspend(inst: *mut c_void) -> i32 {
    if let Some(i) = unsafe { (inst as *mut NoopInstance).as_mut() } {
        i.active = false;
    }
    PULP_NATIVE_OK
}
extern "C" fn reset(_inst: *mut c_void) {}
// No-op process: passthrough/golden-audio is out of scope for this fixture.
// Here we only prove the call links and dispatches RT-safely (no alloc/log/panic).
extern "C" fn process(_inst: *mut c_void, _io: *const c_void) -> i32 {
    PULP_NATIVE_OK
}
extern "C" fn save_state(_inst: *mut c_void, _out: *mut c_void) -> i32 {
    PULP_NATIVE_OK
}
extern "C" fn free_state(_inst: *mut c_void, _out: *mut c_void) {}
extern "C" fn load_state(_inst: *mut c_void, _state: *const c_void) -> i32 {
    PULP_NATIVE_OK
}
extern "C" fn report_latency(_inst: *mut c_void) -> u32 {
    0
}
extern "C" fn report_tail(_inst: *mut c_void) -> u32 {
    0
}
extern "C" fn editor_command(
    _inst: *mut c_void,
    _req: *const u8,
    _req_len: usize,
    out_reply: *mut *mut u8,
    out_len: *mut usize,
) -> i32 {
    if !out_reply.is_null() {
        unsafe { *out_reply = core::ptr::null_mut() };
    }
    if !out_len.is_null() {
        unsafe { *out_len = 0 };
    }
    1 // UNSUPPORTED — this fixture has no editor commands
}
extern "C" fn free_editor_reply(_inst: *mut c_void, _reply: *mut u8, _len: usize) {}

static CORE: Sync_<pulp_native_core_v1> = Sync_(pulp_native_core_v1 {
    size: core::mem::size_of::<pulp_native_core_v1>() as u32,
    abi_version: PULP_NATIVE_CORE_ABI_VERSION,
    descriptor,
    parameters,
    create,
    destroy,
    prepare,
    release,
    set_bus_layout,
    resume,
    suspend,
    reset,
    process,
    save_state,
    free_state,
    load_state,
    report_latency,
    report_tail,
    editor_command,
    free_editor_reply,
});

/// The one symbol the host resolves.
#[no_mangle]
pub extern "C" fn pulp_native_core_entry_v1() -> *const pulp_native_core_v1 {
    &CORE.0
}

// ───────────────────────── RT-safety self-test support ─────────────────────
// Under `pulp_rt_check_allocator`, every Rust allocation first calls the host
// trap, which aborts if it fires inside a no-alloc scope. The C++ death-test
// enters a scope then calls pulp_noop_selftest_alloc() in a child process and
// asserts the child died.
#[cfg(feature = "pulp_rt_check_allocator")]
mod rt_check {
    use core::alloc::{GlobalAlloc, Layout};
    use std::alloc::System;

    extern "C" {
        fn pulp_rt_trap_if_no_alloc_scope(kind: i32, bytes: usize);
    }

    struct CheckingAlloc;
    // kind 2 == rust-global-alloc (see native_core.h).
    unsafe impl GlobalAlloc for CheckingAlloc {
        unsafe fn alloc(&self, l: Layout) -> *mut u8 {
            pulp_rt_trap_if_no_alloc_scope(2, l.size());
            System.alloc(l)
        }
        unsafe fn dealloc(&self, p: *mut u8, l: Layout) {
            System.dealloc(p, l);
        }
        unsafe fn realloc(&self, p: *mut u8, l: Layout, n: usize) -> *mut u8 {
            pulp_rt_trap_if_no_alloc_scope(2, n);
            System.realloc(p, l, n)
        }
    }

    #[global_allocator]
    static GLOBAL: CheckingAlloc = CheckingAlloc;
}

/// Deliberately allocate so the checking allocator fires. Used by the C++
/// death-test child while inside a no-alloc scope.
#[no_mangle]
pub extern "C" fn pulp_noop_selftest_alloc() {
    let mut v: Vec<u8> = Vec::new();
    v.reserve(64);
    v.push(1);
    core::hint::black_box(&v);
}
