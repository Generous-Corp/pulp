#include <pulp/platform/objc_unique_class.hpp>

#include <dlfcn.h>
#include <objc/runtime.h>

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <unordered_map>

namespace pulp::platform::mac {

namespace {

constexpr const char* kContextIvar = "pulp_instance_context";

// Recursive: a configure callback may register another class.
std::recursive_mutex& registry_mutex() {
    static std::recursive_mutex mutex;
    return mutex;
}

// Classes this image created, by full name.
std::unordered_map<std::string, Class>& registry() {
    static std::unordered_map<std::string, Class> classes;
    return classes;
}

std::string& token_override() {
    static std::string token;
    return token;
}

std::string address_token() {
    Dl_info info{};
    if (dladdr(reinterpret_cast<const void*>(&address_token), &info) == 0 ||
        info.dli_fbase == nullptr)
        return "0";
    char buffer[2 + sizeof(std::uintptr_t) * 2 + 1];
    std::snprintf(
        buffer, sizeof buffer, "%llx",
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(info.dli_fbase)));
    return buffer;
}

Ivar context_ivar(id object) {
    if (object == nil)
        return nullptr;
    return class_getInstanceVariable(object_getClass(object), kContextIvar);
}

} // namespace

std::string image_token() {
    std::lock_guard<std::recursive_mutex> lock(registry_mutex());
    if (!token_override().empty())
        return token_override();
    static const std::string token = address_token();
    return token;
}

std::string unique_objc_class_name(std::string_view base) {
    std::string name(base);
    name += '_';
    name += image_token();
    return name;
}

void* unique_objc_class(std::string_view base, void* superclass, ConfigureClass configure,
                        void* user) {
    if (superclass == nullptr || base.empty())
        return nullptr;
    const std::string name = unique_objc_class_name(base);
    std::lock_guard<std::recursive_mutex> lock(registry_mutex());
    auto& classes = registry();
    if (auto it = classes.find(name); it != classes.end())
        return (__bridge void*)it->second;

    // The runtime refuses a name that already exists. A class of this name
    // that this image did not create belongs to someone else, so refuse it too
    // rather than run their code.
    Class cls = objc_allocateClassPair((__bridge Class)superclass, name.c_str(), 0);
    if (cls == nil)
        return nullptr;
    if (!class_addIvar(cls, kContextIvar, sizeof(void*), alignof(void*) == 8 ? 3 : 2, "^v")) {
        objc_disposeClassPair(cls);
        return nullptr;
    }
    if (configure != nullptr)
        configure((__bridge void*)cls, user);
    objc_registerClassPair(cls);
    classes.emplace(name, cls);
    return (__bridge void*)cls;
}

void set_instance_context(void* object, void* context) {
    id obj = (__bridge id)object;
    Ivar ivar = context_ivar(obj);
    if (ivar == nullptr)
        return;
    auto* base = reinterpret_cast<std::uint8_t*>(object);
    *reinterpret_cast<void**>(base + ivar_getOffset(ivar)) = context;
}

void* instance_context(void* object) {
    id obj = (__bridge id)object;
    Ivar ivar = context_ivar(obj);
    if (ivar == nullptr)
        return nullptr;
    auto* base = reinterpret_cast<std::uint8_t*>(object);
    return *reinterpret_cast<void**>(base + ivar_getOffset(ivar));
}

void* super_implementation(void* cls, const char* selector) {
    if (cls == nullptr || selector == nullptr)
        return nullptr;
    Class super_class = class_getSuperclass((__bridge Class)cls);
    if (super_class == nil)
        return nullptr;
    SEL sel = sel_registerName(selector);
    if (!class_respondsToSelector(super_class, sel))
        return nullptr;
    return reinterpret_cast<void*>(class_getMethodImplementation(super_class, sel));
}

namespace detail {

void set_image_token_for_testing(std::string token) {
    std::lock_guard<std::recursive_mutex> lock(registry_mutex());
    token_override() = std::move(token);
}

} // namespace detail

} // namespace pulp::platform::mac
