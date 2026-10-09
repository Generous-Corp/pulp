# Floating-point contraction policy, pinned for every compiler.
#
# Contraction fuses a*b+c into one fused multiply-add, which rounds once
# instead of twice. Whether a compiler does it, and across how much code, is a
# default that differs by compiler: GCC contracts across statements
# (-ffp-contract=fast) in C++, because its ISO-mode "off" applies only to C;
# Clang and AppleClang contract only within one expression. On a target with
# FMA, every aarch64 core among them, GCC's default lets the same arithmetic
# reached through two code paths (block vs scalar, whole vs partitioned,
# left vs swapped right) round differently. The cross-platform-check nightly
# showed it on Linux ARM64 as eleven bit-exact DSP tests failing (run
# 37275414069) that pass on every Clang build.
#
# Numerics must not depend on a compiler's contraction default, so the policy
# is stated here rather than inherited:
#   * GCC:              -ffp-contract=off  (no contraction at all)
#   * Clang/AppleClang: -ffp-contract=on   (within one expression; their
#                                           default, now explicit)
#   * MSVC:             /fp:precise, its default, does not contract unless
#                       /fp:contract is given, so nothing is added.
# A target that owns a stricter contract (a cross-compiler byte golden) still
# overrides this target-locally; a target option comes after this one.
if(NOT MSVC)  # clang-cl takes MSVC's spelling and default
    add_compile_options(
        "$<$<COMPILE_LANG_AND_ID:C,GNU>:-ffp-contract=off>"
        "$<$<COMPILE_LANG_AND_ID:CXX,GNU>:-ffp-contract=off>"
        "$<$<COMPILE_LANG_AND_ID:C,AppleClang,Clang>:-ffp-contract=on>"
        "$<$<COMPILE_LANG_AND_ID:CXX,AppleClang,Clang>:-ffp-contract=on>"
        "$<$<COMPILE_LANG_AND_ID:OBJC,AppleClang,Clang>:-ffp-contract=on>"
        "$<$<COMPILE_LANG_AND_ID:OBJCXX,AppleClang,Clang>:-ffp-contract=on>")
endif()
