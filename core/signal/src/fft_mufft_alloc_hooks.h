/* Force-included into muFFT's sources (see core/signal/CMakeLists.txt) so
 * every allocation muFFT makes is counted by fft_mufft.c. stdlib.h is
 * included first, so its declarations keep their names; only the calls in
 * muFFT's code are redirected. muFFT's aligned allocator is steered to its
 * portable malloc-based branch, which these wrappers then see. */
#ifndef PULP_FFT_MUFFT_ALLOC_HOOKS_H
#define PULP_FFT_MUFFT_ALLOC_HOOKS_H

#include <stddef.h>
#include <stdlib.h>

void* pulp_mufft_malloc(size_t size);
void* pulp_mufft_calloc(size_t count, size_t size);
void* pulp_mufft_realloc(void* ptr, size_t size);
void pulp_mufft_free(void* ptr);

#undef _ISOC11_SOURCE
#undef _POSIX_C_SOURCE
#undef _XOPEN_SOURCE
#define malloc(size) pulp_mufft_malloc(size)
#define calloc(count, size) pulp_mufft_calloc(count, size)
#define realloc(ptr, size) pulp_mufft_realloc(ptr, size)
#define free(ptr) pulp_mufft_free(ptr)

#endif
