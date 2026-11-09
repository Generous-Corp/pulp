// Per-binary unique Objective-C class names. Two images that each link
// pulp-platform (here the test binary and a fixture dylib) must register
// distinct classes for the same base name and each run its own code.

#include <pulp/platform/objc_unique_class.hpp>

#include <catch2/catch_test_macros.hpp>

#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <objc/message.h>
#include <objc/runtime.h>

#include <string>

namespace mac = pulp::platform::mac;

namespace {

int test_identity(id, SEL) {
    return 1;
}

int g_configure_calls = 0;

void add_identity(void* cls, void*) {
    ++g_configure_calls;
    class_addMethod((__bridge Class)cls, sel_registerName("pulpIdentity"),
                    reinterpret_cast<IMP>(&test_identity), "i@:");
}

int identity_of(void* cls) {
    id object = [[(__bridge Class)cls alloc] init];
    auto call = reinterpret_cast<int (*)(id, SEL)>(objc_msgSend);
    const int identity = call(object, sel_registerName("pulpIdentity"));
    [object release];
    return identity;
}

struct Fixture {
    void* handle = nullptr;
    const char* (*class_name)(const char*) = nullptr;
    void* (*register_class)(const char*) = nullptr;
    void (*force_token)(const char*) = nullptr;

    Fixture() {
        handle = dlopen(PULP_OBJC_UNIQUE_FIXTURE_PATH, RTLD_NOW | RTLD_LOCAL);
        if (handle == nullptr)
            return;
        class_name = reinterpret_cast<const char* (*)(const char*)>(
            dlsym(handle, "pulp_unique_fixture_class_name"));
        register_class =
            reinterpret_cast<void* (*)(const char*)>(dlsym(handle, "pulp_unique_fixture_register"));
        force_token = reinterpret_cast<void (*)(const char*)>(
            dlsym(handle, "pulp_unique_fixture_force_token"));
    }
    bool ok() const {
        return class_name != nullptr && register_class != nullptr && force_token != nullptr;
    }
};

Fixture& fixture() {
    static Fixture f; // kept loaded: classes it registered cannot be unloaded
    return f;
}

} // namespace

TEST_CASE("the class name is the base plus this image's token", "[platform][objc-unique]") {
    const std::string token = mac::image_token();
    REQUIRE_FALSE(token.empty());
    REQUIRE(token != "0");
    REQUIRE(token == mac::image_token());
    REQUIRE(mac::unique_objc_class_name("PulpProbe") == "PulpProbe_" + token);
}

TEST_CASE("a class registers once and is cached", "[platform][objc-unique]") {
    g_configure_calls = 0;
    void* first =
        mac::unique_objc_class("PulpUniqueCached", (__bridge void*)[NSObject class], &add_identity);
    void* second =
        mac::unique_objc_class("PulpUniqueCached", (__bridge void*)[NSObject class], &add_identity);
    REQUIRE(first != nullptr);
    REQUIRE(first == second);
    REQUIRE(g_configure_calls == 1);
    REQUIRE(std::string(class_getName((__bridge Class)first)) ==
            mac::unique_objc_class_name("PulpUniqueCached"));
    REQUIRE([(__bridge Class)first isSubclassOfClass:[NSObject class]]);
    REQUIRE(identity_of(first) == 1);
}

TEST_CASE("a name already taken by a class this helper did not make is refused",
          "[platform][objc-unique]") {
    const std::string name = mac::unique_objc_class_name("PulpUniqueSquatted");
    Class squatter = objc_allocateClassPair([NSObject class], name.c_str(), 0);
    REQUIRE(squatter != nil);
    objc_registerClassPair(squatter);
    REQUIRE(mac::unique_objc_class("PulpUniqueSquatted", (__bridge void*)[NSObject class],
                                   &add_identity) == nullptr);
}

TEST_CASE("each instance has its own context slot", "[platform][objc-unique]") {
    void* cls =
        mac::unique_objc_class("PulpUniqueContext", (__bridge void*)[NSObject class], nullptr);
    REQUIRE(cls != nullptr);
    id a = [[(__bridge Class)cls alloc] init];
    id b = [[(__bridge Class)cls alloc] init];
    int x = 0, y = 0;
    REQUIRE(mac::instance_context((__bridge void*)a) == nullptr);
    mac::set_instance_context((__bridge void*)a, &x);
    mac::set_instance_context((__bridge void*)b, &y);
    REQUIRE(mac::instance_context((__bridge void*)a) == &x);
    REQUIRE(mac::instance_context((__bridge void*)b) == &y);
    // An object of an unrelated class has no slot: reads null, writes nothing.
    id plain = [[NSObject alloc] init];
    mac::set_instance_context((__bridge void*)plain, &x);
    REQUIRE(mac::instance_context((__bridge void*)plain) == nullptr);
    [a release];
    [b release];
    [plain release];
}

TEST_CASE("super_implementation finds the superclass method", "[platform][objc-unique]") {
    void* cls =
        mac::unique_objc_class("PulpUniqueSuper", (__bridge void*)[NSObject class], nullptr);
    REQUIRE(cls != nullptr);
    void* imp = mac::super_implementation(cls, "description");
    REQUIRE(imp != nullptr);
    REQUIRE(imp == reinterpret_cast<void*>(
                       class_getMethodImplementation([NSObject class], @selector(description))));
    REQUIRE(mac::super_implementation(cls, "pulpNoSuchSelector") == nullptr);
}

TEST_CASE("two images get distinct classes that each run their own code",
          "[platform][objc-unique][two-images]") {
    auto& f = fixture();
    REQUIRE(f.ok());
    const std::string mine = mac::unique_objc_class_name("PulpUniqueShared");
    const std::string theirs = f.class_name("PulpUniqueShared");
    REQUIRE(mine != theirs);

    void* my_class =
        mac::unique_objc_class("PulpUniqueShared", (__bridge void*)[NSObject class], &add_identity);
    void* their_class = f.register_class("PulpUniqueShared");
    REQUIRE(my_class != nullptr);
    REQUIRE(their_class != nullptr);
    REQUIRE(my_class != their_class);
    REQUIRE(identity_of(my_class) == 1);
    REQUIRE(identity_of(their_class) == 2);
}

// Control for the case above: when both images use the same token (what a
// fixed class name amounts to), the second registration is refused instead of
// silently sharing the first image's class.
TEST_CASE("a forced shared token makes the second image's registration fail loudly",
          "[platform][objc-unique][collision]") {
    auto& f = fixture();
    REQUIRE(f.ok());
    mac::detail::set_image_token_for_testing("fixed");
    f.force_token("fixed");
    REQUIRE(mac::unique_objc_class_name("PulpUniqueCollide") ==
            std::string(f.class_name("PulpUniqueCollide")));
    void* my_class = mac::unique_objc_class("PulpUniqueCollide", (__bridge void*)[NSObject class],
                                            &add_identity);
    void* their_class = f.register_class("PulpUniqueCollide");
    mac::detail::set_image_token_for_testing("");
    f.force_token("");
    REQUIRE(my_class != nullptr);
    REQUIRE(their_class == nullptr);
}
