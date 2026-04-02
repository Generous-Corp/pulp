// JavaScriptCore backend for the Pulp JS engine abstraction.
// Apple platforms only — uses the system JavaScriptCore.framework (LGPL-2.1,
// system framework usage is fine). Zero additional dependency on Apple.

#include <pulp/view/js_engine.hpp>

#if __APPLE__

#import <JavaScriptCore/JavaScriptCore.h>
#include <choc/text/choc_JSON.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <stdexcept>

namespace pulp::view {

// Forward declaration
static JSValue* choc_to_jsc(JSContext* ctx, const choc::value::Value& value);

template <typename ElementType, typename ConvertFn>
static choc::value::Value typed_array_to_choc_array(const ElementType* data,
                                                    size_t length,
                                                    ConvertFn convert) {
    return choc::value::createArray(static_cast<uint32_t>(length),
        [data, convert](uint32_t index) -> choc::value::Value {
            return convert(data[index]);
        });
}

static choc::value::Value jsc_typed_array_to_choc(JSContext* ctx, JSValue* value) {
    JSContextRef raw_ctx = [ctx JSGlobalContextRef];
    JSValueRef exception = nullptr;
    auto typed_array_type = JSValueGetTypedArrayType(raw_ctx, [value JSValueRef], &exception);

    if (exception != nullptr || typed_array_type == kJSTypedArrayTypeNone)
        return {};

    JSObjectRef object = JSValueToObject(raw_ctx, [value JSValueRef], &exception);
    if (exception != nullptr || object == nullptr)
        return {};

    if (typed_array_type == kJSTypedArrayTypeArrayBuffer) {
        auto* bytes = static_cast<const uint8_t*>(JSObjectGetArrayBufferBytesPtr(raw_ctx, object, &exception));
        auto length = JSObjectGetArrayBufferByteLength(raw_ctx, object, &exception);
        if (exception != nullptr || bytes == nullptr)
            return {};

        return typed_array_to_choc_array(bytes, length, [](uint8_t sample) {
            return choc::value::createInt32(static_cast<int32_t>(sample));
        });
    }

    auto* bytes = static_cast<const uint8_t*>(JSObjectGetTypedArrayBytesPtr(raw_ctx, object, &exception));
    auto length = JSObjectGetTypedArrayLength(raw_ctx, object, &exception);
    if (exception != nullptr || bytes == nullptr)
        return {};

    switch (typed_array_type) {
        case kJSTypedArrayTypeInt8Array:
            return typed_array_to_choc_array(reinterpret_cast<const int8_t*>(bytes), length, [](int8_t sample) {
                return choc::value::createInt32(static_cast<int32_t>(sample));
            });
        case kJSTypedArrayTypeInt16Array:
            return typed_array_to_choc_array(reinterpret_cast<const int16_t*>(bytes), length, [](int16_t sample) {
                return choc::value::createInt32(static_cast<int32_t>(sample));
            });
        case kJSTypedArrayTypeInt32Array:
            return typed_array_to_choc_array(reinterpret_cast<const int32_t*>(bytes), length, [](int32_t sample) {
                return choc::value::createInt32(sample);
            });
        case kJSTypedArrayTypeUint8Array:
        case kJSTypedArrayTypeUint8ClampedArray:
            return typed_array_to_choc_array(reinterpret_cast<const uint8_t*>(bytes), length, [](uint8_t sample) {
                return choc::value::createInt32(static_cast<int32_t>(sample));
            });
        case kJSTypedArrayTypeUint16Array:
            return typed_array_to_choc_array(reinterpret_cast<const uint16_t*>(bytes), length, [](uint16_t sample) {
                return choc::value::createInt32(static_cast<int32_t>(sample));
            });
        case kJSTypedArrayTypeUint32Array:
            return typed_array_to_choc_array(reinterpret_cast<const uint32_t*>(bytes), length, [](uint32_t sample) {
                return choc::value::createInt64(static_cast<int64_t>(sample));
            });
        case kJSTypedArrayTypeFloat32Array:
            return typed_array_to_choc_array(reinterpret_cast<const float*>(bytes), length, [](float sample) {
                return choc::value::createFloat64(static_cast<double>(sample));
            });
        case kJSTypedArrayTypeFloat64Array:
            return typed_array_to_choc_array(reinterpret_cast<const double*>(bytes), length, [](double sample) {
                return choc::value::createFloat64(sample);
            });
        case kJSTypedArrayTypeBigInt64Array:
            return typed_array_to_choc_array(reinterpret_cast<const int64_t*>(bytes), length, [](int64_t sample) {
                return choc::value::createInt64(sample);
            });
        case kJSTypedArrayTypeBigUint64Array:
            return typed_array_to_choc_array(reinterpret_cast<const uint64_t*>(bytes), length, [](uint64_t sample) {
                return choc::value::createFloat64(static_cast<double>(sample));
            });
        case kJSTypedArrayTypeNone:
        case kJSTypedArrayTypeArrayBuffer:
            break;
    }

    return {};
}

// ── Value conversion: JSC → choc ────────────────────────────────────────────

static choc::value::Value jsc_to_choc(JSContext* ctx, JSValue* value) {
    if (value == nil || [value isUndefined] || [value isNull])
        return {};

    if ([value isBoolean])
        return choc::value::createBool([value toBool]);

    if ([value isNumber]) {
        double d = [value toDouble];
        if (d == static_cast<double>(static_cast<int32_t>(d)) && d >= INT32_MIN && d <= INT32_MAX)
            return choc::value::createInt32(static_cast<int32_t>(d));
        return choc::value::createFloat64(d);
    }

    if ([value isString])
        return choc::value::createString(std::string([[value toString] UTF8String]));

    auto typed_array = jsc_typed_array_to_choc(ctx, value);
    if (!typed_array.isVoid())
        return typed_array;

    if ([value isArray]) {
        auto arr = choc::value::createEmptyArray();
        int length = [[value valueForProperty:@"length"] toInt32];
        for (int i = 0; i < length; ++i)
            arr.addArrayElement(jsc_to_choc(ctx, [value valueAtIndex:i]));
        return arr;
    }

    if ([value isObject]) {
        JSValue* stringify = [ctx evaluateScript:@"JSON.stringify"];
        JSValue* jsonStr = [stringify callWithArguments:@[value]];
        if (jsonStr && [jsonStr isString]) {
            try {
                return choc::json::parse(std::string([[jsonStr toString] UTF8String]));
            } catch (...) {}
        }
        return choc::value::createObject("Object");
    }

    return {};
}

// ── Value conversion: choc → JSC ────────────────────────────────────────────

static JSValue* choc_to_jsc(JSContext* ctx, const choc::value::Value& value) {
    if (value.isVoid())
        return [JSValue valueWithUndefinedInContext:ctx];

    if (value.isBool())
        return [JSValue valueWithBool:value.getWithDefault<bool>(false) inContext:ctx];

    if (value.isInt32())
        return [JSValue valueWithInt32:value.getWithDefault<int32_t>(0) inContext:ctx];

    if (value.isInt64())
        return [JSValue valueWithDouble:static_cast<double>(value.getWithDefault<int64_t>(0)) inContext:ctx];

    if (value.isFloat32() || value.isFloat64())
        return [JSValue valueWithDouble:value.getWithDefault<double>(0.0) inContext:ctx];

    if (value.isString())
        return [JSValue valueWithObject:
            [NSString stringWithUTF8String:std::string(value.getString()).c_str()]
            inContext:ctx];

    if (value.isArray()) {
        NSMutableArray* arr = [NSMutableArray arrayWithCapacity:value.size()];
        for (uint32_t i = 0; i < value.size(); ++i) {
            choc::value::Value elem(value[static_cast<int>(i)]);
            [arr addObject:choc_to_jsc(ctx, elem)];
        }
        return [JSValue valueWithObject:arr inContext:ctx];
    }

    if (value.isObject()) {
        try {
            auto jsonStr = choc::json::toString(value);
            NSString* nsJson = [NSString stringWithUTF8String:jsonStr.c_str()];
            NSString* expr = [NSString stringWithFormat:@"(%@)", nsJson];
            JSValue* parsed = [ctx evaluateScript:expr];
            if (parsed && ![parsed isUndefined])
                return parsed;
        } catch (...) {}
    }

    return [JSValue valueWithUndefinedInContext:ctx];
}

// ── C API function registration ─────────────────────────────────────────────
// We use JSC's C API for native function binding because the Obj-C block-based
// subscript API has complex ARC/block-lifetime interactions with C++ captures.
// This approach stores NativeFunction in a shared_ptr keyed by name, and uses
// a C callback trampoline that retrieves the function from a static map.

// Global registry of native functions keyed by (JSGlobalContextRef, name).
// This is safe because JSC engines are single-threaded.
struct JscFuncKey {
    JSGlobalContextRef ctx;
    std::string name;
    bool operator==(const JscFuncKey& o) const { return ctx == o.ctx && name == o.name; }
};
struct JscFuncKeyHash {
    size_t operator()(const JscFuncKey& k) const {
        return std::hash<void*>()(k.ctx) ^ std::hash<std::string>()(k.name);
    }
};

static std::unordered_map<JscFuncKey, std::shared_ptr<NativeFunction>, JscFuncKeyHash>&
jsc_func_registry() {
    static std::unordered_map<JscFuncKey, std::shared_ptr<NativeFunction>, JscFuncKeyHash> reg;
    return reg;
}

static JSValueRef jsc_native_trampoline(
    JSContextRef ctx, JSObjectRef function, JSObjectRef thisObject,
    size_t argumentCount, const JSValueRef arguments[], JSValueRef* exception)
{
    // Retrieve the function name from the JS function object
    JSStringRef nameRef = JSStringCreateWithUTF8CString("name");
    JSValueRef nameVal = JSObjectGetProperty(ctx, function, nameRef, nullptr);
    JSStringRelease(nameRef);

    JSStringRef nameStr = JSValueToStringCopy(ctx, nameVal, nullptr);
    size_t len = JSStringGetMaximumUTF8CStringSize(nameStr);
    std::string name(len, '\0');
    JSStringGetUTF8CString(nameStr, name.data(), len);
    JSStringRelease(nameStr);
    name.resize(strlen(name.c_str()));

    auto& registry = jsc_func_registry();
    auto it = registry.find({JSContextGetGlobalContext(ctx), name});
    if (it == registry.end())
        return JSValueMakeUndefined(ctx);

    @autoreleasepool {
        JSContext* objcCtx = [JSContext contextWithJSGlobalContextRef:JSContextGetGlobalContext(ctx)];
        std::vector<choc::value::Value> args;
        args.reserve(argumentCount);
        for (size_t i = 0; i < argumentCount; ++i) {
            JSValue* val = [JSValue valueWithJSValueRef:arguments[i] inContext:objcCtx];
            args.push_back(jsc_to_choc(objcCtx, val));
        }

        try {
            choc::value::Value result = (*(it->second))(args.data(), args.size());
            JSValue* jsResult = choc_to_jsc(objcCtx, result);
            return [jsResult JSValueRef];
        } catch (const std::exception& e) {
            JSStringRef msg = JSStringCreateWithUTF8CString(e.what());
            *exception = JSValueMakeString(ctx, msg);
            JSStringRelease(msg);
            return JSValueMakeUndefined(ctx);
        }
    }
}

// ── JscEngine ───────────────────────────────────────────────────────────────

class JscEngine final : public JsEngine {
public:
    JscEngine() {
        @autoreleasepool {
            context_ = [[JSContext alloc] init];
            global_ctx_ = [context_ JSGlobalContextRef];
            setup_console();
        }
    }

