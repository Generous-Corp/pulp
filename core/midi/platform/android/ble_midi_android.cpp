#if defined(__ANDROID__)

#include <pulp/midi/ble_midi.hpp>
#include <pulp/midi/ble_midi_registry.hpp>
#include <pulp/midi/device.hpp>
#include <pulp/midi/raw_midi_parser.hpp>
#include <pulp/platform/android/jni.hpp>

#include <android/log.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace pulp::midi {
namespace {

constexpr const char* kLogTag = "Pulp";

std::mutex port_change_mu;
std::map<const void*, MidiSystem::PortChangeCallback> port_change_callbacks;

void notify_port_change() {
    std::vector<MidiSystem::PortChangeCallback> callbacks;
    {
        std::lock_guard lock(port_change_mu);
        for (const auto& [owner, callback] : port_change_callbacks) {
            (void)owner;
            if (callback)
                callbacks.push_back(callback);
        }
    }
    for (const auto& callback : callbacks)
        callback();
}

std::string input_port_id(const std::string& id) {
    return "ble-midi-in:" + id;
}

std::string output_port_id(const std::string& id) {
    return "ble-midi-out:" + id;
}

BleMidiError error_from_int(jint value) {
    switch (value) {
    case 1:
        return BleMidiError::PermissionDenied;
    case 2:
        return BleMidiError::BluetoothOff;
    case 3:
        return BleMidiError::Unsupported;
    case 4:
        return BleMidiError::PeripheralNotFound;
    case 6:
        return BleMidiError::ServiceNotFound;
    case 7:
        return BleMidiError::Timeout;
    default:
        return BleMidiError::ConnectFailed;
    }
}

std::string from_jstring(JNIEnv* env, jstring value) {
    if (!value)
        return {};
    const char* chars = env->GetStringUTFChars(value, nullptr);
    if (!chars)
        return {};
    std::string result(chars);
    env->ReleaseStringUTFChars(value, chars);
    return result;
}

struct CentralState {
    mutable std::mutex mu;
    BleMidiScanCallback scan_callback;
    BleMidiStateCallback state_callback;
    std::map<std::string, BleMidiPeripheral> peripherals;
    std::set<std::string> owned_connections;
    std::set<std::string> pending_connections;
    bool scanning = false;
};

class AndroidBleBridge {
  public:
    static AndroidBleBridge& instance() {
        static AndroidBleBridge bridge;
        return bridge;
    }

    void install(JNIEnv* env, jobject bridge) {
        std::lock_guard lock(mu_);
        if (bridge_)
            env->DeleteGlobalRef(bridge_);
        bridge_ = env->NewGlobalRef(bridge);
        const auto cls = env->GetObjectClass(bridge);
        is_available_ = env->GetMethodID(cls, "isNativeAvailable", "()Z");
        start_scan_ = env->GetMethodID(cls, "startNativeScan", "()Z");
        stop_scan_ = env->GetMethodID(cls, "stopNativeScan", "()V");
        connect_ = env->GetMethodID(cls, "connectNative", "(Ljava/lang/String;)Z");
        disconnect_ = env->GetMethodID(cls, "disconnectNative", "(Ljava/lang/String;)V");
        send_ = env->GetMethodID(cls, "sendNative", "(Ljava/lang/String;[B)Z");
        env->DeleteLocalRef(cls);
    }

    void add_state(const std::shared_ptr<CentralState>& state) {
        std::lock_guard lock(mu_);
        states_.push_back(state);
    }

    bool is_available() const {
        return call_boolean(is_available_, nullptr);
    }

    bool start_scan(const std::shared_ptr<CentralState>& state) {
        bool start_platform = false;
        {
            std::lock_guard lock(mu_);
            std::erase_if(scan_states_, [](const auto& item) { return item.expired(); });
            start_platform = scan_states_.empty();
            scan_states_.push_back(state);
        }
        if (!start_platform || call_boolean(start_scan_, nullptr))
            return true;
        std::lock_guard lock(mu_);
        std::erase_if(scan_states_, [&](const auto& item) {
            const auto live = item.lock();
            return !live || live == state;
        });
        return false;
    }

