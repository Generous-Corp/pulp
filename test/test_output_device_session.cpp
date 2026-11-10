#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/output_device_session.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

using namespace pulp::audio;

namespace {

struct FakeBackend;

// A device whose callback is driven by the test through pump(). stop() waits for
// an in-flight pump(), as a real backend's stop() waits for the render block.
class FakeDevice : public AudioDevice {
  public:
    FakeDevice(FakeBackend& backend, std::string id) : backend_(backend), id_(std::move(id)) {}
    ~FakeDevice() override;

    bool open(const DeviceConfig& c) override {
        std::lock_guard<std::mutex> lk(m_);
        opened_ = true;
        sr_ = c.sample_rate;
        return true;
    }
    void close() override {
        ++close_calls;
        std::lock_guard<std::mutex> lk(m_);
        opened_ = false;
    }
    bool start(AudioCallback cb) override;
    void stop() override;
    bool is_open() const override {
        std::lock_guard<std::mutex> lk(m_);
        return opened_;
    }
    bool is_running() const override {
        std::lock_guard<std::mutex> lk(m_);
        return running_;
    }
    DeviceInfo info() const override {
        DeviceInfo i;
        i.id = id_;
        return i;
    }
    double sample_rate() const override {
        return sr_;
    }
    int buffer_size() const override {
        return 64;
    }

    // Run one callback block. Returns false when the device is not running.
    bool pump();
    // The device disappears under the handle: it stops being open.
    void yank();

    std::atomic<bool> in_callback{false};
    std::atomic<int> blocks{0};
    std::function<void()> on_destroy;
    std::atomic<int> stop_calls{0};
    std::atomic<int> close_calls{0};

  private:
    FakeBackend& backend_;
    std::string id_;
    mutable std::mutex m_;
    std::mutex run_;  // held for the duration of one callback block
    AudioCallback cb_;
    bool opened_ = false;
    bool running_ = false;
    double sr_ = 0;
};

struct FakeBackend : AudioSystem {
    std::mutex m;
    std::vector<DeviceInfo> devices;
    std::string default_id;
    std::map<std::string, FakeDevice*> live;  // handles that exist
    std::atomic<int> started{0};              // callbacks currently live
    std::atomic<int> max_started{0};
    std::atomic<int> after_stop_calls{0};

    void add(const std::string& id, int outs = 2) {
        std::lock_guard<std::mutex> lk(m);
        DeviceInfo d;
        d.id = id;
        d.name = id;
        d.max_output_channels = outs;
        devices.push_back(d);
    }
    void remove(const std::string& id) {
        std::lock_guard<std::mutex> lk(m);
        for (auto it = devices.begin(); it != devices.end(); ++it)
            if (it->id == id) {
                devices.erase(it);
                break;
            }
    }
    std::vector<DeviceInfo> enumerate_devices() override {
        std::lock_guard<std::mutex> lk(m);
        return devices;
    }
    std::unique_ptr<AudioDevice> create_device(const std::string& id) override {
        auto d = std::make_unique<FakeDevice>(*this, id);
        std::lock_guard<std::mutex> lk(m);
        live[id] = d.get();
        return d;
    }
    DeviceInfo default_output_device() override {
        std::lock_guard<std::mutex> lk(m);
        DeviceInfo d;
        d.id = default_id;
        return d;
    }
    DeviceInfo default_input_device() override {
        return {};
    }
    FakeDevice* handle(const std::string& id) {
        std::lock_guard<std::mutex> lk(m);
        auto it = live.find(id);
        return it == live.end() ? nullptr : it->second;
    }
    void note_started() {
        const int n = ++started;
        int prev = max_started.load();
        while (n > prev && !max_started.compare_exchange_weak(prev, n)) {}
    }
};

FakeDevice::~FakeDevice() {
    if (on_destroy)
        on_destroy();
    std::lock_guard<std::mutex> lk(backend_.m);
    auto it = backend_.live.find(id_);
    if (it != backend_.live.end() && it->second == this)
        backend_.live.erase(it);
}

bool FakeDevice::start(AudioCallback cb) {
    std::lock_guard<std::mutex> lk(m_);
    cb_ = std::move(cb);
    running_ = true;
    backend_.note_started();
    return true;
}

void FakeDevice::stop() {
    ++stop_calls;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (!running_)
            return;
        running_ = false;
    }
    std::lock_guard<std::mutex> wait(run_);  // waits for an in-flight block
    --backend_.started;
}

