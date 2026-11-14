// FftT<double> against a committed golden at N = 128.
//
// FftT<double> always runs the built-in radix-2 transform, so this is the
// arithmetic a caller gets when it pins one backend. The input is exact (a
// small integer divided by 2^23), so only the transform's own rounding is
// under test.
//
// The golden was generated on arm64 macOS, and there it is compared bit for
// bit. On x86-64 Linux the same source gives 24 of 256 forward and 20 of 256
// inverse values that differ in the last bits (at most 1.8e-15 absolute against
// bins up to about 16), so the transform is NOT bit-identical across platforms:
// compilers and libm versions round the twiddles and the fused multiply-adds
// differently. Everywhere else the test therefore holds the result to a
// relative bound of 1e-13 of the largest bin, three orders of magnitude above
// the measured drift and far below any scaling, ordering or twiddle mistake.
// A caller that needs identical bytes across platforms cannot get them from this
// transform alone; quantise after the comparison, not before.
//
// Regenerate (only for an intended arithmetic change) by running the same
// recurrence and writing the bit patterns of forward() and of inverse() of
// the forward result, on arm64 macOS.
#include <catch2/catch_test_macros.hpp>
#include <pulp/signal/fft.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <complex>
#include <cstdint>
#include <limits>
#include <sstream>
#include <vector>

using namespace pulp::signal;

