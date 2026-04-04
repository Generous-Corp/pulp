#include <pulp/platform/clipboard.hpp>

#ifdef __APPLE__
#import <Cocoa/Cocoa.h>

#include <mutex>

namespace pulp::platform {

namespace {

std::mutex g_fallback_mutex;
std::string g_fallback_text;
bool g_fallback_has_text = false;
long long g_fallback_text_change_count = -1;
std::string g_fallback_data_type;
std::vector<uint8_t> g_fallback_data;
bool g_fallback_has_data = false;
long long g_fallback_data_change_count = -1;

void clear_fallback_text_unlocked() {
    g_fallback_text.clear();
    g_fallback_has_text = false;
    g_fallback_text_change_count = -1;
}

void clear_fallback_data_unlocked() {
    g_fallback_data_type.clear();
    g_fallback_data.clear();
    g_fallback_has_data = false;
    g_fallback_data_change_count = -1;
}

bool fallback_set_text(const std::string& text, long long change_count = -1) {
    std::lock_guard lock(g_fallback_mutex);
    clear_fallback_data_unlocked();
    g_fallback_text = text;
    g_fallback_has_text = true;
    g_fallback_text_change_count = change_count;
    return true;
}

std::optional<std::string> fallback_get_text() {
    std::lock_guard lock(g_fallback_mutex);
    if (!g_fallback_has_text) {
        return std::nullopt;
    }
    return g_fallback_text;
}

bool fallback_has_text() {
    std::lock_guard lock(g_fallback_mutex);
    return g_fallback_has_text;
}

bool fallback_text_matches_change_count(long long change_count) {
    std::lock_guard lock(g_fallback_mutex);
    return g_fallback_has_text && g_fallback_text_change_count >= 0
        && g_fallback_text_change_count == change_count;
}

bool fallback_set_data(const std::string& type, const std::vector<uint8_t>& data, long long change_count = -1) {
    std::lock_guard lock(g_fallback_mutex);
    clear_fallback_text_unlocked();
    g_fallback_data_type = type;
    g_fallback_data = data;
    g_fallback_has_data = true;
    g_fallback_data_change_count = change_count;
    return true;
}

std::optional<std::vector<uint8_t>> fallback_get_data(const std::string& type) {
    std::lock_guard lock(g_fallback_mutex);
    if (!g_fallback_has_data || g_fallback_data_type != type) {
        return std::nullopt;
    }
    return g_fallback_data;
}

bool fallback_data_matches_change_count(const std::string& type, long long change_count) {
    std::lock_guard lock(g_fallback_mutex);
    return g_fallback_has_data && g_fallback_data_type == type
        && g_fallback_data_change_count >= 0
        && g_fallback_data_change_count == change_count;
}

} // namespace

bool Clipboard::set_text(const std::string& text) {
    @autoreleasepool {
        NSPasteboard* pb = [NSPasteboard generalPasteboard];
        if (!pb) {
            return fallback_set_text(text);
        }

        [pb clearContents];
        const bool ok = [pb setString:[NSString stringWithUTF8String:text.c_str()]
                              forType:NSPasteboardTypeString];
        if (!ok) {
            return fallback_set_text(text, static_cast<long long>([pb changeCount]));
        }

        fallback_set_text(text, static_cast<long long>([pb changeCount]));
        return true;
    }
}

std::optional<std::string> Clipboard::get_text() {
    @autoreleasepool {
        NSPasteboard* pb = [NSPasteboard generalPasteboard];
        if (!pb) {
            return fallback_get_text();
        }
        NSString* str = [pb stringForType:NSPasteboardTypeString];
        if (!str) {
            if (fallback_text_matches_change_count(static_cast<long long>([pb changeCount]))) {
                return fallback_get_text();
            }
            return std::nullopt;
        }
        return std::string([str UTF8String]);
    }
}

bool Clipboard::has_text() {
    @autoreleasepool {
        NSPasteboard* pb = [NSPasteboard generalPasteboard];
        if (!pb) {
            return fallback_has_text();
        }
        if ([pb stringForType:NSPasteboardTypeString] != nil) {
            return true;
        }
        return fallback_text_matches_change_count(static_cast<long long>([pb changeCount]));
    }
}

bool Clipboard::set_data(const std::string& type, const std::vector<uint8_t>& data) {
    @autoreleasepool {
        NSPasteboard* pb = [NSPasteboard generalPasteboard];
        if (!pb) {
            return fallback_set_data(type, data);
        }
        NSString* ns_type = [NSString stringWithUTF8String:type.c_str()];
        NSData* ns_data = [NSData dataWithBytes:data.data() length:data.size()];
        [pb clearContents];
        const bool ok = [pb setData:ns_data forType:ns_type];
        if (!ok) {
            return fallback_set_data(type, data, static_cast<long long>([pb changeCount]));
        }

        fallback_set_data(type, data, static_cast<long long>([pb changeCount]));
        return true;
    }
}

std::optional<std::vector<uint8_t>> Clipboard::get_data(const std::string& type) {
    @autoreleasepool {
        NSPasteboard* pb = [NSPasteboard generalPasteboard];
        if (!pb) {
            return fallback_get_data(type);
        }
        NSString* ns_type = [NSString stringWithUTF8String:type.c_str()];
        NSData* ns_data = [pb dataForType:ns_type];
        if (!ns_data) {
            if (fallback_data_matches_change_count(type, static_cast<long long>([pb changeCount]))) {
                return fallback_get_data(type);
            }
            return std::nullopt;
        }
        auto* bytes = static_cast<const uint8_t*>([ns_data bytes]);
        return std::vector<uint8_t>(bytes, bytes + [ns_data length]);
    }
}

} // namespace pulp::platform

#else

namespace pulp::platform {
bool Clipboard::set_text(const std::string&) { return false; }
std::optional<std::string> Clipboard::get_text() { return std::nullopt; }
bool Clipboard::has_text() { return false; }
bool Clipboard::set_data(const std::string&, const std::vector<uint8_t>&) { return false; }
std::optional<std::vector<uint8_t>> Clipboard::get_data(const std::string&) { return std::nullopt; }
}

#endif
