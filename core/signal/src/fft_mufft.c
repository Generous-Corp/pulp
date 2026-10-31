/* The muFFT side of FftT's portable float backend (see fft.hpp). One plan
 * holds the four 1-D plans a transform of size n needs plus two aligned
 * staging buffers. muFFT requires aligned, non-overlapping input and output;
 * FftT accepts any caller pointer and transforms in place, so it passes
 * caller buffers straight through when they qualify and stages through these
 * buffers when they do not. Everything is allocated in pulp_mufft_create();
 * the execute functions only run muFFT's plans, which allocate nothing.
 *
 * Every allocation the backend makes, here or inside muFFT, goes through the
 * counting wrappers at the end of this file (muFFT's sources are compiled
 * with fft_mufft_alloc_hooks.h force-included), so a test can prove the
 * transforms allocate nothing. */
#include "fft.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

void* pulp_mufft_malloc(size_t size);
void* pulp_mufft_calloc(size_t count, size_t size);
void* pulp_mufft_realloc(void* ptr, size_t size);
void pulp_mufft_free(void* ptr);

struct pulp_mufft_plan {
    mufft_plan_1d* forward;
    mufft_plan_1d* inverse;
    mufft_plan_1d* real_forward;
    mufft_plan_1d* real_inverse;
    float* in;
    float* out;
};

/* muFFT's kernels are radix 2/4/8 over powers of two; tiny sizes stay on the
 * built-in transform. */
enum { kPulpMufftMinSize = 16 };

static int pulp_mufft_supported(uint64_t n) {
    return n >= kPulpMufftMinSize && n <= (1u << 30) && (n & (n - 1u)) == 0u;
}

/* The widest aligned load the compiled kernels issue: AVX on x86-64 builds
 * with the SIMD kernels, otherwise the complex element itself. */
size_t pulp_mufft_alignment(void) {
#if defined(MUFFT_HAVE_AVX)
    return 32u;
#elif defined(MUFFT_HAVE_SSE)
    return 16u;
#else
    return 2u * sizeof(float);
#endif
}

uint64_t pulp_mufft_retained_bytes(uint64_t n) {
    if (!pulp_mufft_supported(n))
        return 0u;
    /* Per muFFT plan: the struct, twiddles and a temporary buffer of n
     * complex floats each, and the real plans' resolve twiddles; each muFFT
     * allocation can carry up to 64 bytes of alignment slack. Plus the two
     * staging buffers of 2n + 2 floats. A conservative upper bound. */
    const uint64_t complex_bytes = n * 8u;
    const uint64_t per_plan = 4096u + 3u * complex_bytes + 4u * 64u;
    return sizeof(struct pulp_mufft_plan) + 4u * per_plan + 2u * ((2u * n + 2u) * 4u + 64u);
}

/* The largest single allocation a plan of size n makes, as requested from
 * malloc: the staging buffers (2n + 2 floats) and the n-point twiddle and
 * temporary buffers are the biggest, each carrying muFFT's alignment slack of
 * up to 64 bytes plus a pointer. Admission compares this against its
 * per-allocation ceiling; pulp_mufft_retained_bytes() is the aggregate. */
uint64_t pulp_mufft_largest_allocation_bytes(uint64_t n) {
    if (!pulp_mufft_supported(n))
        return 0u;
    return (2u * n + 2u) * sizeof(float) + 64u + sizeof(void*);
}

void pulp_mufft_destroy(struct pulp_mufft_plan* plan) {
    if (!plan)
        return;
    if (plan->forward) mufft_free_plan_1d(plan->forward);
    if (plan->inverse) mufft_free_plan_1d(plan->inverse);
    if (plan->real_forward) mufft_free_plan_1d(plan->real_forward);
    if (plan->real_inverse) mufft_free_plan_1d(plan->real_inverse);
    if (plan->in) mufft_free(plan->in);
    if (plan->out) mufft_free(plan->out);
    pulp_mufft_free(plan);
}

struct pulp_mufft_plan* pulp_mufft_create(int n) {
    if (n <= 0 || !pulp_mufft_supported((uint64_t)n))
        return NULL;
    struct pulp_mufft_plan* plan = (struct pulp_mufft_plan*)pulp_mufft_calloc(1, sizeof(*plan));
    if (!plan)
        return NULL;
    const unsigned un = (unsigned)n;
    plan->forward = mufft_create_plan_1d_c2c(un, MUFFT_FORWARD, 0);
    plan->inverse = mufft_create_plan_1d_c2c(un, MUFFT_INVERSE, 0);
    plan->real_forward = mufft_create_plan_1d_r2c(un, 0);
    plan->real_inverse = mufft_create_plan_1d_c2r(un, 0);
    plan->in = (float*)mufft_alloc((2u * un + 2u) * sizeof(float));
    plan->out = (float*)mufft_alloc((2u * un + 2u) * sizeof(float));
    if (!plan->forward || !plan->inverse || !plan->real_forward || !plan->real_inverse ||
        !plan->in || !plan->out) {
        pulp_mufft_destroy(plan);
        return NULL;
    }
    return plan;
}

float* pulp_mufft_in(struct pulp_mufft_plan* plan) { return plan->in; }
float* pulp_mufft_out(struct pulp_mufft_plan* plan) { return plan->out; }

void pulp_mufft_complex(struct pulp_mufft_plan* plan, int forward, float* out, const float* in) {
    mufft_execute_plan_1d(forward ? plan->forward : plan->inverse, out, in);
}

void pulp_mufft_real_forward(struct pulp_mufft_plan* plan, float* out, const float* in) {
    mufft_execute_plan_1d(plan->real_forward, out, in);
}

void pulp_mufft_real_inverse(struct pulp_mufft_plan* plan, float* out, const float* in) {
    mufft_execute_plan_1d(plan->real_inverse, out, in);
}

/* Allocation accounting. Relaxed: a count, not a synchronisation point. */
#if defined(_MSC_VER)
#include <intrin.h>
static volatile __int64 pulp_mufft_allocation_count;
uint64_t pulp_mufft_allocations(void) {
    return (uint64_t)_InterlockedOr64(&pulp_mufft_allocation_count, 0);
}
static void pulp_mufft_count(void) {
    _InterlockedExchangeAdd64(&pulp_mufft_allocation_count, 1);
}
#else
static uint64_t pulp_mufft_allocation_count;
uint64_t pulp_mufft_allocations(void) {
    return __atomic_load_n(&pulp_mufft_allocation_count, __ATOMIC_RELAXED);
}
static void pulp_mufft_count(void) {
    __atomic_fetch_add(&pulp_mufft_allocation_count, 1u, __ATOMIC_RELAXED);
}
#endif

void* pulp_mufft_malloc(size_t size) {
    pulp_mufft_count();
    return malloc(size);
}

void* pulp_mufft_calloc(size_t count, size_t size) {
    pulp_mufft_count();
    return calloc(count, size);
}

void* pulp_mufft_realloc(void* ptr, size_t size) {
    pulp_mufft_count();
    return realloc(ptr, size);
}

void pulp_mufft_free(void* ptr) {
    free(ptr);
}
