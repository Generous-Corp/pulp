#include <catch2/catch_test_macros.hpp>
#include "../core/audio/platform/mac/coreaudio_device.hpp"
#include "harness/rt_allocation_probe.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <source_location>
#include <memory>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>

using namespace pulp::audio;
using namespace pulp::audio::mac;

namespace {
struct Native {
    OSStatus stop_status = noErr, uninitialize_status = noErr, dispose_status = noErr;
    OSStatus remove_status = noErr, start_status = noErr, registration_status = noErr;
    unsigned stops = 0, uninitializes = 0, disposes = 0, removals = 0, starts = 0, adds = 0;
    std::function<void()> on_add;
    AURenderCallbackStruct render{};
    CoreAudioNativeOperations operations() {
        return {this,
            [](void* p, AudioUnit) { auto& n=*static_cast<Native*>(p); ++n.starts; return n.start_status; },
            [](void* p, AudioUnit) { auto& n=*static_cast<Native*>(p); ++n.stops; return n.stop_status; },
            [](void* p, AudioUnit) { auto& n=*static_cast<Native*>(p); ++n.uninitializes; return n.uninitialize_status; },
            [](void* p, AudioUnit) { auto& n=*static_cast<Native*>(p); ++n.disposes; return n.dispose_status; },
            [](void* p, AudioUnit, bool, const AURenderCallbackStruct& cb) {
                auto& n=*static_cast<Native*>(p); n.render=cb; return n.registration_status;
            },
            [](void* p, AudioObjectID, const AudioObjectPropertyAddress&, AudioObjectPropertyListenerProc, void*) {
                auto& n=*static_cast<Native*>(p); ++n.removals; return n.remove_status;
            },
            [](void* p, AudioObjectID, const AudioObjectPropertyAddress&, AudioObjectPropertyListenerProc, void*) -> OSStatus {
                auto& n=*static_cast<Native*>(p);++n.adds;
                if(n.on_add)n.on_add();
                return noErr;
            }};
    }
};
OSStatus render(const AURenderCallbackStruct& cb, float* value) {
    AudioUnitRenderActionFlags flags=0;
    AudioTimeStamp timestamp{};
    AudioBufferList output{};
    output.mNumberBuffers=1;
    output.mBuffers[0]={1,sizeof(float),value};
    return cb.inputProc(cb.inputProcRefCon,&flags,&timestamp,0,1,value?&output:nullptr);
}
void await(const std::atomic<bool>& flag,
           std::source_location caller = std::source_location::current()) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!flag.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    if (!flag.load(std::memory_order_acquire)) {
        std::fprintf(stderr, "CoreAudio test progress timed out at %s:%u\n",
                     caller.file_name(), caller.line());
        // A distinct exit code cannot satisfy the child tests expecting termination code 77.
        // Avoid unwinding through live callbacks or invoking Catch on a worker.
        std::_Exit(124);
    }
}
}

