#include "control_static_code_identity.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

#include <string>

namespace pulp::inspect::detail {
namespace {

template <typename T> class CfOwner {
  public:
    explicit CfOwner(T value = nullptr) : value_(value) {}
    ~CfOwner() {
        if (value_)
            CFRelease(value_);
    }
    CfOwner(const CfOwner&) = delete;
    CfOwner& operator=(const CfOwner&) = delete;
    T get() const {
        return value_;
    }

  private:
    T value_;
};

std::string cf_string(CFTypeRef value) {
    if (!value || CFGetTypeID(value) != CFStringGetTypeID())
        return {};
    const auto string = static_cast<CFStringRef>(value);
    const auto capacity =
        CFStringGetMaximumSizeForEncoding(CFStringGetLength(string), kCFStringEncodingUTF8) + 1;
    if (capacity <= 1)
        return {};
    std::string result(static_cast<std::size_t>(capacity), '\0');
    if (!CFStringGetCString(string, result.data(), capacity, kCFStringEncodingUTF8))
        return {};
    result.resize(std::char_traits<char>::length(result.c_str()));
    return result;
}

std::string cf_data_hex(CFTypeRef value) {
    if (!value || CFGetTypeID(value) != CFDataGetTypeID())
        return {};
    const auto data = static_cast<CFDataRef>(value);
    const auto size = CFDataGetLength(data);
    if (size <= 0)
        return {};
    constexpr char digits[] = "0123456789abcdef";
    std::string result(static_cast<std::size_t>(size) * 2, '0');
    const auto* bytes = CFDataGetBytePtr(data);
    for (CFIndex index = 0; index < size; ++index) {
        result[static_cast<std::size_t>(index) * 2] = digits[bytes[index] >> 4];
        result[static_cast<std::size_t>(index) * 2 + 1] = digits[bytes[index] & 0xf];
    }
    return result;
}

std::uint32_t cf_number_u32(CFTypeRef value) {
    if (!value || CFGetTypeID(value) != CFNumberGetTypeID())
        return 0;
    std::uint32_t result = 0;
    return CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberSInt32Type, &result)
               ? result
               : 0;
}

bool entitlement_enabled(CFDictionaryRef information, CFStringRef entitlement) {
    const auto value = CFDictionaryGetValue(information, kSecCodeInfoEntitlementsDict);
    if (!value || CFGetTypeID(value) != CFDictionaryGetTypeID())
        return false;
    const auto enabled = CFDictionaryGetValue(static_cast<CFDictionaryRef>(value), entitlement);
    return enabled && CFGetTypeID(enabled) == CFBooleanGetTypeID() &&
           CFBooleanGetValue(static_cast<CFBooleanRef>(enabled));
}

} // namespace

std::optional<ControlTrustedHostStaticExpectation>
inspect_static_code_identity(const std::filesystem::path& executable) {
    const auto executable_bytes = executable.string();
    CfOwner<CFURLRef> url(CFURLCreateFromFileSystemRepresentation(
        kCFAllocatorDefault, reinterpret_cast<const UInt8*>(executable_bytes.data()),
        static_cast<CFIndex>(executable_bytes.size()), false));
    if (!url.get())
        return std::nullopt;
    SecStaticCodeRef raw_code = nullptr;
    if (SecStaticCodeCreateWithPath(url.get(), kSecCSDefaultFlags, &raw_code) != errSecSuccess ||
        !raw_code)
        return std::nullopt;
    CfOwner<SecStaticCodeRef> code(raw_code);
    if (SecStaticCodeCheckValidity(code.get(), kSecCSStrictValidate, nullptr) != errSecSuccess)
        return std::nullopt;
    CFDictionaryRef raw_information = nullptr;
    if (SecCodeCopySigningInformation(code.get(), kSecCSSigningInformation, &raw_information) !=
            errSecSuccess ||
        !raw_information)
        return std::nullopt;
    CfOwner<CFDictionaryRef> information(raw_information);
    const auto identifier =
        cf_string(CFDictionaryGetValue(information.get(), kSecCodeInfoIdentifier));
    const auto team =
        cf_string(CFDictionaryGetValue(information.get(), kSecCodeInfoTeamIdentifier));
    const auto cdhash = cf_data_hex(CFDictionaryGetValue(information.get(), kSecCodeInfoUnique));
    const auto flags =
        cf_number_u32(CFDictionaryGetValue(information.get(), kSecCodeInfoFlags));
    const bool library_validation =
        (flags & kSecCodeSignatureLibraryValidation) != 0 ||
        ((flags & kSecCodeSignatureRuntime) != 0 &&
         !entitlement_enabled(information.get(),
                              CFSTR("com.apple.security.cs.disable-library-validation")));
    if (identifier.empty() || cdhash.empty())
        return std::nullopt;
    return ControlTrustedHostStaticExpectation{
        "signed:" + identifier + ":" + cdhash,
        team.empty() ? "adhoc:" + cdhash : "team:" + team,
        library_validation};
}

bool is_apple_platform_code(const std::filesystem::path& executable) {
    const auto executable_bytes = executable.string();
    CfOwner<CFURLRef> url(CFURLCreateFromFileSystemRepresentation(
        kCFAllocatorDefault, reinterpret_cast<const UInt8*>(executable_bytes.data()),
        static_cast<CFIndex>(executable_bytes.size()), false));
    if (!url.get())
        return false;
    SecStaticCodeRef raw_code = nullptr;
    if (SecStaticCodeCreateWithPath(url.get(), kSecCSDefaultFlags, &raw_code) != errSecSuccess ||
        !raw_code)
        return false;
    CfOwner<SecStaticCodeRef> code(raw_code);
    CfOwner<CFStringRef> requirement_text(
        CFStringCreateWithCString(kCFAllocatorDefault, "anchor apple", kCFStringEncodingUTF8));
    if (!requirement_text.get())
        return false;
    SecRequirementRef raw_requirement = nullptr;
    if (SecRequirementCreateWithString(requirement_text.get(), kSecCSDefaultFlags,
                                       &raw_requirement) != errSecSuccess ||
        !raw_requirement)
        return false;
    CfOwner<SecRequirementRef> requirement(raw_requirement);
    return SecStaticCodeCheckValidity(code.get(), kSecCSStrictValidate, requirement.get()) ==
           errSecSuccess;
}

} // namespace pulp::inspect::detail