    void stop_scan(const std::shared_ptr<CentralState>& state) {
        bool stop_platform = false;
        {
            std::lock_guard lock(mu_);
            std::erase_if(scan_states_, [&](const auto& item) {
                const auto live = item.lock();
                return !live || live == state;
            });
            stop_platform = scan_states_.empty();
        }
        if (stop_platform)
            call_void(stop_scan_, nullptr);
    }

    bool connect(const std::string& id) const {
        return call_boolean(connect_, &id);
    }

    void release_connection(const std::string& id, const std::shared_ptr<CentralState>& owner) {
        BleMidiStateCallback callback;
        bool release_platform = true;
        {
            std::lock_guard lock(owner->mu);
            owner->pending_connections.erase(id);
            owner->owned_connections.erase(id);
            callback = owner->state_callback;
        }
        for_each_state([&](const std::shared_ptr<CentralState>& state) {
            if (state == owner)
                return;
            std::lock_guard lock(state->mu);
            if (state->pending_connections.contains(id) || state->owned_connections.contains(id))
                release_platform = false;
        });
        if (callback)
            callback(id, BleMidiConnectionState::Disconnected, BleMidiError::None);
        if (release_platform)
            call_void(disconnect_, &id);
    }

    void send(const std::string& id, const std::vector<uint8_t>& bytes) const {
        if (bytes.empty())
            return;
        JNIEnv* env = pulp::android::get_env();
        jobject bridge = local_bridge(env);
        if (!bridge || !send_)
            return;
        jstring java_id = env->NewStringUTF(id.c_str());
        jbyteArray data = env->NewByteArray(static_cast<jsize>(bytes.size()));
        if (data) {
            env->SetByteArrayRegion(data, 0, static_cast<jsize>(bytes.size()),
                                    reinterpret_cast<const jbyte*>(bytes.data()));
            env->CallBooleanMethod(bridge, send_, java_id, data);
        }
        clear_exception(env, "sendNative");
        if (data)
            env->DeleteLocalRef(data);
        env->DeleteLocalRef(java_id);
        env->DeleteLocalRef(bridge);
    }

    void peripheral_found(const BleMidiPeripheral& peripheral) {
        for_each_state([&](const std::shared_ptr<CentralState>& state) {
            BleMidiScanCallback callback;
            {
                std::lock_guard lock(state->mu);
                state->peripherals[peripheral.id] = peripheral;
                if (state->scanning)
                    callback = state->scan_callback;
            }
            if (callback)
                callback(peripheral);
        });
    }

    void connected(const std::string& id, const std::string& name, bool has_input,
                   bool has_output) {
        bool registry_changed = false;
        bool had_input = false;
        bool had_output = false;
        {
            std::lock_guard lock(mu_);
            auto [found, inserted] = connections_.try_emplace(
                id, Connection{name, has_input, has_output, std::make_shared<InputParser>(),
                               monotonic_nanoseconds()});
            if (!inserted) {
                had_input = found->second.has_input;
                had_output = found->second.has_output;
                registry_changed = found->second.name != name || had_input != has_input ||
                                   had_output != has_output;
                found->second.name = name;
                found->second.has_input = has_input;
                found->second.has_output = has_output;
            } else {
                registry_changed = true;
                ++registration_events_;
            }
        }
        auto& registry = BleMidiPortRegistry::instance();
        if (had_input && !has_input)
            registry.unregister_input(input_port_id(id));
        if (had_output && !has_output)
            registry.unregister_output(output_port_id(id));
        if (has_input)
            registry.register_input(input_port_id(id), name);
        if (has_output) {
            registry.register_output(output_port_id(id), name,
                                     [id](const std::vector<uint8_t>& bytes) {
                                         AndroidBleBridge::instance().send(id, bytes);
                                     });
        }
        if (registry_changed)
            notify_port_change();
        for_each_owner(id, true, [&](const std::shared_ptr<CentralState>& state) {
            BleMidiStateCallback callback;
            bool completed = false;
            {
                std::lock_guard lock(state->mu);
                completed = state->pending_connections.erase(id) != 0;
                if (completed) {
                    state->owned_connections.insert(id);
                    callback = state->state_callback;
                }
            }
            if (completed && callback)
                callback(id, BleMidiConnectionState::Connected, BleMidiError::None);
        });
    }