namespace pulp::audio::mac {
struct CoreAudioLifecycleTestAccess {
    static std::unique_ptr<CoreAudioDevice> create(Native& native, AudioCallback callback={}) {
        auto device=std::make_unique<CoreAudioDevice>(41);
        device->native_ops_=native.operations();
        device->audio_unit_=reinterpret_cast<AudioUnit>(std::uintptr_t{1});
        device->is_open_=true;
        device->is_running_=true;
        device->unit_initialized_=true;
        device->config_.sample_rate=48000;
        device->config_.buffer_size=1;
        device->config_.output_channels=1;
        device->output_ptrs_.resize(1);
        device->listener_context_=std::make_unique<CoreAudioCallbackContext>(device.get());
        device->default_output_listener_installed_=true;
        device->overload_listener_installed_=true;
        device->overload_listener_device_id_=41;
        if (!device->install_render_context_locked()) return {};
        device->fallback_priority_configured_.store(true);
        device->callback_=std::move(callback);
        return device;
    }
    static CoreAudioCallbackContext* events(CoreAudioDevice& device) { return device.listener_context_.get(); }
    static void default_event(void* context) {
        CoreAudioDevice::default_output_changed_listener(0,0,nullptr,context);
    }
    static void overload(void* context) { CoreAudioDevice::overload_listener(0,0,nullptr,context); }
    static std::unique_lock<std::mutex> hold_switch(CoreAudioDevice& device) {
        return std::unique_lock(device.switch_mutex_);
    }
    static std::unique_ptr<CoreAudioSystem> system(Native& native, unsigned& calls) {
        auto system=std::make_unique<CoreAudioSystem>();
        system->native_ops_=native.operations();
        system->listener_installed_=true;
        system->default_output_listener_installed_=true;
        system->default_input_listener_installed_=true;
        system->set_device_change_callback([&calls]{++calls;});
        system->set_default_device_change_callback([&calls](bool){++calls;});
        return system;
    }
    static CoreAudioCallbackContext* events(CoreAudioSystem& system) { return system.listener_context_.get(); }
    static void system_events(void* context) {
        CoreAudioSystem::device_list_changed(0,0,nullptr,context);
        CoreAudioSystem::default_device_changed(0,0,nullptr,context);
    }
    static void unregistered(CoreAudioSystem& system) { system.listener_installed_=false; }
    static auto current_slot(CoreAudioSystem& system) { return system.device_change_cb_; }
    static void snapshot_hook(CoreAudioSystem& system, void (*hook)(void*), void* context) {
        system.notification_snapshot_hook_=hook; system.notification_snapshot_hook_context_=context;
    }
    static void drain_hook(CoreAudioSystem& system, void (*hook)(void*), void* context) {
        system.notification_drain_hook_=hook;system.notification_drain_hook_context_=context;
    }
    static void timing_listener(CoreAudioDevice& device) {
        device.timing_listener_context_=std::make_unique<CoreAudioTimingListenerContext>(&device,
            [](void*) noexcept {});
        device.timing_listener_installed_[0]=true;
        device.timing_property_addresses_[0]={kAudioDevicePropertyLatency,
            kAudioObjectPropertyScopeOutput,kAudioObjectPropertyElementMain};
    }
    static void no_priority(CoreAudioDevice& device) { device.fallback_priority_configured_.store(true); }
};
}

TEST_CASE("CoreAudio failed native stop drains admitted user work before releasing callable",
          "[audio][coreaudio][lifetime]") {
    Native native; native.stop_status=kAudio_ParamError;
    std::atomic<bool> entered{false}, resume{false}, stopped{false};
    auto capture=std::make_shared<int>(1); std::weak_ptr<int> weak=capture;
    auto device=CoreAudioLifecycleTestAccess::create(native,
        [capture,&entered,&resume](const auto&,auto&,const auto&) {
            entered.store(true,std::memory_order_release); await(resume);
        });
    capture.reset(); REQUIRE(device);
    const auto callback=native.render;
    auto* context=static_cast<CoreAudioCallbackContext*>(callback.inputProcRefCon);
    float output=1;
    std::thread reader([&]{render(callback,&output);}); await(entered);
    CoreAudioTeardownResult result;
    std::thread stopper([&]{result=device->stop_checked();stopped.store(true,std::memory_order_release);});
    while(!context->closed())std::this_thread::yield();
    CHECK_FALSE(stopped.load()); CHECK_FALSE(weak.expired());
    resume.store(true,std::memory_order_release); reader.join();stopper.join();
    CHECK(result.stage==CoreAudioTeardownResult::Stage::Stop);
    CHECK(weak.expired()); CHECK(device->is_running()); // native ownership uncertain
    output=1; CHECK(render(callback,&output)==noErr); CHECK(output==0);
    CHECK(render(callback,nullptr)==noErr);
    native.stop_status=noErr;
    CHECK(device->stop_checked().complete()); CHECK_FALSE(device->is_running());
    const auto stops=native.stops; CHECK(device->stop_checked().complete()); CHECK(native.stops==stops);
    CHECK(device->close_checked().complete());
}