namespace {

constexpr int kN = 128;

// Bit-exact only where the golden was generated, unless forced off to test the
// tolerant branch.
#if defined(__APPLE__) && defined(__aarch64__) && !defined(PULP_FFT_GOLDEN_TOLERANT)
constexpr bool kBitExact = true;
#else
constexpr bool kBitExact = false;
#endif
constexpr double kRelativeBound = 1.0e-13;

constexpr std::uint64_t kForward[256] = {
    0xbff13a7300000000ull, 0xc01caa9040000000ull, 0xc000cb7e237def0aull, 0x402858fb58b33b2eull,
    0xc023b171eaf7e04cull, 0xbfc7c5d5d75578c0ull, 0x3ff806adadfcf9d9ull, 0xc0207a0bd35ace44ull,
    0x4020f85cb8e7fd78ull, 0xbfe248316cd0f0b8ull, 0x402498baaf1528c4ull, 0xbfde562d1f6a03f4ull,
    0xc003aa910262e0a2ull, 0x40247784d24ee864ull, 0xc01b29b146c4daeaull, 0x400fba8582780f12ull,
    0x402de1c2b4dd1292ull, 0x40026b819a45a52eull, 0x3fecbdc7cf007d48ull, 0xc00d59b856e20f72ull,
    0xc000a6aafb4ddda9ull, 0xc00068a132862a36ull, 0x3ff64aef7ca24aa2ull, 0xbfd7241859123f20ull,
    0x3ff7c0778597db6full, 0xbfdadc11441382f4ull, 0x4012d1238127c170ull, 0xc004e9b365f48d70ull,
    0x40278232af55ccebull, 0xbffeda26bc7418eaull, 0xc012c917bed3a456ull, 0x4025f1506602c4c8ull,
    0xc025bb8490a4df66ull, 0xbfc858b42ddaae90ull, 0xc020135cd1904f85ull, 0x3fcc05949e282480ull,
    0xbffe67a2be085c34ull, 0x4020f7b10903168aull, 0xc0114b1393638d52ull, 0x4010344e745e745eull,
    0xbff035147f32ef98ull, 0x4030f107079d4ac1ull, 0x4024d1f70fd4c346ull, 0x4005b46080a10252ull,
    0x4029b32116255170ull, 0xc00b4f9477c2ceb6ull, 0x3fe4e309d7a5a940ull, 0x4002a701f983b5b4ull,
    0xc0022edd71c9a9e4ull, 0xbfd2444bfd3232fcull, 0x3ffcb158e2c0c82cull, 0x4020b3120f5c869cull,
    0xc016b05d202b5115ull, 0xbff4fcbc50127d82ull, 0xc02043b3c40875dbull, 0x401304875754db63ull,
    0x4015cb8f86250477ull, 0x401c28e504ab6a9cull, 0xc013e1b66c1ccedeull, 0xc022d031b8f83b80ull,
    0x4028be328ad836e6ull, 0x4004bb52efdb8ee0ull, 0x3fd4f9e10cc6498cull, 0xc003c4f7f5408c20ull,
    0x401a2ab428000000ull, 0xc027811344000000ull, 0xbfdc0f354701e364ull, 0x40055757a907fe90ull,
    0x4001e5a42196aa0dull, 0x3fe4155033411844ull, 0x400be4528a973abaull, 0xc0118f2f7d28b4aeull,
    0x401cb39d74a5f6e2ull, 0x3fe908b705306e4eull, 0xbfd8a522b4d2ece0ull, 0xc01dc8152caeaacaull,
    0xbffa6f246e3ede26ull, 0xc02492970037f0baull, 0x40202fdf7939d59cull, 0xc02b714ed51c7394ull,
    0xc00ee3ca192356deull, 0x40019764887c5124ull, 0x402e7aa8b50ebf86ull, 0x3ffdef607afd8eb6ull,
    0x401adfc063deb2feull, 0xbfe93a8493b51a6bull, 0xc024352ca3fde6b2ull, 0xc0256407e6ff1eceull,
    0xc000ad9dfad573d1ull, 0xc001567373b7e2b6ull, 0x4018b429e9ae855cull, 0x3fd0886fc299e7a8ull,
    0x3fdf65eaf14555d0ull, 0x4015e5e871aac040ull, 0x40126752cdfc1e1bull, 0x401293e950ce6e77ull,
    0x401d12782d5e24b9ull, 0x4013b20d4b72fe5aull, 0xc01af3bdcd3bbefaull, 0xc0123154a9ddc7e3ull,
    0x3ffaba7ec02e44acull, 0x3fe00c81731f9f80ull, 0x40046e3e5abb2a73ull, 0xbff5b5cd1f76fdd4ull,
    0xc01d1ad7fc354d6aull, 0xbff15f515a2ad323ull, 0x4034fa3f4a3dfbc3ull, 0xc006f519614beed3ull,
    0x401cf0e48c13aa05ull, 0xbfef7c1f86c68850ull, 0xc02304f852bfe846ull, 0x3fb941f8715dd5c0ull,
    0x3ffe938fa3252c92ull, 0x3fdd8ea18163dea0ull, 0x3fe1fc387ed0efd0ull, 0xc008e1e7851eb405ull,
    0xc01825f03031787dull, 0xc01624ab988d6dfbull, 0x3ff352e22fb11cc8ull, 0xc010baa8789f1690ull,
    0xc021aa8b392c2f86ull, 0xbfc8572b7bd6fa20ull, 0xc022367290db13d1ull, 0xbff5e3b497042ad1ull,
    0xc023802a5ab20971ull, 0xc011f1dadd38891aull, 0x4008e7e5141071d6ull, 0x400dc57565be0753ull,
    0xc021907ce8000000ull, 0xc019eb27c0000000ull, 0x4018fa61487a7733ull, 0x401f1bcd2b5da72cull,
    0xbff941c12deb7c5cull, 0x3fff7d88bae8f8baull, 0xc002060183e59a52ull, 0xc0195519dc693f56ull,
    0xc012a5fb39a797e0ull, 0xc029f81cbfdb8daeull, 0x4002b0536502636aull, 0xbfcb78555fcc15f8ull,
    0x3ff75231f6de7773ull, 0x3ffc6c693d47d544ull, 0x3ff1dd979b851e72ull, 0x3ffed1148e183114ull,
    0x401b86d98c2a497dull, 0xc01419fbc573137dull, 0x3ff3b7a5b01ee084ull, 0xbfec3c431dd7d1b2ull,
    0x40007b4568db3b6bull, 0x3ffadb8a18bb11a5ull, 0x400581aa66c2b137ull, 0x400a53b0f92d3eb0ull,
    0x400f4e4bd83dda1aull, 0x3fffbd5f1b7c628bull, 0xc00d1a67acb9fbe9ull, 0x40307b077e0980d0ull,
    0x40154cb5cc4fcf5aull, 0xbfc96798e46d55d0ull, 0x402509352ba31aeeull, 0x3fe7aed597abf448ull,
    0xc0114c2d5eb64134ull, 0x401dbdfab16ed574ull, 0x400ebc824c2a8004ull, 0x401bd6576ed43863ull,
    0xc02d33543850a96cull, 0xbfbd4ee2e4be2e00ull, 0x401a70e797af6daeull, 0xbff97c5062ae707eull,
    0xc02a397c1534429bull, 0x40204f18c617175eull, 0x40121a8cf8401202ull, 0x4002406be705c07cull,
    0x4014487057f2f4a5ull, 0x400005bec6acc06eull, 0xc0143984cd4b43fcull, 0xc02a34c9d16d3729ull,
    0x40266aae1538b9d6ull, 0x4008336cf5495360ull, 0x4012e11fd6e9fb95ull, 0xc014b13dc1c31f44ull,
    0xc00b7e319f0c7472ull, 0x3fe8620cde2c3a3cull, 0x401c46b52e611ff6ull, 0xc023137ce0f0508aull,
    0xc0274bcfad5f06f2ull, 0x4026436477ab5e7aull, 0xc01898795293b8baull, 0x40321e7b62625655ull,
    0x4001351b9be35182ull, 0x40159a2874660d75ull, 0x3fd0f6bbd38f7934ull, 0xc02a8451d04cf0e4ull,
    0xbfd74a8b80000008ull, 0x3ffe2a2ea0000000ull, 0x40107e86f1f4a610ull, 0x3fe7ea2876188a01ull,
    0x4013315e06c03d4eull, 0x401d6bc7bfa0166aull, 0x3ffdc91f228748b3ull, 0x40255d4ad67ba0dcull,
    0xc0190c5e4752d2f6ull, 0xbfe132ffc9d6343eull, 0xc00dd3af71744a64ull, 0xc0064093156b8294ull,
    0xbfe545040b6ffcb4ull, 0xbffca20fa69d4914ull, 0xc014e99516d3e016ull, 0xc01e88f89751d90full,
    0x401d6cda36ad3ccfull, 0x4011bd4fd4121854ull, 0xc010d81a42356033ull, 0x3fe3041b0dd22194ull,
    0xc0177bf2ae195ee6ull, 0x3ff61e2b5ea7c4b8ull, 0xc01f8d120aeaa9c5ull, 0x3fe02d2f6af7bae0ull,
    0x4012381ecccbe7aeull, 0x40200d5622c159aaull, 0x401d4c92b95589a4ull, 0x4018e73235bcc5acull,
    0xbfe3061d0dd37ff8ull, 0xc013a3ae21ea0f2eull, 0x403317362790d683ull, 0xc0137b2dd38948e1ull,
    0x402dbd6cd950eda4ull, 0x4026ba61224680d3ull, 0x40220970a5240fc3ull, 0xbff1df5018ddbc20ull,
    0x4022012410734e54ull, 0x402b35b88d68e606ull, 0x4024ed549a42b9b5ull, 0xc029aa49c2c4a72cull,
    0xc01eab5bdf10f876ull, 0xc003449b9c65658eull, 0xc014150cb23975f5ull, 0x4017732260def834ull,
    0xbffa3ee5ce6440f4ull, 0x3fe981ce7b5d1364ull, 0x401051849c335d15ull, 0x400e861ccce07acaull,
    0x40210a51a2d50b11ull, 0xc02979d7d973e235ull, 0x4015b89347050c42ull, 0xc0015d8588bc54adull,
    0x40136fc10c674a3full, 0x4018669548fcc8b5ull, 0xc009c994809fd42cull, 0x3fc1dc0c66ab1cf0ull,
    0xbfcbe3f2be952660ull, 0x40148cee6997edd6ull, 0x3fe3f668760aa290ull, 0x400df83ff8a32490ull,
    0x4000be3c3a580c23ull, 0x4006f1010f082ee2ull, 0xc01e444d0c11a39full, 0x40069cddaab4a87dull,
};
constexpr std::uint64_t kRoundTrip[256] = {
    0x3fee9e82c0000000ull, 0x3fdef84d80000001ull, 0x3fec5f5a80000000ull, 0x3fe5145fffffffffull,
    0xbfed2f2280000000ull, 0xbfe0201900000002ull, 0x3fc19c2b00000002ull, 0xbfeb299640000000ull,
    0xbfdb930c7ffffffeull, 0xbfd5e54900000002ull, 0x3fc1530c00000000ull, 0x3fec0d3c3fffffffull,
    0xbfe922d780000000ull, 0x3febfe2140000000ull, 0xbfa9c83800000006ull, 0x3fcfb5f200000013ull,
    0x3fb97a55fffffffcull, 0x3fefbf2540000000ull, 0xbfb2b5fffffffff5ull, 0xbfeed46040000000ull,
    0xbfc02cb9fffffffbull, 0xbfee825b40000000ull, 0xbfe33fd97fffffffull, 0xbfe76f153fffffffull,
    0x3fd3c87c00000000ull, 0x3fe2e42c7fffffffull, 0x3fd4feb1fffffffeull, 0x3fd4c0e500000002ull,
    0xbfd878be00000004ull, 0x3fefc7997ffffffeull, 0xbfd477de7ffffff9ull, 0x3fdb183800000000ull,
    0x3fe2199b80000000ull, 0xbfebdbe700000000ull, 0xbfcad5a5fffffffeull, 0xbfec466380000001ull,
    0x3fed6e5ebfffffffull, 0x3fd6d44effffffffull, 0x3fe0974340000000ull, 0xbf9d10effffffff0ull,
    0xbfe7e35480000000ull, 0x3fc86e4a00000000ull, 0xbfe1107ffffffffaull, 0xbfecfdfa40000000ull,
    0xbfd69a5a80000001ull, 0x3fe2b87700000000ull, 0xbfee9d3a00000002ull, 0xbfd70f2f00000000ull,
    0xbfe61c4a80000000ull, 0xbfe903be80000000ull, 0xbfd638c400000000ull, 0x3fef6195ffffffffull,
    0xbfd5f07600000008ull, 0x3fe66aaf7ffffffeull, 0x3fe970a0ffffffffull, 0xbfd4bd5900000004ull,
    0xbfbe8ff1fffffffeull, 0xbfecd33240000001ull, 0x3fd69af000000004ull, 0xbfb8165200000008ull,
    0x3fe0bcfd40000000ull, 0xbfd3840d80000002ull, 0x3feb4bdc3ffffffeull, 0xbfde5cbc00000004ull,
    0xbfec5c2780000000ull, 0x3fd7d1be00000001ull, 0xbf9b6d3000000020ull, 0x3fd92d997fffffffull,
    0x3fd83687fffffffbull, 0xbfdcd7077fffffffull, 0xbfed1f803ffffffdull, 0x3fc0d6ee00000002ull,
    0x3faf681400000000ull, 0x3fae11dc00000007ull, 0x3fc14603fffffffcull, 0x3fef2d4300000000ull,
    0xbfbbdcf800000010ull, 0x3fec156080000001ull, 0xbfdcc8d900000002ull, 0xbfc1e3de00000000ull,
    0xbfd1ff7780000002ull, 0xbfec24ce3fffffffull, 0x3fe3cdc000000000ull, 0x3fdb3900fffffffbull,
    0xbfe46ce3c0000000ull, 0xbfe9b33200000000ull, 0xbfa74e0800000016ull, 0x3faabf0800000008ull,
    0xbfea1b967fffffffull, 0x3fe151c2c0000001ull, 0x3fdb87b600000002ull, 0xbfd7852580000000ull,
    0x3fee3b3d7ffffffeull, 0xbfc6d04bfffffffcull, 0xbfe581d440000000ull, 0x3fc95f6f00000006ull,
    0x3fee2f3940000000ull, 0x3fa1479400000006ull, 0x3fefaf5a7fffffffull, 0xbfed7a0ec0000001ull,
    0xbf6672bfffffff80ull, 0xbfe3b71affffffffull, 0xbfcf6cfeffffffeeull, 0x3fe7ab3280000002ull,
    0xbfe70a0500000000ull, 0xbfe4180300000000ull, 0xbfe18339ffffffffull, 0xbfa570b80000000cull,
    0xbfdcb8597ffffffeull, 0xbf982437ffffffb8ull, 0x3fd05fca00000002ull, 0xbfe02ca3c0000000ull,
    0xbfc82423fffffffcull, 0x3fd64beb80000002ull, 0x3fc2a697fffffff8ull, 0x3fed365f00000000ull,
    0xbfb6b943fffffff8ull, 0xbfb38ff9fffffff8ull, 0x3fe971a1fffffffeull, 0xbfd2aa7d00000002ull,
    0x3fc2e5d600000004ull, 0xbfe4a2f400000001ull, 0x3fc1920900000001ull, 0x3fd976317ffffffeull,
    0xbfe3ef9e00000002ull, 0xbfea648b7ffffffeull, 0x3fd9f9fe80000004ull, 0xbfd14e6c80000001ull,
    0xbfd4a48400000000ull, 0x3fe2f0e780000000ull, 0x3fedace280000000ull, 0x3fe56109c0000001ull,
    0x3fb759d400000004ull, 0xbfe58e9e80000000ull, 0xbfe1b9fb40000002ull, 0x3fd3e3ba7ffffffdull,
    0x3fde9a317ffffffeull, 0xbfba2e7ffffffffdull, 0x3fd0e69e7ffffffeull, 0xbfe370e63ffffffdull,
    0xbfef9cd680000000ull, 0x3fd0bddf7fffffffull, 0xbf97c8afffffffe4ull, 0xbfddd13780000000ull,
    0xbfe4c6323ffffffeull, 0xbfe9597200000000ull, 0x3fb5ca8000000003ull, 0x3fec893140000002ull,
    0x3f86bdafffffff70ull, 0xbfe267b880000000ull, 0x3fd10c50fffffffaull, 0xbfe46839c0000001ull,
    0x3fef5f253ffffffeull, 0xbfd74ead7ffffffeull, 0x3fe2b16d40000001ull, 0xbfefefc7ffffffffull,
    0xbfecf195bffffffcull, 0xbfd252df7ffffffcull, 0xbfe295a940000004ull, 0xbfc7c650fffffff8ull,
    0x3fcc459b00000000ull, 0xbfea37d680000000ull, 0x3fd9be5d00000003ull, 0xbfdbfbd3fffffffeull,
    0x3fe8324bffffffffull, 0xbf501b7fffffff00ull, 0x3fc0993500000006ull, 0x3fe883bc3ffffffeull,
    0xbfdfa44affffffffull, 0xbfee1e48c0000000ull, 0x3fee371c3ffffffcull, 0x3fe0f9b340000000ull,
    0xbfe1eb9c3ffffffeull, 0x3fd13f2afffffffcull, 0xbfdc19d7fffffffdull, 0xbfe3f2e000000000ull,
    0xbfe3c53740000000ull, 0x3fe526fa40000000ull, 0x3fea3ebe00000000ull, 0xbfd222107ffffffeull,
    0x3fc2ede800000005ull, 0xbfe32a5dfffffffeull, 0x3fd65565fffffffaull, 0xbfeb9300c0000002ull,
    0x3fc61de400000005ull, 0xbfebb165bfffffffull, 0x3fda59387ffffffeull, 0x3fc35b2dfffffffeull,
    0x3fc41d5a00000002ull, 0xbfdcb18080000000ull, 0xbfd12f9b7ffffffdull, 0x3fe1b2c180000000ull,
    0x3fe84c3380000000ull, 0xbfda378080000001ull, 0x3fe2083e80000000ull, 0xbfedbce97ffffffeull,
    0xbfc0bcfafffffffaull, 0xbfea3968fffffffeull, 0x3fe9a799bfffffffull, 0xbfe22530c0000000ull,
    0x3fdf948080000000ull, 0xbfb0bb6c00000000ull, 0xbfe335d2c0000000ull, 0x3fe102c0bffffffeull,
    0x3fe90981ffffffffull, 0x3fe62acf3fffffffull, 0xbfbcd0720000000aull, 0x3fea6e9000000000ull,
    0x3fcdaf9e00000000ull, 0x3fd4e2747ffffffeull, 0xbfe7841000000000ull, 0xbfd67c9b7ffffffdull,
    0x3fe2dc6140000000ull, 0x3fe6b010c0000000ull, 0x3fac141c0000001eull, 0x3fe2246c00000002ull,
    0x3fdfc8e17ffffffeull, 0xbfe7b72080000001ull, 0xbfca9fc300000004ull, 0x3fdd51a600000000ull,
    0xbfe332d93ffffffeull, 0x3feeb7837fffffffull, 0x3fe7989200000002ull, 0x3fdcff96fffffffdull,
    0x3fe8502440000000ull, 0x3fd11e537fffffffull, 0xbfed15ed80000001ull, 0xbfe301f53fffffffull,
    0xbfed27657ffffffeull, 0xbfc8e2c1fffffffcull, 0xbfe0541580000004ull, 0xbfee8eea00000000ull,
    0xbfc2aed700000000ull, 0xbfe7a73e80000000ull, 0xbfedd17dc0000001ull, 0x3fdd888400000004ull,
    0xbfcdadee00000003ull, 0x3fd8553980000000ull, 0xbfc10eb300000000ull, 0x3fb7ada000000000ull,
    0xbfdf81ab80000000ull, 0x3fde9e9cfffffffeull, 0x3febb2e640000002ull, 0xbfc2927e00000008ull,
    0xbfe3b0537fffffffull, 0xbfe46e6cc0000002ull, 0xbfe2542bc0000000ull, 0x3fe2b80d00000001ull,
    0xbfefd7f380000001ull, 0xbfedae877fffffffull, 0x3fdb6b8c7ffffffeull, 0x3fdc409c80000002ull,
    0x3fdc63b600000005ull, 0x3fd762b67ffffffdull, 0xbfc18a2a00000008ull, 0x3fdb5f127ffffffdull,
};

std::vector<std::complex<double>> input() {
    std::vector<std::complex<double>> x(kN);
    std::uint32_t s = 20261110u;
    for (auto& v : x) {
        s = s * 1664525u + 1013904223u;
        const double re = static_cast<double>(static_cast<std::int32_t>(s) >> 8) / 8388608.0;
        s = s * 1664525u + 1013904223u;
        const double im = static_cast<double>(static_cast<std::int32_t>(s) >> 8) / 8388608.0;
        v = {re, im};
    }
    return x;
}

// Empty when every value matches; otherwise a one-line summary of the drift.
std::string compare(const std::vector<std::complex<double>>& got, const std::uint64_t* want,
                    bool bit_exact = kBitExact) {
    std::size_t differing = 0;
    std::uint64_t max_ulp = 0;
    double max_abs = 0.0;
    bool non_finite = false;
    for (std::size_t k = 0; k < got.size(); ++k) {
        const double parts[2] = {got[k].real(), got[k].imag()};
        for (int p = 0; p < 2; ++p) {
            non_finite |= !std::isfinite(parts[p]);
            const std::uint64_t a = std::bit_cast<std::uint64_t>(parts[p]);
            const std::uint64_t b = want[2 * k + static_cast<std::size_t>(p)];
            if (a == b)
                continue;
            ++differing;
            // Distance between the two patterns read as signed integers, taken
            // in unsigned arithmetic: a NaN against a negative golden spans more
            // than INT64_MAX, and a signed subtraction would overflow.
            const auto ia = static_cast<std::int64_t>(a);
            const auto ib = static_cast<std::int64_t>(b);
            max_ulp = std::max<std::uint64_t>(max_ulp, ia > ib ? a - b : b - a);
            max_abs = std::max(max_abs, std::abs(parts[p] - std::bit_cast<double>(b)));
        }
    }
    if (non_finite)
        return "non-finite transform result";
    if (differing == 0)
        return {};
    if (!bit_exact) {
        double peak = 0.0;
        for (std::size_t k = 0; k < got.size(); ++k)
            peak = std::max({peak, std::abs(std::bit_cast<double>(want[2 * k])),
                             std::abs(std::bit_cast<double>(want[2 * k + 1]))});
        if (max_abs <= kRelativeBound * peak)
            return {};
    }
    std::ostringstream out;
    out << differing << " of " << 2 * got.size() << " values differ from the golden; max "
        << max_ulp << " bit-pattern steps (not exact ulps across a sign change), max absolute "
        << max_abs;
    return out.str();
}

} // namespace