    ~JscEngine() override {
        @autoreleasepool {
            // Clean up registered functions for this context
            auto& registry = jsc_func_registry();
            for (auto it = registry.begin(); it != registry.end(); ) {
                if (it->first.ctx == global_ctx_)
                    it = registry.erase(it);
                else
                    ++it;
            }
            context_ = nil;
        }
    }

    JsEngineType type() const override { return JsEngineType::jsc; }
    bool is_valid() const override { return context_ != nil; }

    choc::value::Value evaluate(const std::string& code) override {
        @autoreleasepool {
            NSString* script = [NSString stringWithUTF8String:code.c_str()];
            JSValue* result = [context_ evaluateScript:script];
            check_exception();
            return jsc_to_choc(context_, result);
        }
    }

    void run_module(const std::string& code, ModuleResolver /*resolver*/) override {
        @autoreleasepool {
            evaluate(code);
        }
    }

    void register_function(const std::string& name, NativeFunction fn) override {
        @autoreleasepool {
            // Store function in the global registry
            auto fn_ptr = std::make_shared<NativeFunction>(std::move(fn));
            jsc_func_registry()[{global_ctx_, name}] = fn_ptr;

            // Create a named JS function using the C API
            JSStringRef jsName = JSStringCreateWithUTF8CString(name.c_str());
            JSObjectRef funcObj = JSObjectMakeFunctionWithCallback(global_ctx_, jsName,
                                                                    jsc_native_trampoline);
            // Set as global property
            JSObjectRef globalObj = JSContextGetGlobalObject(global_ctx_);
            JSObjectSetProperty(global_ctx_, globalObj, jsName, funcObj,
                                kJSPropertyAttributeNone, nullptr);
            JSStringRelease(jsName);
        }
    }