void FakeDevice::yank() {
    std::lock_guard<std::mutex> lk(m_);
    opened_ = false;
    if (running_)
        --backend_.started;  // the dead stream no longer delivers callbacks
    running_ = false;
}

bool FakeDevice::pump() {
    std::lock_guard<std::mutex> block(run_);
    AudioCallback cb;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (!running_)
            return false;
        cb = cb_;
    }
    in_callback = true;
    float data[64] = {};
    float* ch[1] = {data};
    BufferView<const float> in;
    BufferView<float> out(ch, 1, 64);
    CallbackContext ctx;
    cb(in, out, ctx);
    ++blocks;
    in_callback = false;
    return true;
}

DeviceConfig base_config() {
    DeviceConfig c;
    c.sample_rate = 48000;
    c.buffer_size = 64;
    return c;
}

} // namespace

TEST_CASE("follow-default moves to the new default; a named choice stays",
          "[audio][output_session]") {
    FakeBackend sys;
    sys.add("A");
    sys.add("B");
    sys.default_id = "A";

    OutputDeviceSession follow(sys, base_config(), [](auto&, auto&, auto&) {});
    REQUIRE(follow.open({true, ""}));
    REQUIRE(follow.open_device_id() == "A");

    OutputDeviceSession named(sys, base_config(), [](auto&, auto&, auto&) {});
    REQUIRE(named.open({false, "A"}));

    sys.default_id = "B";
    REQUIRE(follow.reconcile());
    REQUIRE(named.reconcile());
    REQUIRE(follow.open_device_id() == "B");
    REQUIRE(named.open_device_id() == "A");  // control: a named choice does not move
    REQUIRE(named.open_count() == 1);
    REQUIRE(follow.open_count() == 2);
}

TEST_CASE("reconcile with nothing changed does not reopen", "[audio][output_session]") {
    FakeBackend sys;
    sys.add("A");
    sys.default_id = "A";
    OutputDeviceSession s(sys, base_config(), [](auto&, auto&, auto&) {});
    REQUIRE(s.open({true, ""}));
    REQUIRE(s.reconcile());
    REQUIRE(s.reconcile());
    REQUIRE(s.open_count() == 1);
}

TEST_CASE("a named device that is unplugged is reported missing and reopens on return",
          "[audio][output_session]") {
    FakeBackend sys;
    sys.add("A");
    sys.add("B");
    sys.default_id = "B";
    OutputDeviceSession s(sys, base_config(), [](auto&, auto&, auto&) {});
    REQUIRE(s.open({false, "A"}));

    sys.remove("A");
    REQUIRE_FALSE(s.reconcile());
    REQUIRE(s.state() == OutputDeviceSession::State::missing);
    REQUIRE(sys.started == 0);  // the old callback is gone
    REQUIRE(s.choice().device_id == "A");  // the choice is kept

    sys.add("A");
    REQUIRE(s.reconcile());
    REQUIRE(s.open_device_id() == "A");
    REQUIRE(s.open_count() == 2);
    REQUIRE(sys.started == 1);
}

TEST_CASE("a replug the device list never showed is caught by the dead handle",
          "[audio][output_session]") {
    FakeBackend sys;
    sys.add("A");
    sys.default_id = "A";
    OutputDeviceSession s(sys, base_config(), [](auto&, auto&, auto&) {});
    REQUIRE(s.open({false, "A"}));
    FakeDevice* old_handle = sys.handle("A");
    REQUIRE(old_handle != nullptr);

    old_handle->yank();  // same id still listed, but the handle is dead
    REQUIRE(s.reconcile());
    REQUIRE(s.open_count() == 2);
    REQUIRE(sys.started == 1);
    REQUIRE(sys.handle("A") != nullptr);
    REQUIRE(sys.handle("A")->pump());  // the new handle is the live one
}

