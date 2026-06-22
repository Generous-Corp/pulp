//! A real Rust stereo-gain DSP core implementing the Pulp native-component C
//! ABI, used to prove end-to-end native audio through the C++
//! NativeCoreProcessor adapter. TEST FIXTURE ONLY.
//!
//! Hand-mirrors the `#[repr(C)]` structs it reads in `process()` (audio buses,
//! the parameter-event view, the process bundle) plus the descriptor/param/
//! vtable. Must match
//! core/native-components/include/pulp/native_components/native_core.h.

#![allow(non_camel_case_types)]
#![allow(private_interfaces)]

use core::ffi::{c_char, c_void};

const ABI_VERSION: u32 = 1;
const OK: i32 = 0;
const ERR_MALFORMED_STATE: i32 = 5;
const CAP_STATE: u32 = 1 << 7;
const CAP_EDITOR_COMMAND: u32 = 1 << 9;
const PARAM_AUTOMATABLE: u32 = 1 << 0;
const EVENT_AUTOMATION: i32 = 0;

// FNV-1a/64("gain"); matches native_core.hpp param_id_hash and the host.
const GAIN_HASH: u64 = 0x8AE8_7E72_043D_203E;

#[repr(transparent)]
struct Sync_<T>(T);
unsafe impl<T> Sync for Sync_<T> {}