    choc::value::Value invoke(std::string_view name) override {
        @autoreleasepool {
            NSString* nsName = [NSString stringWithUTF8String:std::string(name).c_str()];
            JSValue* func = context_[nsName];
            if (!func || [func isUndefined])
                throw std::runtime_error("Function not found: " + std::string(name));

            JSValue* result = [func callWithArguments:@[]];
            check_exception();
            return jsc_to_choc(context_, result);
        }
    }

    choc::value::Value invoke(std::string_view name,
                              const choc::value::Value* args,
                              size_t num_args) override {
        @autoreleasepool {
            NSString* nsName = [NSString stringWithUTF8String:std::string(name).c_str()];
            JSValue* func = context_[nsName];
            if (!func || [func isUndefined])
                throw std::runtime_error("Function not found: " + std::string(name));

            NSMutableArray* jsArgs = [NSMutableArray arrayWithCapacity:num_args];
            for (size_t i = 0; i < num_args; ++i)
                [jsArgs addObject:choc_to_jsc(context_, args[i])];

            JSValue* result = [func callWithArguments:jsArgs];
            check_exception();
            return jsc_to_choc(context_, result);
        }
    }

    void set_log_callback(LogCallback callback) override {
        log_callback_ = std::move(callback);
        setup_console();
    }