    void connection_failed(const std::string& id, BleMidiError error) {
        for_each_owner(id, true, [&](const std::shared_ptr<CentralState>& state) {
            BleMidiStateCallback callback;
            {
                std::lock_guard lock(state->mu);
                state->pending_connections.erase(id);
                state->owned_connections.erase(id);
                callback = state->state_callback;
            }
            if (callback)
                callback(id, BleMidiConnectionState::Failed, error);
        });
    }

    void scan_stopped(BleMidiError error) {
        {
            std::lock_guard lock(mu_);
            scan_states_.clear();
        }
        for_each_state([&](const std::shared_ptr<CentralState>& state) {
            BleMidiStateCallback callback;
            {
                std::lock_guard lock(state->mu);
                if (!std::exchange(state->scanning, false))
                    return;
                callback = state->state_callback;
            }
            if (callback)
                callback({}, BleMidiConnectionState::Failed, error);
        });
    }

    void disconnected(const std::string& id) {
        {
            std::lock_guard lock(mu_);
            connections_.erase(id);
            ++teardown_events_;
        }
        auto& registry = BleMidiPortRegistry::instance();
        registry.unregister_input(input_port_id(id));
        registry.unregister_output(output_port_id(id));
        notify_port_change();
        for_each_owner(id, true, [&](const std::shared_ptr<CentralState>& state) {
            BleMidiStateCallback callback;
            {
                std::lock_guard lock(state->mu);
                state->pending_connections.erase(id);
                state->owned_connections.erase(id);
                callback = state->state_callback;
            }
            if (callback)
                callback(id, BleMidiConnectionState::Disconnected, BleMidiError::None);
        });
    }

    void midi_received(const std::string& id, const std::vector<uint8_t>& bytes,
                       int64_t timestamp_ns) {
        std::shared_ptr<InputParser> parser;
        {
            std::lock_guard lock(mu_);
            const auto found = connections_.find(id);
            if (found == connections_.end())
                return;
            parser = found->second.parser;
        }
        const auto event_ns = timestamp_ns > 0 ? timestamp_ns : monotonic_nanoseconds();
        const auto elapsed_ns = std::max<int64_t>(0, event_ns - connection_timestamp_origin(id));
        const auto timestamp = static_cast<double>(elapsed_ns) / 1'000'000'000.0;
        const auto port = input_port_id(id);
        std::lock_guard lock(parser->mu);
        parse_raw_midi_bytes(
            bytes.data(), bytes.size(), parser->state,
            [port, timestamp](uint8_t status, uint8_t d1, uint8_t d2) {
                BleMidiPortRegistry::instance().deliver_message(port, {status, d1, d2}, timestamp);
            },
            [port, timestamp](const std::vector<uint8_t>& sysex) {
                BleMidiPortRegistry::instance().deliver_message(port, sysex, timestamp);
            });
    }

    int registry_flags(const std::string& id) const {
        auto& registry = BleMidiPortRegistry::instance();
        return (registry.is_input(input_port_id(id)) ? 1 : 0) |
               (registry.is_output(output_port_id(id)) ? 2 : 0);
    }

    void reset_validation() {
        {
            std::lock_guard lock(validation_mu_);
            validation_received_.clear();
            validation_received_at_ = 0.0;
        }
        {
            std::lock_guard lock(mu_);
            registration_events_ = 0;
            teardown_events_ = 0;
        }
    }

