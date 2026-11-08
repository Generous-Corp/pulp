// A second image for the unique-class tests: a dylib that links its own copy
// of pulp-platform, as a second plug-in in the same host process would.

#include <pulp/platform/objc_unique_class.hpp>

#import <Foundation/Foundation.h>
#include <objc/message.h>
#include <objc/runtime.h>

#include <string>

namespace {

int fixture_identity(id, SEL) {
    return 2;
}

void add_identity(void* cls, void*) {
    class_addMethod((__bridge Class)cls, sel_registerName("pulpIdentity"),
                    reinterpret_cast<IMP>(&fixture_identity), "i@:");
}

std::string g_name;

} // namespace

extern "C" __attribute__((visibility("default"))) const char*
pulp_unique_fixture_class_name(const char* base) {
    g_name = pulp::platform::mac::unique_objc_class_name(base);
    return g_name.c_str();
}

extern "C" __attribute__((visibility("default"))) void*
pulp_unique_fixture_register(const char* base) {
    return pulp::platform::mac::unique_objc_class(base, (__bridge void*)[NSObject class],
                                                  &add_identity);
}

extern "C" __attribute__((visibility("default"))) void
pulp_unique_fixture_force_token(const char* token) {
    pulp::platform::mac::detail::set_image_token_for_testing(token != nullptr ? token : "");
}
