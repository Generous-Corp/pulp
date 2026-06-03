//! RUIF-4 whole-screen Rust UI provider fixture.
//!
//! This intentionally emits a baked canonical DesignIR JSON fixture through the
//! draft native_ui.h-shaped ABI. C++ still validates and materializes the result;
//! Rust does not own View objects, renderer state, layout, windows, or plugin
//! editor lifecycle.

#![allow(non_camel_case_types)]
#![allow(private_interfaces)]

use core::ffi::c_void;

const ABI_VERSION: u32 = 1;
const OK: i32 = 0;
const ERR_INVALID_ARGUMENT: i32 = 2;

const PROVIDER_ID: &[u8] = b"test.rust.elysium";
const PROVIDER_VERSION: &[u8] = b"0.1.0";
const ELYSIUM_DESIGN_IR_JSON: &[u8] = include_bytes!(
    "../../../../../planning/artifacts/rust-ui/ruif-1/cpp-baseline/ir/elysium.designir.json"
);

#[repr(transparent)]
struct Sync_<T>(T);
unsafe impl<T> Sync for Sync_<T> {}

#[repr(C)]
#[derive(Clone, Copy)]
struct ByteSpan {
    bytes: *const u8,
    byte_len: usize,
}

impl ByteSpan {
    const fn empty() -> Self {
        Self {
            bytes: core::ptr::null(),
            byte_len: 0,
        }
    }

    fn from_static(bytes: &'static [u8]) -> Self {
        Self {
            bytes: bytes.as_ptr(),
            byte_len: bytes.len(),
        }
    }
}

#[repr(C)]
struct Request {
    size: u32,
    abi_version: u32,
    canonical_source_design_ir_json: ByteSpan,
    source_design_hash: ByteSpan,
    strict_mode: u32,
    reserved: u32,
}

#[repr(C)]
struct ResultV1 {
    size: u32,
    abi_version: u32,
    status: i32,
    reserved: u32,
    provider_id: ByteSpan,
    provider_version: ByteSpan,
    source_design_hash: ByteSpan,
    canonical_design_ir_json: ByteSpan,
    diagnostics_json: ByteSpan,
    failure_reason: ByteSpan,
    owned_result: *mut c_void,
}

#[repr(C)]
struct ProviderV1 {
    size: u32,
    abi_version: u32,
    provider_id: extern "C" fn() -> ByteSpan,
    provider_version: extern "C" fn() -> ByteSpan,
    import_design: extern "C" fn(*const Request, *mut ResultV1) -> i32,
    free_result: extern "C" fn(*mut ResultV1),
}

struct OwnedResult {
    source_design_hash: Vec<u8>,
}

extern "C" fn provider_id() -> ByteSpan {
    ByteSpan::from_static(PROVIDER_ID)
}

extern "C" fn provider_version() -> ByteSpan {
    ByteSpan::from_static(PROVIDER_VERSION)
}

extern "C" fn import_design(request: *const Request, out: *mut ResultV1) -> i32 {
    if out.is_null() {
        return ERR_INVALID_ARGUMENT;
    }
    if request.is_null() {
        unsafe {
            *out = failed_result();
        }
        return ERR_INVALID_ARGUMENT;
    }
    let request = unsafe { &*request };
    if request.size < core::mem::size_of::<Request>() as u32 || request.abi_version != ABI_VERSION {
        unsafe {
            *out = failed_result();
        }
        return ERR_INVALID_ARGUMENT;
    }

    let hash =
        if request.source_design_hash.bytes.is_null() || request.source_design_hash.byte_len == 0 {
            Vec::new()
        } else {
            let bytes = unsafe {
                core::slice::from_raw_parts(
                    request.source_design_hash.bytes,
                    request.source_design_hash.byte_len,
                )
            };
            bytes.to_vec()
        };
    let owned = Box::new(OwnedResult {
        source_design_hash: hash,
    });
    let owned_ptr = Box::into_raw(owned);
    let source_hash_span = unsafe {
        ByteSpan {
            bytes: (*owned_ptr).source_design_hash.as_ptr(),
            byte_len: (*owned_ptr).source_design_hash.len(),
        }
    };

    unsafe {
        *out = ResultV1 {
            size: core::mem::size_of::<ResultV1>() as u32,
            abi_version: ABI_VERSION,
            status: OK,
            reserved: 0,
            provider_id: ByteSpan::from_static(PROVIDER_ID),
            provider_version: ByteSpan::from_static(PROVIDER_VERSION),
            source_design_hash: source_hash_span,
            canonical_design_ir_json: ByteSpan::from_static(ELYSIUM_DESIGN_IR_JSON),
            diagnostics_json: ByteSpan::empty(),
            failure_reason: ByteSpan::empty(),
            owned_result: owned_ptr as *mut c_void,
        };
    }
    OK
}

extern "C" fn free_result(result: *mut ResultV1) {
    if result.is_null() {
        return;
    }
    let result = unsafe { &mut *result };
    if !result.owned_result.is_null() {
        unsafe {
            drop(Box::from_raw(result.owned_result as *mut OwnedResult));
        }
    }
    *result = empty_result();
}

fn empty_result() -> ResultV1 {
    ResultV1 {
        size: core::mem::size_of::<ResultV1>() as u32,
        abi_version: ABI_VERSION,
        status: OK,
        reserved: 0,
        provider_id: ByteSpan::empty(),
        provider_version: ByteSpan::empty(),
        source_design_hash: ByteSpan::empty(),
        canonical_design_ir_json: ByteSpan::empty(),
        diagnostics_json: ByteSpan::empty(),
        failure_reason: ByteSpan::empty(),
        owned_result: core::ptr::null_mut(),
    }
}

fn failed_result() -> ResultV1 {
    ResultV1 {
        status: ERR_INVALID_ARGUMENT,
        ..empty_result()
    }
}

static PROVIDER: Sync_<ProviderV1> = Sync_(ProviderV1 {
    size: core::mem::size_of::<ProviderV1>() as u32,
    abi_version: ABI_VERSION,
    provider_id,
    provider_version,
    import_design,
    free_result,
});

#[no_mangle]
pub extern "C" fn pulp_native_ui_entry_v1() -> *const ProviderV1 {
    &PROVIDER.0
}
