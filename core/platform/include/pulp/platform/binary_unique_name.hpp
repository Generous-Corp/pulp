#pragma once

/// @file binary_unique_name.hpp
/// Names that are unique to the binary (executable or shared library) that
/// contains this code.
///
/// Some operating-system registrations are process-wide by name: Win32 window
/// classes, X11 atoms a library creates, Objective-C classes. When two Pulp
/// plug-ins built from different Pulp versions load into one host, a fixed
/// name registered by both makes one plug-in run the other's code, or makes
/// the second registration fail. pulp-platform is a static library, so each
/// binary links its own copy of this function and so gets its own token: the
/// load address of the image that holds it.

#include <string>
#include <string_view>

namespace pulp::platform {

/// Hex load address of the image holding this code. Stable for the life of
/// the process; different in every loaded image. "0" if the platform cannot
/// say.
std::string binary_image_token();

/// `<base>_<binary_image_token()>`.
std::string binary_unique_name(std::string_view base);

} // namespace pulp::platform