#[repr(C)]
struct Descriptor {
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
struct Param {
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

#[repr(C)]
struct AudioBus {
    size: u32,
    channel_count: u32,
    channels: *const *mut f32, // float* const*
}

#[repr(C)]
struct AudioIo {
    size: u32,
    abi_version: u32,
    frame_count: u32,
    input_bus_count: u32,
    output_bus_count: u32,
    sidechain_bus_count: u32,
    reserved: u32,
    inputs: *const AudioBus,
    outputs: *mut AudioBus,
    sidechains: *const AudioBus,
}

#[repr(C)]
struct ParamEvent {
    param_id_hash: u64,
    value: f64,
    sample_offset: u32,
    ramp_frames: u32,
    kind: i32,
    reserved: u32,
}

#[repr(C)]
struct ParamEventView {
    size: u32,
    abi_version: u32,
    events: *const ParamEvent,
    count: u32,
    capacity: u32,
    overflowed: u32,
    reserved: u32,
}

#[repr(C)]
struct ProcessV1 {
    size: u32,
    abi_version: u32,
    audio: *const AudioIo,
    params: *const ParamEventView,
    midi: *const c_void,
    context: *const c_void,
}

#[repr(C)]
struct Core {
    size: u32,
    abi_version: u32,
    descriptor: extern "C" fn() -> *const Descriptor,
    parameters: extern "C" fn(*mut u32) -> *const Param,
    create: extern "C" fn(*const c_void, *mut *mut c_void) -> i32,
    destroy: extern "C" fn(*mut c_void),
    prepare: extern "C" fn(*mut c_void, *const c_void) -> i32,
    release: extern "C" fn(*mut c_void),
    set_bus_layout: extern "C" fn(*mut c_void, *const c_void) -> i32,
    resume: extern "C" fn(*mut c_void) -> i32,
    suspend: extern "C" fn(*mut c_void) -> i32,
    reset: extern "C" fn(*mut c_void),
    process: extern "C" fn(*mut c_void, *const ProcessV1) -> i32,
    save_state: extern "C" fn(*mut c_void, *mut c_void) -> i32,
    free_state: extern "C" fn(*mut c_void, *mut c_void),
    load_state: extern "C" fn(*mut c_void, *const StateSpan) -> i32,
    report_latency: extern "C" fn(*mut c_void) -> u32,
    report_tail: extern "C" fn(*mut c_void) -> u32,
    editor_command:
        extern "C" fn(*mut c_void, *const u8, usize, *mut *mut u8, *mut usize) -> i32,
    free_editor_reply: extern "C" fn(*mut c_void, *mut u8, usize),
}

#[repr(C)]
struct StateOut {
    size: u32,
    abi_version: u32,
    state_version: u32,
    reserved: u32,
    bytes: *mut u8,
    byte_len: usize,
}

#[repr(C)]
struct StateSpan {
    size: u32,
    abi_version: u32,
    state_version: u32,
    reserved: u32,
    bytes: *const u8,
    byte_len: usize,
}

const ID: &[u8] = b"com.pulp.example.gain-rust";
const NAME: &[u8] = b"Rust Gain";
const VENDOR: &[u8] = b"Pulp";
const PARAM_ID: &[u8] = b"gain";

static DESC: Sync_<Descriptor> = Sync_(Descriptor {
    size: core::mem::size_of::<Descriptor>() as u32,
    abi_version: ABI_VERSION,
    id: ID.as_ptr() as *const c_char,
    id_len: ID.len(),
    name: NAME.as_ptr() as *const c_char,
    name_len: NAME.len(),
    vendor: VENDOR.as_ptr() as *const c_char,
    vendor_len: VENDOR.len(),
    plugin_version: 1,
    capabilities: CAP_STATE | CAP_EDITOR_COMMAND,
    default_input_bus_count: 1,
    default_output_bus_count: 1,
    latency_frames: 0,
    tail_frames: 0,
});

static PARAMS: Sync_<[Param; 1]> = Sync_([Param {
    size: core::mem::size_of::<Param>() as u32,
    abi_version: ABI_VERSION,
    id: PARAM_ID.as_ptr() as *const c_char,
    id_len: PARAM_ID.len(),
    id_hash: GAIN_HASH,
    name: core::ptr::null(),
    name_len: 0,
    min_value: 0.0,
    max_value: 2.0,
    default_value: 1.0,
    flags: PARAM_AUTOMATABLE,
    step_count: 0,
}]);

struct GainInstance {
    gain: f32,
}

extern "C" fn descriptor() -> *const Descriptor {
    &DESC.0
}
extern "C" fn parameters(out: *mut u32) -> *const Param {
    if !out.is_null() {
        unsafe { *out = 1 };
    }
    PARAMS.0.as_ptr()
}
extern "C" fn create(_h: *const c_void, out: *mut *mut c_void) -> i32 {
    if out.is_null() {
        return 2;
    }
    let inst = Box::new(GainInstance { gain: 1.0 });
    unsafe { *out = Box::into_raw(inst) as *mut c_void };
    OK
}
extern "C" fn destroy(i: *mut c_void) {
    if !i.is_null() {
        unsafe { drop(Box::from_raw(i as *mut GainInstance)) };
    }
}
extern "C" fn prepare(_i: *mut c_void, _c: *const c_void) -> i32 {
    OK
}
extern "C" fn release(_i: *mut c_void) {}
extern "C" fn set_bus_layout(_i: *mut c_void, _l: *const c_void) -> i32 {
    OK
}
extern "C" fn resume(_i: *mut c_void) -> i32 {
    OK
}
extern "C" fn suspend(_i: *mut c_void) -> i32 {
    OK
}
extern "C" fn reset(_i: *mut c_void) {}

// The real DSP: apply the automated gain to every channel. RT-safe — no
// allocation, no logging, no panics.
extern "C" fn process(inst: *mut c_void, io: *const ProcessV1) -> i32 {
    let self_ = match unsafe { (inst as *mut GainInstance).as_mut() } {
        Some(s) => s,
        None => return OK,
    };
    let proc = match unsafe { io.as_ref() } {
        Some(p) => p,
        None => return OK,
    };
    // Pull the latest automation value for our parameter.
    if let Some(view) = unsafe { proc.params.as_ref() } {
        if !view.events.is_null() {
            let events =
                unsafe { core::slice::from_raw_parts(view.events, view.count as usize) };
            for e in events {
                if e.param_id_hash == GAIN_HASH && e.kind == EVENT_AUTOMATION {
                    self_.gain = e.value as f32;
                }
            }
        }
    }
    let audio = match unsafe { proc.audio.as_ref() } {
        Some(a) => a,
        None => return OK,
    };
    let n_bus = audio.output_bus_count.min(audio.input_bus_count) as usize;
    let outs = unsafe { core::slice::from_raw_parts(audio.outputs, n_bus) };
    let ins = unsafe { core::slice::from_raw_parts(audio.inputs, n_bus) };
    let frames = audio.frame_count as usize;
    for b in 0..n_bus {
        let ch = outs[b].channel_count.min(ins[b].channel_count) as usize;
        let out_ch = unsafe { core::slice::from_raw_parts(outs[b].channels, ch) };
        let in_ch = unsafe { core::slice::from_raw_parts(ins[b].channels, ch) };
        for c in 0..ch {
            let o = unsafe { core::slice::from_raw_parts_mut(out_ch[c], frames) };
            let i = unsafe { core::slice::from_raw_parts(in_ch[c], frames) };
            for s in 0..frames {
                o[s] = i[s] * self_.gain;
            }
        }
    }
    OK
}

extern "C" fn save_state(inst: *mut c_void, out: *mut c_void) -> i32 {
    let self_ = unsafe { &*(inst as *const GainInstance) };
    let out = unsafe { &mut *(out as *mut StateOut) };
    let boxed: Box<[u8; 4]> = Box::new(self_.gain.to_le_bytes());
    out.byte_len = 4;
    out.state_version = 1;
    out.bytes = Box::into_raw(boxed) as *mut u8;
    OK
}
extern "C" fn free_state(_i: *mut c_void, out: *mut c_void) {
    let out = unsafe { &mut *(out as *mut StateOut) };
    if !out.bytes.is_null() {
        unsafe { drop(Box::from_raw(out.bytes as *mut [u8; 4])) };
        out.bytes = core::ptr::null_mut();
    }
}
extern "C" fn load_state(inst: *mut c_void, span: *const StateSpan) -> i32 {
    let self_ = unsafe { &mut *(inst as *mut GainInstance) };
    let span = unsafe { &*span };
    if span.bytes.is_null() || span.byte_len == 0 {
        self_.gain = 1.0;
        return OK;
    }
    if span.byte_len != 4 {
        return ERR_MALFORMED_STATE;
    }
    let bytes = unsafe { core::slice::from_raw_parts(span.bytes, 4) };
    self_.gain = f32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]);
    OK
}
extern "C" fn report_latency(_i: *mut c_void) -> u32 {
    0
}
extern "C" fn report_tail(_i: *mut c_void) -> u32 {
    0
}
// Non-RT domain logic in Rust: answer an editor command with JSON. Allocation
// is fine here (this is NOT the audio thread). The host copies the reply then
// calls free_editor_reply with the same (ptr, len).
extern "C" fn editor_command(
    inst: *mut c_void,
    _req: *const u8,
    req_len: usize,
    out_r: *mut *mut u8,
    out_n: *mut usize,
) -> i32 {
    if req_len == 0 {
        if !out_r.is_null() {
            unsafe { *out_r = core::ptr::null_mut() };
        }
        if !out_n.is_null() {
            unsafe { *out_n = 0 };
        }
        return 1; // UNSUPPORTED
    }
    let gain = unsafe { (inst as *const GainInstance).as_ref() }
        .map(|g| g.gain)
        .unwrap_or(1.0);
    let reply = format!(r#"{{"engine":"rust","gain":{}}}"#, gain);
    let boxed: Box<[u8]> = reply.into_bytes().into_boxed_slice();
    let len = boxed.len();
    let ptr = Box::into_raw(boxed) as *mut u8;
    if !out_r.is_null() {
        unsafe { *out_r = ptr };
    }
    if !out_n.is_null() {
        unsafe { *out_n = len };
    }
    OK
}
extern "C" fn free_editor_reply(_i: *mut c_void, r: *mut u8, n: usize) {
    if !r.is_null() {
        unsafe { drop(Box::from_raw(core::ptr::slice_from_raw_parts_mut(r, n))) };
    }
}

static CORE: Sync_<Core> = Sync_(Core {
    size: core::mem::size_of::<Core>() as u32,
    abi_version: ABI_VERSION,
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

#[no_mangle]
pub extern "C" fn pulp_native_core_entry_v1() -> *const Core {
    &CORE.0
}