    bool attach_validation_input(const std::string& id) {
        auto system = create_midi_system();
        validation_input_ = system->create_input();
        validation_input_->set_sysex_callback(
            [this](const std::vector<uint8_t>& bytes, double timestamp) {
                std::lock_guard lock(validation_mu_);
                validation_received_ = bytes;
                validation_received_at_ = timestamp;
            });
        return validation_input_->open(input_port_id(id), [this](const MidiEvent& event) {
            const auto message = event.message;
            std::lock_guard lock(validation_mu_);
            validation_received_ = {message.data()[0], message.data()[1], message.data()[2]};
            validation_received_at_ = event.timestamp;
        });
    }

    bool validation_received(const std::vector<uint8_t>& expected) const {
        std::lock_guard lock(validation_mu_);
        return validation_received_ == expected;
    }

    double validation_received_at() const {
        std::lock_guard lock(validation_mu_);
        return validation_received_at_;
    }

    bool send_validation_output(const std::string& id, const std::vector<uint8_t>& bytes) const {
        if (bytes.empty() || bytes.size() > 3)
            return false;
        auto system = create_midi_system();
        auto output = system->create_output();
        if (!output->open(output_port_id(id)))
            return false;
        MidiEvent event;
        event.message = choc::midi::ShortMessage(bytes[0], bytes.size() > 1 ? bytes[1] : 0,
                                                 bytes.size() > 2 ? bytes[2] : 0);
        output->send(event);
        return true;
    }

    std::pair<int, int> registry_event_counts() const {
        std::lock_guard lock(mu_);
        return {registration_events_, teardown_events_};
    }

  private:
    struct InputParser {
        std::mutex mu;
        RawMidiParserState state;
    };

    struct Connection {
        std::string name;
        bool has_input = false;
        bool has_output = false;
        std::shared_ptr<InputParser> parser;
        int64_t timestamp_origin_ns = 0;
    };

    static int64_t monotonic_nanoseconds() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    int64_t connection_timestamp_origin(const std::string& id) const {
        std::lock_guard lock(mu_);
        const auto found = connections_.find(id);
        return found == connections_.end() ? monotonic_nanoseconds()
                                           : found->second.timestamp_origin_ns;
    }

    template <typename Callback>
    void for_each_owner(const std::string& id, bool include_pending, Callback&& callback) {
        for_each_state([&](const std::shared_ptr<CentralState>& state) {
            bool matches = false;
            {
                std::lock_guard lock(state->mu);
                matches = state->owned_connections.contains(id) ||
                          (include_pending && state->pending_connections.contains(id));
            }
            if (matches)
                callback(state);
        });
    }

    jobject local_bridge(JNIEnv* env) const {
        std::lock_guard lock(mu_);
        return bridge_ ? env->NewLocalRef(bridge_) : nullptr;
    }

    bool call_boolean(jmethodID method, const std::string* id) const {
        JNIEnv* env = pulp::android::get_env();
        jobject bridge = local_bridge(env);
        if (!bridge || !method)
            return false;
        jboolean result = JNI_FALSE;
        if (id) {
            jstring java_id = env->NewStringUTF(id->c_str());
            result = env->CallBooleanMethod(bridge, method, java_id);
            env->DeleteLocalRef(java_id);
        } else {
            result = env->CallBooleanMethod(bridge, method);
        }
        clear_exception(env, "BLE MIDI bridge call");
        env->DeleteLocalRef(bridge);
        return result == JNI_TRUE;
    }

    void call_void(jmethodID method, const std::string* id) const {
        JNIEnv* env = pulp::android::get_env();
        jobject bridge = local_bridge(env);
        if (!bridge || !method)
            return;
        if (id) {
            jstring java_id = env->NewStringUTF(id->c_str());
            env->CallVoidMethod(bridge, method, java_id);
            env->DeleteLocalRef(java_id);
        } else {
            env->CallVoidMethod(bridge, method);
        }
        clear_exception(env, "BLE MIDI bridge call");
        env->DeleteLocalRef(bridge);
    }

