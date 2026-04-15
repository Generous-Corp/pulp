// iOS UIPasteboard-backed Clipboard
#import <UIKit/UIKit.h>
#include <pulp/platform/clipboard.hpp>

namespace pulp::platform {

bool Clipboard::set_text(const std::string& text) {
    @autoreleasepool {
        UIPasteboard* pb = [UIPasteboard generalPasteboard];
        pb.string = [NSString stringWithUTF8String:text.c_str()];
        return true;
    }
}

std::optional<std::string> Clipboard::get_text() {
    @autoreleasepool {
        UIPasteboard* pb = [UIPasteboard generalPasteboard];
        NSString* s = pb.string;
        if (!s) return std::nullopt;
        return std::string(s.UTF8String ? s.UTF8String : "");
    }
}

bool Clipboard::has_text() {
    @autoreleasepool { return [UIPasteboard generalPasteboard].hasStrings; }
}

bool Clipboard::set_data(const std::string& type, const std::vector<uint8_t>& data) {
    @autoreleasepool {
        UIPasteboard* pb = [UIPasteboard generalPasteboard];
        NSData* nsdata = [NSData dataWithBytes:data.data() length:data.size()];
        [pb setData:nsdata forPasteboardType:[NSString stringWithUTF8String:type.c_str()]];
        return true;
    }
}

std::optional<std::vector<uint8_t>> Clipboard::get_data(const std::string& type) {
    @autoreleasepool {
        UIPasteboard* pb = [UIPasteboard generalPasteboard];
        NSData* d = [pb dataForPasteboardType:[NSString stringWithUTF8String:type.c_str()]];
        if (!d) return std::nullopt;
        const uint8_t* bytes = static_cast<const uint8_t*>(d.bytes);
        return std::vector<uint8_t>(bytes, bytes + d.length);
    }
}

} // namespace pulp::platform
