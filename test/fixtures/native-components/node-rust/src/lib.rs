//! A Rust custom node implementing the public `pulp_node_v1` C ABI, used to
//! prove a C node and a Rust node load through the SAME contract. TEST FIXTURE
//! ONLY. Hand-mirrors the `#[repr(C)]` structs from
//! core/native-components/include/pulp/native_components/pulp_node_v1.h.

#![allow(non_camel_case_types)]
#![allow(private_interfaces)]

use core::ffi::{c_char, c_void};

const ABI_MAJOR: u32 = 1;
const OK: i32 = 0;
const ERR_MALFORMED_STATE: i32 = 5;
const CAP_STATE: u32 = 1 << 0;
const CAP_RESET: u32 = 1 << 1;

#[repr(transparent)]
struct Sync_<T>(T);
unsafe impl<T> Sync for Sync_<T> {}

#[repr(C)]
struct Descriptor {
    size: u32,
    abi_major: u32,
    stable_id: *const c_char,
    stable_id_len: usize,
    display_name: *const c_char,
    display_name_len: usize,
    node_version: u32,
    capability_flags: u32,
    audio_input_count: u32,
    audio_output_count: u32,
}

#[repr(C)]
struct Audio {
    size: u32,
    abi_major: u32,
    frame_count: u32,
    input_count: u32,
    output_count: u32,
    reserved: u32,
    inputs: *const *const f32,
    outputs: *const *mut f32,
}

#[repr(C)]
struct Writer {
    size: u32,
    abi_major: u32,
    writer_context: *mut c_void,
    write: extern "C" fn(*mut c_void, *const u8, usize),
}

#[repr(C)]
struct Entry {
    size: u32,
    abi_major: u32,
    descriptor: extern "C" fn() -> *const Descriptor,
    create: extern "C" fn(*const c_void, *mut *mut c_void) -> i32,
    destroy: extern "C" fn(*mut c_void),
    prepare: extern "C" fn(*mut c_void, *const c_void) -> i32,
    reset: extern "C" fn(*mut c_void),
    process: extern "C" fn(*mut c_void, *const Audio) -> i32,
    release: extern "C" fn(*mut c_void),
    save_state: extern "C" fn(*mut c_void, *const Writer) -> i32,
    load_state: extern "C" fn(*mut c_void, *const u8, usize) -> i32,
    report_latency: extern "C" fn(*mut c_void) -> u32,
}

struct GainNode {
    level: f32,
}

const ID: &[u8] = b"pulp.test.node.rust-gain";
const NAME: &[u8] = b"Rust Gain Node";

static DESC: Sync_<Descriptor> = Sync_(Descriptor {
    size: core::mem::size_of::<Descriptor>() as u32,
    abi_major: ABI_MAJOR,
    stable_id: ID.as_ptr() as *const c_char,
    stable_id_len: ID.len(),
    display_name: NAME.as_ptr() as *const c_char,
    display_name_len: NAME.len(),
    node_version: 1,
    capability_flags: CAP_STATE | CAP_RESET,
    audio_input_count: 1,
    audio_output_count: 1,
});

extern "C" fn descriptor() -> *const Descriptor {
    &DESC.0
}
extern "C" fn create(_host: *const c_void, out: *mut *mut c_void) -> i32 {
    if out.is_null() {
        return 2;
    }
    let inst = Box::new(GainNode { level: 1.0 });
    unsafe { *out = Box::into_raw(inst) as *mut c_void };
    OK
}
extern "C" fn destroy(i: *mut c_void) {
    if !i.is_null() {
        unsafe { drop(Box::from_raw(i as *mut GainNode)) };
    }
}
extern "C" fn prepare(_i: *mut c_void, _c: *const c_void) -> i32 {
    OK
}
extern "C" fn reset(i: *mut c_void) {
    if let Some(g) = unsafe { (i as *mut GainNode).as_mut() } {
        g.level = 1.0;
    }
}
extern "C" fn process(i: *mut c_void, a: *const Audio) -> i32 {
    let g = match unsafe { (i as *mut GainNode).as_ref() } {
        Some(g) => g,
        None => return OK,
    };
    let a = match unsafe { a.as_ref() } {
        Some(a) => a,
        None => return OK,
    };
    let ch = a.input_count.min(a.output_count) as usize;
    let frames = a.frame_count as usize;
    let ins = unsafe { core::slice::from_raw_parts(a.inputs, ch) };
    let outs = unsafe { core::slice::from_raw_parts(a.outputs, ch) };
    for c in 0..ch {
        let inp = unsafe { core::slice::from_raw_parts(ins[c], frames) };
        let outp = unsafe { core::slice::from_raw_parts_mut(outs[c], frames) };
        for s in 0..frames {
            outp[s] = inp[s] * g.level;
        }
    }
    OK
}
extern "C" fn release(_i: *mut c_void) {}
extern "C" fn save_state(i: *mut c_void, w: *const Writer) -> i32 {
    let g = unsafe { &*(i as *const GainNode) };
    let w = unsafe { &*w };
    let bytes = g.level.to_le_bytes();
    (w.write)(w.writer_context, bytes.as_ptr(), bytes.len());
    OK
}
extern "C" fn load_state(i: *mut c_void, bytes: *const u8, len: usize) -> i32 {
    if len != 4 {
        return ERR_MALFORMED_STATE;
    }
    let g = unsafe { &mut *(i as *mut GainNode) };
    let b = unsafe { core::slice::from_raw_parts(bytes, 4) };
    g.level = f32::from_le_bytes([b[0], b[1], b[2], b[3]]);
    OK
}
extern "C" fn report_latency(_i: *mut c_void) -> u32 {
    0
}

static ENTRY: Sync_<Entry> = Sync_(Entry {
    size: core::mem::size_of::<Entry>() as u32,
    abi_major: ABI_MAJOR,
    descriptor,
    create,
    destroy,
    prepare,
    reset,
    process,
    release,
    save_state,
    load_state,
    report_latency,
});

#[no_mangle]
pub extern "C" fn pulp_node_v1_entry() -> *const Entry {
    &ENTRY.0
}
