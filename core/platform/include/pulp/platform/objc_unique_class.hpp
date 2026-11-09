#pragma once

/// @file objc_unique_class.hpp
/// Objective-C classes whose names are unique to the binary that defines them.
///
/// Objective-C class names are process-wide. When two plug-ins built from Pulp
/// load into one host, a class compiled into both under a fixed name exists
/// twice; the runtime warns and lets the first loaded copy win, so one
/// plug-in's views or delegates silently run the other's code. That is
/// dangerous across versions, where the two copies behave differently.
///
/// The helper registers a class at run time under `<base>_<token>`, where the
/// token is the load address of the image that contains this helper.
/// pulp-platform is a static library, so each plug-in, app or test binary
/// links its own copy and so names its own classes. Methods are added from C
/// functions with `class_addMethod` inside the `configure` callback, before
/// the class is registered.
///
/// Apple platforms only. The API uses `void*` for `Class` and `id`, so C++
/// translation units can include it; Objective-C++ callers bridge with
/// `(__bridge Class)` / `(__bridge id)` or a plain cast.

#include <string>
#include <string_view>

namespace pulp::platform::mac {

/// Hex load address of the image (executable or dylib) holding this helper.
/// Stable for the life of the process; different in every loaded image.
std::string image_token();

/// `<base>_<image_token()>`.
std::string unique_objc_class_name(std::string_view base);

/// Called once, between allocating and registering a class, to add methods
/// and protocols to it. `cls` is the `Class`.
using ConfigureClass = void (*)(void* cls, void* user);

/// The class named unique_objc_class_name(base), subclassing `superclass`,
/// registered on first use and cached afterwards. Each class gets one
/// instance-context slot (see set_instance_context). Thread-safe.
///
/// Returns nullptr when the runtime refuses the class, for instance because a
/// class of that name already exists that this helper did not create. It
/// never hands back a class registered by another image.
void* unique_objc_class(std::string_view base, void* superclass, ConfigureClass configure,
                        void* user = nullptr);

/// Store an opaque pointer in an instance of a class made by
/// unique_objc_class (or a subclass of one). Not retained.
void set_instance_context(void* object, void* context);
/// The pointer stored with set_instance_context, or nullptr.
void* instance_context(void* object);

/// The implementation `selector` has in `cls`'s superclass, for an override
/// that has to call super. nullptr when the superclass does not respond.
void* super_implementation(void* cls, const char* selector);

namespace detail {
/// Replace the token for the rest of the process (empty restores the image
/// address). Lets a test show what happens when two images collide.
void set_image_token_for_testing(std::string token);
} // namespace detail

} // namespace pulp::platform::mac
