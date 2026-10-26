// CoreText face registry and measurement; see src/coretext_text.hpp.

#include "../../src/coretext_text.hpp"

#include "../../src/bundled_font_blobs.hpp"

#import <CoreText/CoreText.h>
#import <Foundation/Foundation.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace pulp::canvas::detail {

namespace {

struct RegisteredFace {
    CGFontRef font = nullptr; // Retained for the life of the process.
    std::vector<std::uint8_t> bytes;
    int weight = 400;
    bool italic = false;
};

struct Registry {
    std::mutex mutex;
    bool bundled_loaded = false;
    // Lower-cased family -> faces in registration order.
    std::unordered_map<std::string, std::vector<RegisteredFace>> families;
};

Registry& registry() {
    static Registry* r = new Registry(); // Never destroyed: fonts outlive statics.
    return *r;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// usWeightClass from the OS/2 table, the weight the font declares for itself.
int os2_weight(CGFontRef font) {
    CFDataRef table = CGFontCopyTableForTag(font, 'OS/2');
    if (!table)
        return 400;
    int weight = 400;
    if (CFDataGetLength(table) >= 6) {
        const UInt8* p = CFDataGetBytePtr(table);
        weight = (p[4] << 8) | p[5];
    }
    CFRelease(table);
    return weight >= 1 && weight <= 1000 ? weight : 400;
}

std::string font_family_name(CGFontRef font) {
    CTFontRef probe = CTFontCreateWithGraphicsFont(font, 12.0, nullptr, nullptr);
    if (!probe)
        return {};
    std::string out;
    if (CFStringRef name = CTFontCopyFamilyName(probe)) {
        out = [(__bridge NSString*)name UTF8String] ?: "";
        CFRelease(name);
    }
    CFRelease(probe);
    return out;
}

// Caller holds the registry lock.
std::string register_locked(Registry& r, const std::uint8_t* data, std::size_t size,
                            const std::string& family_override) {
    if (!data || size == 0)
        return {};
    RegisteredFace face;
    face.bytes.assign(data, data + size);
    CFDataRef cf = CFDataCreate(nullptr, face.bytes.data(), static_cast<CFIndex>(size));
    CGDataProviderRef provider = CGDataProviderCreateWithCFData(cf);
    CFRelease(cf);
    if (!provider)
        return {};
    face.font = CGFontCreateWithDataProvider(provider);
    CGDataProviderRelease(provider);
    if (!face.font)
        return {};

    std::string family = family_override.empty() ? font_family_name(face.font) : family_override;
    if (family.empty()) {
        CGFontRelease(face.font);
        return {};
    }
    auto& faces = r.families[lower(family)];
    for (const auto& existing : faces) {
        if (existing.bytes == face.bytes) {
            CGFontRelease(face.font);
            return family;
        }
    }
    face.weight = os2_weight(face.font);
    CTFontRef probe = CTFontCreateWithGraphicsFont(face.font, 12.0, nullptr, nullptr);
    if (probe) {
        face.italic = (CTFontGetSymbolicTraits(probe) & kCTFontItalicTrait) != 0;
        CFRelease(probe);
    }
    faces.push_back(std::move(face));
    return family;
}

void ensure_bundled_locked(Registry& r) {
    if (r.bundled_loaded)
        return;
    r.bundled_loaded = true;
    for (const auto& blob : bundled_font_blobs())
        register_locked(r, blob.data, blob.size, {});
}

// Closest weight within the requested slant, then within any slant.
const RegisteredFace* best_face(const std::vector<RegisteredFace>& faces, int weight, bool italic) {
    const RegisteredFace* best = nullptr;
    int best_score = 0;
    for (const auto& f : faces) {
        const int score = std::abs(f.weight - weight) + (f.italic == italic ? 0 : 10000);
        if (!best || score < best_score) {
            best = &f;
            best_score = score;
        }
    }
    return best;
}

std::vector<std::string> split_family_stack(std::string_view stack) {
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (pos <= stack.size()) {
        const std::size_t comma = stack.find(',', pos);
        std::string_view seg = stack.substr(
            pos, comma == std::string_view::npos ? std::string_view::npos : comma - pos);
        const auto a = seg.find_first_not_of(" \t\n\r\f\v");
        const auto b = seg.find_last_not_of(" \t\n\r\f\v");
        if (a != std::string_view::npos) {
            seg = seg.substr(a, b - a + 1);
            if (seg.size() >= 2 && (seg.front() == '"' || seg.front() == '\'') &&
                seg.back() == seg.front())
                seg = seg.substr(1, seg.size() - 2);
            if (!seg.empty())
                out.emplace_back(seg);
        }
        if (comma == std::string_view::npos)
            break;
        pos = comma + 1;
    }
    return out;
}

CTFontRef apply_style(CTFontRef font, int weight, int slant) {
    CTFontSymbolicTraits want = 0;
    if (weight >= 600)
        want |= kCTFontBoldTrait;
    if (slant != 0)
        want |= kCTFontItalicTrait;
    if (!want)
        return font;
    CTFontRef styled = CTFontCreateCopyWithSymbolicTraits(font, 0.0, nullptr, want, want);
    if (!styled)
        return font;
    CFRelease(font);
    return styled;
}

// An installed family: CoreText answers CTFontCreateWithName with Helvetica
// for a name it does not know, so a match is only real when the family name
// it returns is the one asked for.
CTFontRef create_installed(const std::string& family, float size) {
    NSString* name = [NSString stringWithUTF8String:family.c_str()];
    if (!name)
        return nullptr;
    CTFontRef font = CTFontCreateWithName((__bridge CFStringRef)name, size, nullptr);
    if (!font)
        return nullptr;
    bool match = false;
    if (CFStringRef got = CTFontCopyFamilyName(font)) {
        match = [(__bridge NSString*)got caseInsensitiveCompare:name] == NSOrderedSame;
        CFRelease(got);
    }
    if (!match) {
        if (CFStringRef ps = CTFontCopyPostScriptName(font)) {
            match = [(__bridge NSString*)ps caseInsensitiveCompare:name] == NSOrderedSame;
            CFRelease(ps);
        }
    }
    if (!match) {
        CFRelease(font);
        return nullptr;
    }
    return font;
}

CTFontRef apply_features(CTFontRef font, const std::vector<Canvas::FontFeature>& features) {
    if (features.empty())
        return font;
    NSMutableArray* settings = [NSMutableArray arrayWithCapacity:features.size()];
    for (const auto& f : features) {
        const char tag[5] = {
            static_cast<char>((f.tag >> 24) & 0xFF), static_cast<char>((f.tag >> 16) & 0xFF),
            static_cast<char>((f.tag >> 8) & 0xFF), static_cast<char>(f.tag & 0xFF), 0};
        [settings addObject:@{
            (__bridge id)kCTFontOpenTypeFeatureTag : [NSString stringWithUTF8String:tag],
            (__bridge id)kCTFontOpenTypeFeatureValue : @(f.value),
        }];
    }
    NSDictionary* attrs = @{(__bridge id)kCTFontFeatureSettingsAttribute : settings};
    CTFontDescriptorRef desc =
        CTFontDescriptorCreateWithAttributes((__bridge CFDictionaryRef)attrs);
    CTFontRef featured = CTFontCreateCopyWithAttributes(font, 0.0, nullptr, desc);
    CFRelease(desc);
    if (!featured)
        return font;
    CFRelease(font);
    return featured;
}

} // namespace

std::string coretext_register_font(const std::uint8_t* data, std::size_t size,
                                   const std::string& family_override) {
    auto& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    ensure_bundled_locked(r);
    return register_locked(r, data, size, family_override);
}

bool coretext_has_registered_family(const std::string& family) {
    auto& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    ensure_bundled_locked(r);
    return r.families.count(lower(family)) != 0;
}

namespace {

const void* create_styled_font(std::string_view family_stack, float size, int weight, int slant) {
    if (!(size > 0.0f))
        size = 12.0f;
    for (const auto& family : split_family_stack(family_stack)) {
        {
            auto& r = registry();
            std::lock_guard<std::mutex> lock(r.mutex);
            ensure_bundled_locked(r);
            auto it = r.families.find(lower(family));
            if (it != r.families.end()) {
                if (const auto* face = best_face(it->second, weight, slant != 0))
                    return CTFontCreateWithGraphicsFont(face->font, size, nullptr, nullptr);
            }
        }
        if (CTFontRef font = create_installed(family, size))
            return apply_style(font, weight, slant);
    }
    CTFontRef font = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, size, nullptr);
    return apply_style(font, weight, slant);
}

} // namespace