    static void clear_exception(JNIEnv* env, const char* operation) {
        if (!env->ExceptionCheck())
            return;
        env->ExceptionDescribe();
        env->ExceptionClear();
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "%s raised a Java exception", operation);
    }

    template <typename Callback> void for_each_state(Callback&& callback) {
        std::vector<std::shared_ptr<CentralState>> live;
        {
            std::lock_guard lock(mu_);
            std::erase_if(states_, [](const auto& state) { return state.expired(); });
            live.reserve(states_.size());
            for (const auto& state : states_) {
                if (auto shared = state.lock())
                    live.push_back(std::move(shared));
            }
        }
        for (const auto& state : live)
            callback(state);
    }

    void fire_state(const std::string& id, BleMidiConnectionState connection, BleMidiError error) {
        for_each_state([&](const std::shared_ptr<CentralState>& state) {
            BleMidiStateCallback callback;
            {
                std::lock_guard lock(state->mu);
                callback = state->state_callback;
            }
            if (callback)
                callback(id, connection, error);
        });
    }

    mutable std::mutex mu_;
    jobject bridge_ = nullptr;
    jmethodID is_available_ = nullptr;
    jmethodID start_scan_ = nullptr;
    jmethodID stop_scan_ = nullptr;
    jmethodID connect_ = nullptr;
    jmethodID disconnect_ = nullptr;
    jmethodID send_ = nullptr;
    std::vector<std::weak_ptr<CentralState>> states_;
    std::vector<std::weak_ptr<CentralState>> scan_states_;
    std::map<std::string, Connection> connections_;
    int registration_events_ = 0;
    int teardown_events_ = 0;

    mutable std::mutex validation_mu_;
    std::vector<uint8_t> validation_received_;
    double validation_received_at_ = 0.0;
    std::unique_ptr<MidiInput> validation_input_;
};

class AndroidBleMidiCentral final : public BleMidiCentral {
  public:
    AndroidBleMidiCentral() : state_(std::make_shared<CentralState>()) {
        AndroidBleBridge::instance().add_state(state_);
    }

    ~AndroidBleMidiCentral() override {
        stop_scan();
        std::vector<std::string> ids;
        {
            std::lock_guard lock(state_->mu);
            ids.assign(state_->owned_connections.begin(), state_->owned_connections.end());
            ids.insert(ids.end(), state_->pending_connections.begin(),
                       state_->pending_connections.end());
        }
        for (const auto& id : ids)
            disconnect(id);
    }

    bool is_available() const override {
        return AndroidBleBridge::instance().is_available();
    }

    bool start_scan(BleMidiScanCallback callback) override {
        {
            std::lock_guard lock(state_->mu);
            if (state_->scanning)
                return true;
            state_->scan_callback = std::move(callback);
            state_->scanning = true;
        }
        if (!AndroidBleBridge::instance().start_scan(state_)) {
            std::lock_guard lock(state_->mu);
            state_->scanning = false;
            return false;
        }
        return true;
    }

    void stop_scan() override {
        bool was_scanning = false;
        {
            std::lock_guard lock(state_->mu);
            was_scanning = std::exchange(state_->scanning, false);
            state_->scan_callback = nullptr;
        }
        if (was_scanning)
            AndroidBleBridge::instance().stop_scan(state_);
    }

    bool is_scanning() const override {
        std::lock_guard lock(state_->mu);
        return state_->scanning;
    }

    std::vector<BleMidiPeripheral> known_peripherals() const override {
        std::lock_guard lock(state_->mu);
        std::vector<BleMidiPeripheral> result;
        result.reserve(state_->peripherals.size());
        for (const auto& [id, peripheral] : state_->peripherals) {
            (void)id;
            result.push_back(peripheral);
        }
        return result;
    }

