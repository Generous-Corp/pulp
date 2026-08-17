#import <JavaScriptCore/JavaScriptCore.h>

int main() {
    JSGlobalContextRef context = JSGlobalContextCreate(nullptr);
    if (context == nullptr)
        return 1;
    JSGlobalContextRelease(context);
    return 0;
}
