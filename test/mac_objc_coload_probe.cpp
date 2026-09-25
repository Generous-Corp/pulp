// Co-load two Pulp plug-in binaries into one process and prove that their
// macOS view Objective-C classes are registered under distinct names.
//
// Objective-C class names are process-global. _pulp_apply_view_mac_objc_suffix()
// (tools/cmake/PulpUtils.cmake) compiles the macOS view / window /
// accessibility cluster into every shipped binary with each class name
// suffixed by the binary's target name, so two Pulp plug-ins loaded by one host
// never register the same class. Nothing short of loading two real bundles
// proves that: a unit test that links pulp::view only ever sees the library's
// fixed-name copies. This probe dlopens both binaries and, for every class
// base name it is given, requires
//
//   - the class suffixed for binary A to exist,
//   - the class suffixed for binary B to exist, and
//   - the bare, unsuffixed class to be absent.
//
// A build that quietly stopped applying the suffix, or that let pulp-view-core's
// fixed-name copy be pulled into a bundle beside the suffixed one, produces the
// bare name, which is exactly the collision the suffix exists to prevent.
//
// usage: mac_objc_coload_probe <binary-a> <suffix-a> <binary-b> <suffix-b>
//                              <ClassBase> [<ClassBase> ...]
//
// Exit 0 when every check holds, 1 on a failed check, 2 on a usage or load
// error. Every finding is printed, so one run names every offending class.

#include <dlfcn.h>
#include <objc/runtime.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

bool class_exists(const std::string& name) {
    return objc_getClass(name.c_str()) != nullptr;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 6) {
        std::fprintf(stderr,
                     "usage: %s <binary-a> <suffix-a> <binary-b> <suffix-b> <ClassBase>...\n",
                     argv[0]);
        return 2;
    }
    const std::string binary_a = argv[1];
    const std::string suffix_a = argv[2];
    const std::string binary_b = argv[3];
    const std::string suffix_b = argv[4];
    std::vector<std::string> bases(argv + 5, argv + argc);

    if (suffix_a == suffix_b) {
        std::fprintf(stderr, "the two suffixes must differ (both are '%s')\n",
                     suffix_a.c_str());
        return 2;
    }

    // The bare names must be absent BEFORE either binary loads too; otherwise a
    // fixed-name class already present in this process would make the absence
    // check below meaningless.
    for (const auto& base : bases) {
        if (class_exists(base)) {
            std::fprintf(stderr, "instrument broken: class %s already exists before "
                                 "any bundle is loaded\n",
                         base.c_str());
            return 2;
        }
    }

    for (const auto& binary : {binary_a, binary_b}) {
        if (!dlopen(binary.c_str(), RTLD_NOW | RTLD_LOCAL)) {
            std::fprintf(stderr, "dlopen(%s) failed: %s\n", binary.c_str(), dlerror());
            return 2;
        }
    }

    int failures = 0;
    for (const auto& base : bases) {
        const std::string a = base + "_" + suffix_a;
        const std::string b = base + "_" + suffix_b;
        if (!class_exists(a)) {
            std::fprintf(stderr, "FAIL: %s is not registered after loading %s\n", a.c_str(),
                         binary_a.c_str());
            ++failures;
        }
        if (!class_exists(b)) {
            std::fprintf(stderr, "FAIL: %s is not registered after loading %s\n", b.c_str(),
                         binary_b.c_str());
            ++failures;
        }
        if (class_exists(base)) {
            std::fprintf(stderr,
                         "FAIL: bare class %s is registered; a binary shipped the shared "
                         "fixed-name copy and would collide with the next Pulp plug-in\n",
                         base.c_str());
            ++failures;
        }
    }

    if (failures) {
        std::fprintf(stderr, "%d failed check(s) across %zu class base name(s)\n", failures,
                     bases.size());
        return 1;
    }
    std::printf("OK: %zu class base name(s) registered under distinct per-binary names "
                "(%s, %s), none under the bare name\n",
                bases.size(), suffix_a.c_str(), suffix_b.c_str());
    return 0;
}