    bool connect(const std::string& id) override {
        BleMidiStateCallback missing_callback;
        bool missing = false;
        {
            std::lock_guard lock(state_->mu);
            if (!state_->peripherals.contains(id)) {
                missing_callback = state_->state_callback;
                missing = true;
            } else if (state_->owned_connections.contains(id)) {
                return true;
            }
        }
        if (missing) {
            if (missing_callback) {
                missing_callback(id, BleMidiConnectionState::Failed,
                                 BleMidiError::PeripheralNotFound);
            }
            return false;
        }
        {
            std::lock_guard lock(state_->mu);
            state_->pending_connections.insert(id);
        }
        fire_state(id, BleMidiConnectionState::Connecting, BleMidiError::None);
        if (!AndroidBleBridge::instance().connect(id)) {
            std::lock_guard lock(state_->mu);
            state_->pending_connections.erase(id);
            return false;
        }
        return true;
    }

    void disconnect(const std::string& id) override {
        {
            std::lock_guard lock(state_->mu);
            if (!state_->owned_connections.contains(id) &&
                !state_->pending_connections.contains(id))
                return;
        }
        AndroidBleBridge::instance().release_connection(id, state_);
    }

    void set_state_callback(BleMidiStateCallback callback) override {
        std::lock_guard lock(state_->mu);
        state_->state_callback = std::move(callback);
    }

    std::string midi_input_port_for(const std::string& id) const override {
        auto& registry = BleMidiPortRegistry::instance();
        const auto port = input_port_id(id);
        return registry.is_input(port) ? port : std::string{};
    }

    std::string midi_output_port_for(const std::string& id) const override {
        auto& registry = BleMidiPortRegistry::instance();
        const auto port = output_port_id(id);
        return registry.is_output(port) ? port : std::string{};
    }

  private:
    void fire_state(const std::string& id, BleMidiConnectionState connection, BleMidiError error) {
        BleMidiStateCallback callback;
        {
            std::lock_guard lock(state_->mu);
            callback = state_->state_callback;
        }
        if (callback)
            callback(id, connection, error);
    }

    std::shared_ptr<CentralState> state_;
};

class AndroidBleMidiInput final : public MidiInput {
  public:
    ~AndroidBleMidiInput() override {
        close();
    }

    bool open(const std::string& port_id, MidiInputCallback callback) override {
        port_id_ = port_id;
        open_ = BleMidiPortRegistry::instance().attach_input(port_id, std::move(callback), sysex_);
        return open_;
    }

    void close() override {
        if (open_)
            BleMidiPortRegistry::instance().detach_input(port_id_);
        open_ = false;
        port_id_.clear();
    }

    bool is_open() const override {
        return open_;
    }
    void set_sysex_callback(MidiSysexCallback callback) override {
        sysex_ = std::move(callback);
    }

  private:
    std::string port_id_;
    MidiSysexCallback sysex_;
    bool open_ = false;
};

class AndroidBleMidiOutput final : public MidiOutput {
  public:
    bool open(const std::string& port_id) override {
        sink_ = BleMidiPortRegistry::instance().output_sink(port_id);
        return static_cast<bool>(sink_);
    }
    void close() override {
        sink_ = nullptr;
    }
    bool is_open() const override {
        return static_cast<bool>(sink_);
    }
    void send(const MidiEvent& event) override {
        if (!sink_)
            return;
        const auto* bytes = event.data();
        const auto size = event.size();
        sink_(std::vector<uint8_t>(bytes, bytes + size));
    }

  private:
    std::function<void(const std::vector<uint8_t>&)> sink_;
};

class AndroidMidiSystem final : public MidiSystem {
  public:
    ~AndroidMidiSystem() override {
        std::lock_guard lock(port_change_mu);
        port_change_callbacks.erase(this);
    }
    std::vector<MidiPortInfo> enumerate_inputs() override {
        return BleMidiPortRegistry::instance().list_inputs();
    }
    std::vector<MidiPortInfo> enumerate_outputs() override {
        return BleMidiPortRegistry::instance().list_outputs();
    }
    std::unique_ptr<MidiInput> create_input() override {
        return std::make_unique<AndroidBleMidiInput>();
    }
    std::unique_ptr<MidiOutput> create_output() override {
        return std::make_unique<AndroidBleMidiOutput>();
    }
    void set_port_change_callback(PortChangeCallback callback) override {
        std::lock_guard lock(port_change_mu);
        if (callback)
            port_change_callbacks[this] = std::move(callback);
        else
            port_change_callbacks.erase(this);
    }
};

