/*
 * pulp/native_components/pulp_node_v1.h — Pulp public node ABI, generation 1.
 *
 * The stable, language-neutral C contract a precompiled custom `SignalGraph`
 * node implements (Rust, C, Zig, generated DSP). Derived from the
 * source-level `CustomNodeType` contract (lifecycle + opaque state +
 * serialization) and frozen here as `pulp_node_v1`.
 *
 * SCOPE: custom `SignalGraph` nodes only. This is NOT the Processor-level FFI
 * (`native_core.h`), NOT a format-adapter replacement, and NOT a JS widget
 * callback surface. A node is a small DSP block with audio ports, opaque
 * versioned state, and an optional host-services handle.
 *
 * COMPATIBILITY: same-major promise. Within ABI major 1, the contract is
 * append-only — fields are added by growing a struct's leading `size`, behavior
 * by capability flags; existing fields, callback order, and semantics never
 * change. A host accepts any node whose `abi_major` matches and whose entry
 * `size` is at least the host's minimum. Loaders that find a higher minor simply
 * ignore trailing fields they do not know.
 *
 * REPRESENTATION RULES (binding):
 *   - POD only. No C++ types, templates, exceptions, RTTI, virtuals,
 *     `std::function`, or references cross this boundary.
 *   - Every versioned struct leads with `uint32_t size` then `uint32_t abi_major`.
 *   - Integers are little-endian. Structs use natural C alignment; a `reserved`
 *     field pads to 8-byte alignment where noted and MUST be zero.
 *   - Strings are UTF-8, passed as `ptr` + `byte_len`; NUL termination is NOT
 *     required. `ptr==NULL,len==0` is the empty string; `ptr==NULL,len>0` is
 *     invalid. String lifetime: descriptor/port strings are owned by the node
 *     and valid for the node's whole load; per-call strings are valid only for
 *     that call.
 *   - The host owns all audio buffers and borrows them to the node for the
 *     duration of one `process` call; the node never retains or frees them.
 *   - Status codes, never error returns by sentinel. No callback allocates or
 *     blocks on the audio thread.
 */
#ifndef PULP_NATIVE_COMPONENTS_PULP_NODE_V1_H
#define PULP_NATIVE_COMPONENTS_PULP_NODE_V1_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef PULP_NODE_V1_EXPORT
#define PULP_NODE_V1_EXPORT
#endif

/* ───────────────────────── ABI version ───────────────────────── */

#define PULP_NODE_V1_ABI_MAJOR 1u
/* Minor bumps are additive only (new trailing fields / new capability bits). */
#define PULP_NODE_V1_ABI_MINOR 0u

/* ───────────────────────── Status codes ──────────────────────── */

typedef int32_t pulp_node_status_v1;
enum {
    PULP_NODE_OK_V1 = 0,
    PULP_NODE_ERR_UNSUPPORTED_V1 = 1,
    PULP_NODE_ERR_INVALID_ARGUMENT_V1 = 2,
    PULP_NODE_ERR_OUT_OF_MEMORY_V1 = 3,
    PULP_NODE_ERR_INVALID_STATE_V1 = 4,
    PULP_NODE_ERR_MALFORMED_STATE_V1 = 5,
    PULP_NODE_ERR_VERSION_MISMATCH_V1 = 6,
    PULP_NODE_ERR_INTERNAL_V1 = 7
};

/* ───────────────────────── Capability flags ──────────────────────────
 * Additive negotiation: optional behavior is a bit, never a required field. */
typedef uint32_t pulp_node_caps_v1;
enum {
    PULP_NODE_CAP_STATE_V1 = 1u << 0,   /* implements save_state / load_state  */
    PULP_NODE_CAP_RESET_V1 = 1u << 1,   /* implements reset                    */
    PULP_NODE_CAP_EVENTS_V1 = 1u << 2,  /* consumes the event port (extension) */
    PULP_NODE_CAP_LATENCY_V1 = 1u << 3  /* reports a non-zero latency          */
};

/* ───────────────────────── Opaque handles ────────────────────── */

typedef struct pulp_node_instance_v1 pulp_node_instance_v1; /* node-owned */
typedef struct pulp_node_host_v1 pulp_node_host_v1;         /* host-owned */

/* ───────────────────────── Host services (decision: explicit RT labels) ──
 * The host hands this to the node at create(). Each callback is labeled
 * RT-callable or NON-RT-ONLY; calling a NON-RT-ONLY callback from process() is a
 * contract violation. `host_context` is opaque and passed back to each. */
typedef struct pulp_node_host_services_v1 {
    uint32_t size;
    uint32_t abi_major;
    void* host_context;

    /* NON-RT-ONLY. Allocate / free host memory for state side-channels. The node
     * must not pass these pointers to its own allocator, or vice versa. */
    void* (*alloc)(void* host_context, size_t bytes);
    void (*free)(void* host_context, void* ptr);
    /* NON-RT-ONLY. Diagnostic log (UTF-8 ptr+len). Never call from process(). */
    void (*log)(void* host_context, int32_t level, const char* utf8, size_t len);
    /* RT-callable. Monotonic nanosecond clock for profiling; allocation-free. */
    uint64_t (*now_ns)(void* host_context);
} pulp_node_host_services_v1;

/* ───────────────────────── Descriptor ────────────────────────── */