TEST_CASE("unplug mid-block: the old callback finishes, then exactly one new one runs",
          "[audio][output_session]") {
    FakeBackend sys;
    sys.add("A");
    sys.default_id = "A";

    std::mutex gate_m;
    std::condition_variable gate_cv;
    bool release = false;
    std::atomic<bool> hold_next{true};
    std::atomic<int> callbacks{0};
    auto cb = [&](const BufferView<const float>&, BufferView<float>&, const CallbackContext&) {
        ++callbacks;
        if (hold_next.exchange(false)) {
            std::unique_lock<std::mutex> lk(gate_m);
            gate_cv.wait(lk, [&] { return release; });
        }
    };

    OutputDeviceSession s(sys, base_config(), cb);
    REQUIRE(s.open({false, "A"}));
    FakeDevice* old_handle = sys.handle("A");

    std::thread audio([&] { old_handle->pump(); });
    while (!old_handle->in_callback)
        std::this_thread::yield();

    sys.remove("A");
    std::atomic<bool> reconciled{false};
    std::thread control([&] {
        s.reconcile();
        reconciled = true;
    });
    // The control thread is blocked in stop() while the block is in flight.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    REQUIRE_FALSE(reconciled.load());

    {
        std::lock_guard<std::mutex> lk(gate_m);
        release = true;
    }
    gate_cv.notify_all();
    audio.join();
    control.join();

    REQUIRE(s.state() == OutputDeviceSession::State::missing);
    REQUIRE(sys.started == 0);

    sys.add("A");
    REQUIRE(s.reconcile());
    REQUIRE(sys.started == 1);
    REQUIRE(sys.max_started.load() == 1);  // never two live callbacks
    FakeDevice* fresh = sys.handle("A");
    REQUIRE(fresh != nullptr);
    REQUIRE(fresh->pump());
    REQUIRE(callbacks.load() == 2);  // one block on the old handle, one on the new
}

TEST_CASE("close releases the device and forgets the choice", "[audio][output_session]") {
    FakeBackend sys;
    sys.add("A");
    sys.default_id = "A";
    OutputDeviceSession s(sys, base_config(), [](auto&, auto&, auto&) {});
    REQUIRE(s.open({true, ""}));
    s.close();
    REQUIRE(s.state() == OutputDeviceSession::State::closed);
    REQUIRE(sys.started == 0);
    REQUIRE_FALSE(s.reconcile());  // no choice: nothing to restore
    REQUIRE(sys.started == 0);
}

TEST_CASE("a device with no outputs is not a valid target", "[audio][output_session]") {
    FakeBackend sys;
    sys.add("IN", 0);
    sys.default_id = "IN";
    OutputDeviceSession s(sys, base_config(), [](auto&, auto&, auto&) {});
    REQUIRE_FALSE(s.open({true, ""}));
    REQUIRE(s.state() == OutputDeviceSession::State::missing);
}

// A real unplugged device may keep reporting open and running. The session must
// stop, join and close the old handle itself rather than wait to be told.
TEST_CASE("a dead device that never reports stopped is still stopped and closed by the session",
          "[audio][output_session]") {
    FakeBackend sys;
    sys.add("A");
    sys.default_id = "A";
    OutputDeviceSession s(sys, base_config(), [](auto&, auto&, auto&) {});
    REQUIRE(s.open({false, "A"}));
    FakeDevice* dead = sys.handle("A");
    REQUIRE(dead != nullptr);
    REQUIRE(dead->is_open());
    REQUIRE(dead->is_running());  // the dead handle still claims to be healthy
    std::atomic<int>& stops = dead->stop_calls;
    std::atomic<int>& closes = dead->close_calls;
    // The counters live in the handle; read them before the session destroys it.
    int stops_seen = -1, closes_seen = -1;
    dead->on_destroy = [&] {
        stops_seen = stops.load();
        closes_seen = closes.load();
    };

    sys.remove("A");  // unplugged: gone from the list, handle still "running"
    REQUIRE_FALSE(s.reconcile());
    REQUIRE(stops_seen == 1);   // the session stopped it itself
    REQUIRE(closes_seen == 1);  // and closed it
    REQUIRE(sys.started == 0);  // no callback left live

    sys.add("A");
    REQUIRE(s.reconcile());
    REQUIRE(sys.started == 1);
    REQUIRE(sys.max_started.load() == 1);
}
