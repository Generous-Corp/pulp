/*
 * pulp/native_components/native_ui.h — Pulp Native UI Provider ABI (draft v1)
 *
 * Experimental, source-built contract for non-real-time UI recipe providers.
 * This is deliberately separate from native_core.h: native_core.h is the
 * Processor/DSP ABI, while this header is import/materialization-time UI
 * recipe production. Providers return canonical DesignIR JSON for C++ to
 * validate and materialize into Pulp-owned View objects.
 *
 * Rust/C/Zig providers must not own View instances, renderer state, platform
 * windows, plugin editor lifecycle, Yoga layout objects, or host assets. The
 * provider may allocate its result, but ownership is explicit: every successful
 * import result must be released with the provider's free_result callback.
 *
 * STATUS: trusted in-process provider ABI for source-built staticlibs. Raw
 * pointer/length spans are not a security boundary for arbitrary third-party
 * dynamic providers. A future public/plugin-ready contract may need host-owned
 * output buffers, two-pass sizing/copy APIs, dynamic-load policy, or process
 * isolation. This draft proves the recipe/materialization seam first.
 *
 * ABI rules:
 * - All structs are zero-initialized before use.
 * - Every struct `size` is set to sizeof(the exact struct).
 * - Every `abi_version` is PULP_NATIVE_UI_ABI_VERSION.
 * - Reserved fields must be zero.
 * - Boolean fields are fixed-width integers; strict_mode is 0 or 1.
 * - Empty spans are {NULL, 0}; non-empty spans are {non-NULL, len}.
 * - Returned spans are borrowed and valid only until free_result().
 * - The host copies all returned bytes before calling free_result().
 * - import_design() must initialize out_result before returning. If
 *   out_result.owned_result is non-NULL, the host calls free_result() exactly
 *   once, including for failure statuses.
 * - source_design_hash is opaque pass-through provenance in this draft. Hosts
 *   that use it for cache/materialization correctness must compare the returned
 *   span to the request hash before accepting provider output.
 */
#ifndef PULP_NATIVE_COMPONENTS_NATIVE_UI_H
#define PULP_NATIVE_COMPONENTS_NATIVE_UI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PULP_NATIVE_UI_ABI_VERSION 1u

typedef int32_t pulp_native_ui_status;
enum {
    PULP_NATIVE_UI_OK = 0,
    PULP_NATIVE_UI_ERR_UNSUPPORTED = 1,
    PULP_NATIVE_UI_ERR_INVALID_ARGUMENT = 2,
    PULP_NATIVE_UI_ERR_OUT_OF_MEMORY = 3,
    PULP_NATIVE_UI_ERR_VERSION_MISMATCH = 4,
    PULP_NATIVE_UI_ERR_MALFORMED_INPUT = 5,
    PULP_NATIVE_UI_ERR_INTERNAL = 6
};

typedef struct pulp_native_ui_byte_span_v1 {
    const uint8_t* bytes;
    size_t byte_len;
} pulp_native_ui_byte_span_v1;

typedef struct pulp_native_ui_provider_request_v1 {
    uint32_t size;
    uint32_t abi_version;
    pulp_native_ui_byte_span_v1 canonical_source_design_ir_json;
    /* Opaque canonical source hash/provenance supplied by the C++ host. */
    pulp_native_ui_byte_span_v1 source_design_hash;
    uint32_t strict_mode; /* 0 or 1 */
    uint32_t reserved;
} pulp_native_ui_provider_request_v1;

typedef struct pulp_native_ui_provider_result_v1 {
    uint32_t size;
    uint32_t abi_version;
    pulp_native_ui_status status;
    uint32_t reserved;

    pulp_native_ui_byte_span_v1 provider_id;
    pulp_native_ui_byte_span_v1 provider_version;
    pulp_native_ui_byte_span_v1 source_design_hash;
    pulp_native_ui_byte_span_v1 canonical_design_ir_json;
    pulp_native_ui_byte_span_v1 diagnostics_json;
    pulp_native_ui_byte_span_v1 failure_reason;

    /*
     * Provider-owned context for returned spans. The host treats all spans as
     * borrowed until free_result() is called, then discards them.
     */
    void* owned_result;
} pulp_native_ui_provider_result_v1;

typedef struct pulp_native_ui_provider_v1 {
    uint32_t size;
    uint32_t abi_version;

    pulp_native_ui_byte_span_v1 (*provider_id)(void);
    pulp_native_ui_byte_span_v1 (*provider_version)(void);

    pulp_native_ui_status (*import_design)(
        const pulp_native_ui_provider_request_v1* request,
        pulp_native_ui_provider_result_v1* out_result);

    void (*free_result)(pulp_native_ui_provider_result_v1* result);
} pulp_native_ui_provider_v1;

const pulp_native_ui_provider_v1* pulp_native_ui_entry_v1(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PULP_NATIVE_COMPONENTS_NATIVE_UI_H */