std::unique_ptr<BleMidiCentral> validation_central;

std::vector<uint8_t> from_jbytes(JNIEnv* env, jbyteArray data, jint offset = 0, jint count = -1) {
    if (!data)
        return {};
    const jsize size = env->GetArrayLength(data);
    if (count < 0)
        count = size - offset;
    if (offset < 0 || count < 0 || offset > size || count > size - offset)
        return {};
    std::vector<uint8_t> result(static_cast<std::size_t>(count));
    if (count > 0) {
        env->GetByteArrayRegion(data, offset, count, reinterpret_cast<jbyte*>(result.data()));
    }
    return result;
}

} // namespace

std::unique_ptr<BleMidiCentral> create_ble_midi_central() {
    return std::make_unique<AndroidBleMidiCentral>();
}

std::unique_ptr<MidiSystem> create_midi_system() {
    return std::make_unique<AndroidMidiSystem>();
}

} // namespace pulp::midi

#if defined(PULP_ANDROID_BLE_VALIDATION_LIBRARY)
extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        return JNI_ERR;
    }
    pulp::android::g_vm = vm;
    return JNI_VERSION_1_6;
}
#endif

extern "C" JNIEXPORT void
    JNICALL Java_com_pulp_midi_PulpBluetoothMidi_nativeInstallBridge(JNIEnv* env, jobject bridge) {
    pulp::midi::AndroidBleBridge::instance().install(env, bridge);
}

extern "C" JNIEXPORT void JNICALL Java_com_pulp_midi_PulpBluetoothMidi_nativeOnPeripheralFound(
    JNIEnv* env, jobject, jstring id, jstring name, jint rssi, jboolean paired) {
    pulp::midi::BleMidiPeripheral peripheral;
    peripheral.id = pulp::midi::from_jstring(env, id);
    peripheral.name = pulp::midi::from_jstring(env, name);
    peripheral.rssi = rssi;
    peripheral.last_seen = std::chrono::steady_clock::now();
    peripheral.is_paired = paired == JNI_TRUE;
    pulp::midi::AndroidBleBridge::instance().peripheral_found(peripheral);
}

extern "C" JNIEXPORT void JNICALL Java_com_pulp_midi_PulpBluetoothMidi_nativeOnConnected(
    JNIEnv* env, jobject, jstring id, jstring name, jboolean has_input, jboolean has_output) {
    pulp::midi::AndroidBleBridge::instance().connected(
        pulp::midi::from_jstring(env, id), pulp::midi::from_jstring(env, name),
        has_input == JNI_TRUE, has_output == JNI_TRUE);
}

extern "C" JNIEXPORT void JNICALL Java_com_pulp_midi_PulpBluetoothMidi_nativeOnConnectionFailed(
    JNIEnv* env, jobject, jstring id, jint error) {
    pulp::midi::AndroidBleBridge::instance().connection_failed(pulp::midi::from_jstring(env, id),
                                                               pulp::midi::error_from_int(error));
}

extern "C" JNIEXPORT void JNICALL
Java_com_pulp_midi_PulpBluetoothMidi_nativeOnDisconnected(JNIEnv* env, jobject, jstring id) {
    pulp::midi::AndroidBleBridge::instance().disconnected(pulp::midi::from_jstring(env, id));
}

extern "C" JNIEXPORT void JNICALL
Java_com_pulp_midi_PulpBluetoothMidi_nativeOnScanStopped(JNIEnv*, jobject, jint error) {
    pulp::midi::AndroidBleBridge::instance().scan_stopped(pulp::midi::error_from_int(error));
}