TEST_CASE("CoreAudio teardown errors retain native ownership behind permanently closed refcons",
          "[audio][coreaudio][lifetime]") {
    Native native;
    auto expected=CoreAudioTeardownResult::Stage::Stop;
    SECTION("stop") {native.stop_status=kAudio_ParamError;}
    SECTION("uninitialize") {native.uninitialize_status=kAudio_ParamError;expected=CoreAudioTeardownResult::Stage::Uninitialize;}
    SECTION("dispose") {native.dispose_status=kAudio_ParamError;expected=CoreAudioTeardownResult::Stage::Dispose;}
    SECTION("listener detach") {native.remove_status=kAudio_ParamError;expected=CoreAudioTeardownResult::Stage::Listener;}
    unsigned user_calls=0;
    auto device=CoreAudioLifecycleTestAccess::create(native,[&](const auto&,auto&,const auto&){++user_calls;});
    REQUIRE(device);
    const auto callback=native.render;
    auto* events=CoreAudioLifecycleTestAccess::events(*device);
    CHECK(device->close_checked().stage==expected);
    CHECK_FALSE(device->open({}));
    device.reset(); // actual production destructor transfers uncertain ownership
    float sample=1;
    CHECK(render(callback,&sample)==noErr); CHECK(sample==0);
    CHECK(render(callback,nullptr)==noErr);
    CoreAudioLifecycleTestAccess::default_event(events);
    CoreAudioLifecycleTestAccess::overload(events);
    CHECK(user_calls==0);
    CHECK(retry_coreaudio_native_retirements()==1);
    native.stop_status=native.uninitialize_status=native.dispose_status=native.remove_status=noErr;
    CHECK(retry_coreaudio_native_retirements()==0);
    const auto disposed=native.disposes;
    CHECK(retry_coreaudio_native_retirements()==0); CHECK(native.disposes==disposed);
    sample=1;CHECK(render(callback,&sample)==noErr);CHECK(sample==0);
}

TEST_CASE("CoreAudio restart installs a fresh refcon and never reopens retired callbacks",
          "[audio][coreaudio][lifetime]") {
    Native native;unsigned calls=0;
    auto cb=[&](const auto&,auto&,const auto&){++calls;};
    auto device=CoreAudioLifecycleTestAccess::create(native,cb);REQUIRE(device);
    const auto old=native.render;
    CHECK_FALSE(device->start(cb)); CHECK(native.starts==0);
    REQUIRE(device->stop_checked().complete());REQUIRE(device->start(cb));
    CoreAudioLifecycleTestAccess::no_priority(*device);
    CHECK(native.render.inputProcRefCon!=old.inputProcRefCon);
    float output=1;
    {
        pulp::test::RtAllocationProbe allocations;
        render(old,&output);render(native.render,&output);
        CHECK_FALSE(allocations.saw_allocation());
    }
    CHECK(calls==1);CHECK(device->close_checked().complete());
    CHECK(native.uninitializes==1);CHECK(native.disposes==1);
}

TEST_CASE("CoreAudio close drains default listener before taking its switch mutex",
          "[audio][coreaudio][lifetime]") {
    Native native;auto device=CoreAudioLifecycleTestAccess::create(native);REQUIRE(device);
    auto* events=CoreAudioLifecycleTestAccess::events(*device);
    auto lock=CoreAudioLifecycleTestAccess::hold_switch(*device);
    std::thread listener([&]{CoreAudioLifecycleTestAccess::default_event(events);});
    while(events->admitted()==0)std::this_thread::yield();
    std::atomic<bool> closed{false};
    std::thread closer([&]{device->close();closed.store(true,std::memory_order_release);});
    while(!events->closed())std::this_thread::yield();
    CHECK_FALSE(closed.load());
    lock.unlock();listener.join();closer.join();CHECK(closed.load());
}

TEST_CASE("CoreAudio failed start closes newly registered callback before returning",
          "[audio][coreaudio][lifetime]") {
    Native native;auto device=CoreAudioLifecycleTestAccess::create(native);REQUIRE(device);
    REQUIRE(device->stop_checked().complete());native.start_status=kAudio_ParamError;
    unsigned calls=0;
    CHECK_FALSE(device->start([&](const auto&,auto&,const auto&){++calls;}));
    float output=1;render(native.render,&output);CHECK(output==0);CHECK(calls==0);
    CHECK(device->stop_checked().complete());CHECK(device->close_checked().complete());
}