typedef struct pulp_node_descriptor_v1 {
    uint32_t size;
    uint32_t abi_major;
    const char* stable_id; /* UTF-8 reverse-DNS-ish id; ptr+len */
    size_t stable_id_len;
    const char* display_name;
    size_t display_name_len;
    uint32_t node_version;     /* node-defined, distinct from the ABI version */
    pulp_node_caps_v1 capability_flags;
    uint32_t audio_input_count;  /* number of mono audio input ports  */
    uint32_t audio_output_count; /* number of mono audio output ports */
} pulp_node_descriptor_v1;

/* ───────────────────────── Audio (host-owned, borrowed) ───────────────
 * Planar float32. `channels[c]` points to `frame_count` contiguous floats.
 * Channel pointers are never NULL when the count is > 0. In-place processing
 * (an output aliasing an input) is legal; the node must tolerate it. */
typedef struct pulp_node_audio_v1 {
    uint32_t size;
    uint32_t abi_major;
    uint32_t frame_count;
    uint32_t input_count;
    uint32_t output_count;
    uint32_t reserved; /* must be 0 (8-byte alignment) */
    const float* const* inputs;  /* input_count planar pointers, read-only  */
    float* const* outputs;       /* output_count planar pointers, written   */
} pulp_node_audio_v1;

/* ───────────────────────── prepare() config ───────────────────────────
 * Any sample-rate or block-size change is a fresh prepare(); the node must
 * not assume a fixed configuration across prepare() calls. */
typedef struct pulp_node_prepare_v1 {
    uint32_t size;
    uint32_t abi_major;
    double sample_rate;
    uint32_t max_block_size;
    uint32_t reserved; /* must be 0 */
} pulp_node_prepare_v1;

/* ───────────────────────── State writer (decision: bounded ownership) ──
 * save_state pushes its bytes through this host-provided writer instead of
 * allocating across the boundary. The host owns the writer; the node calls
 * `write(ctx, bytes, len)` zero or more times. RT-illegal (NON-RT-ONLY). */
typedef struct pulp_node_writer_v1 {
    uint32_t size;
    uint32_t abi_major;
    void* writer_context;
    void (*write)(void* writer_context, const uint8_t* bytes, size_t len);
} pulp_node_writer_v1;

/* ───────────────────────── The node vtable / entry ────────────────────
 * A node binary exports ONE symbol, `pulp_node_v1_entry`, returning a const
 * pointer to this table. The host checks `abi_major` + `size`, reads the
 * descriptor, then drives the lifecycle. Growth is additive via `size`. */
typedef struct pulp_node_entry_v1 {
    uint32_t size;
    uint32_t abi_major;

    /* Static identity. */
    const pulp_node_descriptor_v1* (*descriptor)(void);

    /* Lifecycle. create() returns a fresh instance (NON-RT). The host-services
     * pointer stays valid for the instance's lifetime. */
    pulp_node_status_v1 (*create)(const pulp_node_host_services_v1* host,
                                  pulp_node_instance_v1** out_instance);
    void (*destroy)(pulp_node_instance_v1* instance);

    /* NON-RT. Allocate scratch for this configuration. */
    pulp_node_status_v1 (*prepare)(pulp_node_instance_v1* instance,
                                   const pulp_node_prepare_v1* config);
    /* RT-callable. Clear runtime history without reallocating
     * (PULP_NODE_CAP_RESET_V1). May be NULL. */
    void (*reset)(pulp_node_instance_v1* instance);
    /* RT-callable. The audio callback. Host owns buffers. No alloc / lock / log
     * / IO / panic. On failure, return a status and leave outputs silent. */
    pulp_node_status_v1 (*process)(pulp_node_instance_v1* instance,
                                   const pulp_node_audio_v1* audio);
    /* NON-RT. Release scratch (symmetric with prepare). May be NULL. */
    void (*release)(pulp_node_instance_v1* instance);

    /* NON-RT, optional (PULP_NODE_CAP_STATE_V1). Opaque versioned state via the
     * host writer; load validates-before-commit and never unwinds on malformed
     * input. May be NULL. */
    pulp_node_status_v1 (*save_state)(pulp_node_instance_v1* instance,
                                      const pulp_node_writer_v1* writer);
    pulp_node_status_v1 (*load_state)(pulp_node_instance_v1* instance,
                                      const uint8_t* bytes, size_t byte_len);

    /* NON-RT, optional (PULP_NODE_CAP_LATENCY_V1). Latency in frames. May be
     * NULL (treated as 0). */
    uint32_t (*report_latency)(pulp_node_instance_v1* instance);
} pulp_node_entry_v1;

/* ───────────────────────── Symbol / version handshake ─────────────────
 * The ONE exported symbol. A node binary defines it; the host resolves it,
 * checks abi_major + size, and refuses on mismatch. Referencing the symbol
 * directly lets a static node link without --whole-archive. */
PULP_NODE_V1_EXPORT const pulp_node_entry_v1* pulp_node_v1_entry(void);

/* Convenience: the ABI major the host was built against. A node can compare
 * against the entry it returns; the host compares against the node's. */
static inline uint32_t pulp_node_v1_abi_major(void) { return PULP_NODE_V1_ABI_MAJOR; }

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PULP_NATIVE_COMPONENTS_PULP_NODE_V1_H */