TEST_CASE("FftT<double> forward matches the committed golden at N = 128", "[signal][fft][golden]") {
    FftT<double> fft(kN);
    auto x = input();
    fft.forward(x.data());
    const auto drift = compare(x, kForward);
    INFO(drift);
    CHECK(drift.empty());
}

TEST_CASE("FftT<double> inverse of the golden forward matches the committed round trip",
          "[signal][fft][golden]") {
    FftT<double> fft(kN);
    std::vector<std::complex<double>> f(kN);
    for (std::size_t k = 0; k < f.size(); ++k)
        f[k] = {std::bit_cast<double>(kForward[2 * k]), std::bit_cast<double>(kForward[2 * k + 1])};
    fft.inverse(f.data());
    const auto drift = compare(f, kRoundTrip);
    INFO(drift);
    CHECK(drift.empty());
}

TEST_CASE("the tolerant golden comparison rejects non-finite transform results",
          "[signal][fft][golden][negative-control]") {
    std::vector<std::complex<double>> values(kN);
    for (std::size_t k = 0; k < values.size(); ++k)
        values[k] = {std::bit_cast<double>(kForward[2 * k]),
                     std::bit_cast<double>(kForward[2 * k + 1])};
    values.front().real(std::numeric_limits<double>::quiet_NaN());

    CHECK_FALSE(compare(values, kForward, false).empty());
}

TEST_CASE("the golden inputs are exact, so only the transform rounds", "[signal][fft][golden]") {
    for (const auto& v : input()) {
        CHECK(v.real() * 8388608.0 == std::round(v.real() * 8388608.0));
        CHECK(v.imag() * 8388608.0 == std::round(v.imag() * 8388608.0));
    }
}