TEST_CASE("CoreAudio system listener removal failure cannot reach a destroyed system",
          "[audio][coreaudio][lifetime]") {
    Native native; native.remove_status=kAudio_ParamError; unsigned calls=0;
    auto system=CoreAudioLifecycleTestAccess::system(native,calls);
    auto* events=CoreAudioLifecycleTestAccess::events(*system);
    CoreAudioLifecycleTestAccess::system_events(events);CHECK(calls==2);
    system.reset();
    CoreAudioLifecycleTestAccess::system_events(events);CHECK(calls==2);
    CHECK(native.removals==3);CHECK(retry_coreaudio_native_retirements()==1);
    native.remove_status=noErr;CHECK(retry_coreaudio_native_retirements()==0);
    CoreAudioLifecycleTestAccess::system_events(events);CHECK(calls==2);
    CHECK(native.disposes==0);
}

TEST_CASE("CoreAudio public close and quiesce serialize their shared listener context",
          "[audio][coreaudio][lifetime]") {
    Native native;auto device=CoreAudioLifecycleTestAccess::create(native);REQUIRE(device);
    std::thread first([&]{device->close();});
    std::thread second([&]{device->quiesce_workgroup_changes();device->close();});
    first.join();second.join();
    CHECK(native.disposes==1);CHECK(native.uninitializes==1);CHECK(native.stops==1);
}

TEST_CASE("CoreAudio system callbacks retain their callable during self replacement",
          "[audio][coreaudio][lifetime]") {
    Native native;unsigned calls=0;
    auto system=CoreAudioLifecycleTestAccess::system(native,calls);
    auto* events=CoreAudioLifecycleTestAccess::events(*system);
    system->set_device_change_callback([&]{++calls;system->set_device_change_callback(nullptr);});
    system->set_default_device_change_callback([&](bool){++calls;system->set_default_device_change_callback(nullptr);});
    CoreAudioLifecycleTestAccess::system_events(events);CHECK(calls==2);
    CoreAudioLifecycleTestAccess::system_events(events);CHECK(calls==2);
}

TEST_CASE("CoreAudio system replacement preserves an already dispatched callable",
          "[audio][coreaudio][lifetime]") {
    Native native;unsigned calls=0;
    auto system=CoreAudioLifecycleTestAccess::system(native,calls);
    auto* events=CoreAudioLifecycleTestAccess::events(*system);
    std::atomic<bool> entered{false},resume{false};
    auto capture=std::make_shared<int>(1);std::weak_ptr<int> weak=capture;
    system->set_device_change_callback([capture,&entered,&resume]{
        entered.store(true,std::memory_order_release);await(resume);
    });
    capture.reset();
    std::thread reader([&]{CoreAudioLifecycleTestAccess::system_events(events);});await(entered);
    auto slot=CoreAudioLifecycleTestAccess::current_slot(*system);
    std::atomic<bool> cleared{false};
    std::thread clearer([&]{system->set_device_change_callback(nullptr);cleared.store(true);});
    while(!slot->admission.closed())std::this_thread::yield();
    CHECK_FALSE(cleared.load());CHECK_FALSE(weak.expired());
    resume.store(true,std::memory_order_release);reader.join();clearer.join();
    slot.reset();CHECK(cleared.load());CHECK(weak.expired());
}

TEST_CASE("CoreAudio same object destruction from its notification fails fast instead of self draining",
          "[audio][coreaudio][lifetime]") {
    const auto child=fork();REQUIRE(child>=0);
    if(child==0) {
        std::set_terminate([]{_Exit(77);});
        Native native;unsigned calls=0;
        auto system=CoreAudioLifecycleTestAccess::system(native,calls);
        auto* events=CoreAudioLifecycleTestAccess::events(*system);
        system->set_device_change_callback([&]{system.reset();});
        CoreAudioLifecycleTestAccess::system_events(events);
        _Exit(1);
    }
    int status=0;REQUIRE(waitpid(child,&status,0)==child);
    REQUIRE(WIFEXITED(status));CHECK(WEXITSTATUS(status)==77);
}

TEST_CASE("CoreAudio external clear drains a generation already retired by self clear",
          "[audio][coreaudio][lifetime]") {
    Native native;unsigned calls=0;
    auto system=CoreAudioLifecycleTestAccess::system(native,calls);
    auto* events=CoreAudioLifecycleTestAccess::events(*system);
    std::atomic<bool> retired{false},resume{false},cleared{false};
    system->set_device_change_callback([&]{
        system->set_device_change_callback(nullptr);
        retired.store(true,std::memory_order_release);await(resume);
    });
    std::thread reader([&]{CoreAudioLifecycleTestAccess::system_events(events);});await(retired);
    std::atomic<bool> draining{false};
    CoreAudioLifecycleTestAccess::drain_hook(*system,[](void* p){
        static_cast<std::atomic<bool>*>(p)->store(true,std::memory_order_release);
    },&draining);
    std::thread clearer([&]{system->set_device_change_callback(nullptr);cleared.store(true);});
    await(draining);
    // The old delivery is still admitted even though the current slot is empty.
    CHECK_FALSE(cleared.load());
    resume.store(true,std::memory_order_release);reader.join();clearer.join();CHECK(cleared.load());
}