    void gc_hint() override {
        @autoreleasepool {
            if (context_)
                JSGarbageCollect(global_ctx_);
        }
    }

    bool supports_host_objects() const override { return true; }
    bool supports_typed_arrays() const override { return true; }

private:
    JSContext* context_ = nil;
    JSGlobalContextRef global_ctx_ = nullptr;
    LogCallback log_callback_;

    void check_exception() {
        JSValue* exception = context_.exception;
        if (exception && ![exception isUndefined] && ![exception isNull]) {
            std::string msg([[exception toString] UTF8String]);
            context_.exception = nil;
            throw std::runtime_error(msg);
        }
    }

    void setup_console() {
        @autoreleasepool {
            if (!context_) return;

            // Register console functions via the C API + registry pattern
            auto make_log_fn = [this](const char* level) -> NativeFunction {
                std::string lvl(level);
                return [this, lvl](const choc::value::Value* args, size_t count) -> choc::value::Value {
                    if (log_callback_ && count > 0) {
                        std::string text;
                        for (size_t i = 0; i < count; ++i) {
                            if (i > 0) text += ' ';
                            if (args[i].isString())
                                text += std::string(args[i].getString());
                            else if (args[i].isVoid())
                                text += "undefined";
                            else
                                text += args[i].toString();
                        }
                        log_callback_(lvl, text);
                    }
                    return {};
                };
            };

            // Create console object, then set methods
            [context_ evaluateScript:@"var console = {};"];

            // Register each console method as a global, then assign to console object
            register_function("__pulp_console_log", make_log_fn("log"));
            register_function("__pulp_console_info", make_log_fn("info"));
            register_function("__pulp_console_warn", make_log_fn("warn"));
            register_function("__pulp_console_error", make_log_fn("error"));
            register_function("__pulp_console_debug", make_log_fn("debug"));

            [context_ evaluateScript:@"console.log = __pulp_console_log;"
                                     @"console.info = __pulp_console_info;"
                                     @"console.warn = __pulp_console_warn;"
                                     @"console.error = __pulp_console_error;"
                                     @"console.debug = __pulp_console_debug;"];
        }
    }
};

std::unique_ptr<JsEngine> create_jsc_engine() {
    return std::make_unique<JscEngine>();
}

} // namespace pulp::view

#endif // __APPLE__