extern "C" JNIEXPORT void JNICALL Java_com_pulp_midi_PulpBluetoothMidi_nativeOnMidiReceived(
    JNIEnv* env, jobject, jstring id, jbyteArray data, jint offset, jint count,
    jlong timestamp_ns) {
    pulp::midi::AndroidBleBridge::instance().midi_received(
        pulp::midi::from_jstring(env, id), pulp::midi::from_jbytes(env, data, offset, count),
        timestamp_ns);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_pulp_midi_PulpBluetoothMidi_nativeBeginRegistryValidation(JNIEnv*, jobject) {
    auto& bridge = pulp::midi::AndroidBleBridge::instance();
    bridge.reset_validation();
    pulp::midi::validation_central = pulp::midi::create_ble_midi_central();
    return pulp::midi::validation_central->start_scan([](const auto&) {}) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_pulp_midi_PulpBluetoothMidi_nativeConnectRegistryValidation(JNIEnv* env, jobject,
                                                                     jstring id) {
    if (!pulp::midi::validation_central)
        return JNI_FALSE;
    return pulp::midi::validation_central->connect(pulp::midi::from_jstring(env, id)) ? JNI_TRUE
                                                                                      : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_pulp_midi_PulpBluetoothMidi_nativeAttachRegistryValidationInput(JNIEnv* env, jobject,
                                                                         jstring id) {
    return pulp::midi::AndroidBleBridge::instance().attach_validation_input(
               pulp::midi::from_jstring(env, id))
               ? JNI_TRUE
               : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_pulp_midi_PulpBluetoothMidi_nativeRegistryValidationReceived(JNIEnv* env, jobject,
                                                                      jbyteArray expected) {
    return pulp::midi::AndroidBleBridge::instance().validation_received(
               pulp::midi::from_jbytes(env, expected))
               ? JNI_TRUE
               : JNI_FALSE;
}

extern "C" JNIEXPORT jdouble JNICALL
Java_com_pulp_midi_PulpBluetoothMidi_nativeRegistryValidationReceivedAt(JNIEnv*, jobject) {
    return pulp::midi::AndroidBleBridge::instance().validation_received_at();
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_pulp_midi_PulpBluetoothMidi_nativeSendRegistryValidationOutput(JNIEnv* env, jobject,
                                                                        jstring id,
                                                                        jbyteArray bytes) {
    return pulp::midi::AndroidBleBridge::instance().send_validation_output(
               pulp::midi::from_jstring(env, id), pulp::midi::from_jbytes(env, bytes))
               ? JNI_TRUE
               : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_pulp_midi_PulpBluetoothMidi_nativeDisconnectRegistryValidation(JNIEnv* env, jobject,
                                                                        jstring id) {
    if (!pulp::midi::validation_central)
        return;
    pulp::midi::validation_central->disconnect(pulp::midi::from_jstring(env, id));
    pulp::midi::validation_central.reset();
}

extern "C" JNIEXPORT jint JNICALL
Java_com_pulp_midi_PulpBluetoothMidi_nativeRegistryFlags(JNIEnv* env, jobject, jstring id) {
    return pulp::midi::AndroidBleBridge::instance().registry_flags(
        pulp::midi::from_jstring(env, id));
}

extern "C" JNIEXPORT jintArray JNICALL
Java_com_pulp_midi_PulpBluetoothMidi_nativeRegistryEventCounts(JNIEnv* env, jobject) {
    const auto [registered, torn_down] =
        pulp::midi::AndroidBleBridge::instance().registry_event_counts();
    const jint values[] = {registered, torn_down};
    jintArray result = env->NewIntArray(2);
    if (result)
        env->SetIntArrayRegion(result, 0, 2, values);
    return result;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_pulp_midi_PulpBluetoothMidi_nativeRegistryValidationIsScanning(JNIEnv*, jobject) {
    return pulp::midi::validation_central && pulp::midi::validation_central->is_scanning()
               ? JNI_TRUE
               : JNI_FALSE;
}

#endif // __ANDROID__