TEST_CASE("CoreAudio system destruction waits across the snapshot before user invocation window",
          "[audio][coreaudio][lifetime]") {
    struct Pause {std::atomic<bool> entered{false},resume{false};} pause;
    Native native;unsigned calls=0;
    auto system=CoreAudioLifecycleTestAccess::system(native,calls);
    auto* events=CoreAudioLifecycleTestAccess::events(*system);
    CoreAudioLifecycleTestAccess::snapshot_hook(*system,[](void* p){
        auto& state=*static_cast<Pause*>(p);state.entered.store(true,std::memory_order_release);await(state.resume);
    },&pause);
    std::thread reader([&]{CoreAudioLifecycleTestAccess::system_events(events);});await(pause.entered);
    std::atomic<bool> destroyed{false};
    std::thread destroyer([&]{system.reset();destroyed.store(true);});
    while(!events->closed())std::this_thread::yield();
    CHECK_FALSE(destroyed.load());
    pause.resume.store(true,std::memory_order_release);reader.join();destroyer.join();
    CHECK(destroyed.load());CHECK(calls==1);
}

TEST_CASE("CoreAudio checked close includes timing listener failure and retry",
          "[audio][coreaudio][lifetime]") {
    Native native;auto device=CoreAudioLifecycleTestAccess::create(native);REQUIRE(device);
    CoreAudioLifecycleTestAccess::timing_listener(*device);
    native.remove_status=kAudio_ParamError;
    CHECK(device->close_checked().stage==CoreAudioTeardownResult::Stage::Listener);
    device.reset();CHECK(retry_coreaudio_native_retirements()==1);
    native.remove_status=noErr;CHECK(retry_coreaudio_native_retirements()==0);
}

TEST_CASE("CoreAudio render reentrant stop fails before blocking on another stop thread",
          "[audio][coreaudio][lifetime]") {
    const auto child=fork();REQUIRE(child>=0);
    if(child==0) {
        alarm(3);std::set_terminate([]{_Exit(77);});
        Native native;std::atomic<bool> entered{false},resume{false};
        CoreAudioDevice* raw=nullptr;
        auto device=CoreAudioLifecycleTestAccess::create(native,[&](const auto&,auto&,const auto&){
            entered.store(true,std::memory_order_release);await(resume);raw->stop();
        });raw=device.get();
        const auto callback=native.render;
        auto* context=static_cast<CoreAudioCallbackContext*>(callback.inputProcRefCon);
        std::thread reader([&]{float output=0;render(callback,&output);});await(entered);
        std::thread stopper([&]{device->stop();});
        while(!context->closed())std::this_thread::yield();
        resume.store(true,std::memory_order_release);
        reader.join();stopper.join();_Exit(1);
    }
    int status=0;REQUIRE(waitpid(child,&status,0)==child);
    REQUIRE(WIFEXITED(status));CHECK(WEXITSTATUS(status)==77);
}

TEST_CASE("CoreAudio system reentrant replacement does not duplicate pending registration",
          "[audio][coreaudio][lifetime]") {
    Native native;unsigned calls=0;
    auto system=CoreAudioLifecycleTestAccess::system(native,calls);
    auto* events=CoreAudioLifecycleTestAccess::events(*system);
    CoreAudioLifecycleTestAccess::unregistered(*system);
    native.on_add=[&]{CoreAudioLifecycleTestAccess::system_events(events);};
    system->set_device_change_callback([&]{
        ++calls;system->set_device_change_callback([&]{calls+=10;});
    });
    CHECK(native.adds==1);CHECK(calls==2);
    native.on_add=nullptr;
    CoreAudioLifecycleTestAccess::system_events(events);CHECK(calls==13);
    system->fire_device_change();CHECK(calls==23);
}