const void* coretext_create_font(std::string_view family_stack, float size, int weight, int slant,
                                 const std::vector<Canvas::FontFeature>& features) {
    return apply_features(
        static_cast<CTFontRef>(create_styled_font(family_stack, size, weight, slant)), features);
}

CoreTextFaceMetrics coretext_face_metrics(std::string_view family_stack, float size, int weight,
                                          int slant) {
    CoreTextFaceMetrics m;
    auto font = static_cast<CTFontRef>(coretext_create_font(family_stack, size, weight, slant));
    if (!font)
        return m;
    m.ascent = static_cast<float>(CTFontGetAscent(font));
    m.descent = static_cast<float>(CTFontGetDescent(font));
    m.leading = static_cast<float>(CTFontGetLeading(font));
    const CGRect bounds = CTFontGetBoundingBox(font);
    m.top = static_cast<float>(bounds.origin.y + bounds.size.height);
    m.bottom = static_cast<float>(-bounds.origin.y);
    CFRelease(font);
    return m;
}

std::string coretext_face_postscript_name(std::string_view family_stack, int weight, int slant) {
    auto font = static_cast<CTFontRef>(coretext_create_font(family_stack, 12.0f, weight, slant));
    if (!font)
        return {};
    std::string out;
    if (CFStringRef name = CTFontCopyPostScriptName(font)) {
        char buf[256];
        if (CFStringGetCString(name, buf, sizeof(buf), kCFStringEncodingUTF8))
            out = buf;
        CFRelease(name);
    }
    CFRelease(font);
    return out;
}

float coretext_advance(std::string_view text, std::string_view family_stack, float size, int weight,
                       int slant, const std::vector<Canvas::FontFeature>& features) {
    if (text.empty())
        return 0.0f;
    @autoreleasepool {
        NSString* ns = [[NSString alloc] initWithBytes:text.data()
                                                length:text.size()
                                              encoding:NSUTF8StringEncoding];
        if (!ns)
            return 0.0f;
        auto font = static_cast<CTFontRef>(
            coretext_create_font(family_stack, size, weight, slant, features));
        if (!font)
            return 0.0f;
        NSDictionary* attrs = @{(__bridge id)kCTFontAttributeName : (__bridge id)font};
        NSAttributedString* str = [[NSAttributedString alloc] initWithString:ns attributes:attrs];
        CTLineRef line = CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)str);
        const double width =
            line ? CTLineGetTypographicBounds(line, nullptr, nullptr, nullptr) : 0.0;
        if (line)
            CFRelease(line);
        CFRelease(font);
        return static_cast<float>(width);
    }
}

} // namespace pulp::canvas::detail
