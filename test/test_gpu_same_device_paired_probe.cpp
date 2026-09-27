#include "support/same_device_storage_probe.hpp"
#include "detail/shared_io_convolution_session.hpp"
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <ctime>

// Ordinary-thread, source-linked screening only. Uses the production callback
// bridge and dispatcher, while fallback data is a precomputed reference. This
// intentionally measures neither CPU shadow cost nor real audio-thread safety.
struct Row { std::uint64_t scheduled=0,start=0,end=0,deadline=0; unsigned sequence=0; int delivery=0; bool accepted=false; double error=0; };
struct Summary { double cpu=0,error=0; unsigned gpu=0,fallback=0,misses=0,rejected=0; };
using Session=SharedIoConvolutionSession;
constexpr unsigned warmup=32, measured=750, lead=2, capacity=8;
constexpr unsigned total=warmup+measured+lead;
std::vector<float> reference_audio() {
    std::vector<float> result(total*channels*frames);
    for(unsigned q=0;q<total;++q)for(unsigned ch=0;ch<channels;++ch)for(unsigned i=0;i<frames;++i){
        double value=0;
        for(unsigned t=0;t<taps;++t)value+=sample(long(q)*frames+i-t,ch)*std::exp(-double(t)/60.)*std::cos(double(t)*.31)/80.;
        result[(q*channels+ch)*frames+i]=float(value);
    }
    return result;
}
Summary trial(std::unique_ptr<Provider>& owner,Kind kind,unsigned ordinal,
              const std::vector<float>& reference,const std::filesystem::path& directory) {
    require(owner->reconfigure_storage_kind(kind),"paired storage reconfiguration refused");
    const auto generation=owner->device_owner_generation();
    const auto before=owner->stats();auto ir=ir_spectrum();
    auto program=owner->make_convolution_program({.fft_size=n,.channels=channels,.logical_frames=frames,.ir_length=taps,.normalized_ir_spectrum=ir});
    Session session;auto* provider=owner.get();
    Session::Config config;
    config.pipeline={.capacity=capacity,.channels=channels,.block_size=frames,.fft_size=n,.ir_length=taps,.lead_blocks=lead};
    config.slots=slots;config.sample_rate=48000;config.storage_kind=kind;
    config.active_path=kind==Kind::Staged?SharedIoPath::StagedAsync:SharedIoPath::SharedHostPointer;
    config.shared_host_pointer_capable=kind==Kind::ImportedHostPointer;
    config.cpu_fallback_prepared=false;
    require(session.prepare({std::move(owner),std::move(program)},config),"paired preparation failed");
    std::vector<Row> rows(total);std::vector<float> input(channels*frames),output(channels*frames);
    std::atomic<bool> stop{false},fenced{false};std::mutex mutex;std::condition_variable wake;
    std::thread worker([&]{
        while(!stop.load(std::memory_order_acquire)) {
            const auto result=session.service(now_ns());if(result.fenced)fenced.store(true,std::memory_order_release);
            std::unique_lock lock(mutex);wake.wait_for(lock,std::chrono::microseconds(50));
        }
    });
    // All allocations precede worker startup; join before any release/error.
    const auto start=Clock::now()+std::chrono::milliseconds(10);Summary summary;
    std::clock_t cpu_start=0;
    for(unsigned q=0;q<total;++q) {
        const auto scheduled=start+std::chrono::nanoseconds(std::uint64_t(q)*frames*1000000000ull/48000);
        std::this_thread::sleep_until(scheduled);
        if(q==warmup)cpu_start=std::clock();
        auto& row=rows[q];row.sequence=q;row.scheduled=std::chrono::duration_cast<std::chrono::nanoseconds>(scheduled.time_since_epoch()).count();
        for(unsigned ch=0;ch<channels;++ch)for(unsigned i=0;i<frames;++i)input[ch*frames+i]=sample(long(q)*frames+i,ch);
        row.deadline=std::chrono::duration_cast<std::chrono::nanoseconds>(start.time_since_epoch()).count()+std::uint64_t(q+1)*frames*1000000000ull/48000;
        row.start=now_ns();const auto callback=session.begin_callback(input);
        row.accepted=callback.admission==SharedIoStampedBridge::Admission::Accepted;
        const auto delivery=session.consume_output(callback,output);row.delivery=int(delivery);
        if(q>=lead && delivery!=Session::Delivery::Ready)
            std::copy_n(reference.data()+(q-lead)*channels*frames,channels*frames,output.data());
        row.end=now_ns();wake.notify_one();
        if(q>=lead && delivery==Session::Delivery::Ready) {
            for(unsigned i=0;i<channels*frames;++i) {
                const double error=std::abs(double(output[i])-reference[(q-lead)*channels*frames+i]);
                row.error=std::isfinite(error)?std::max(row.error,error):1e30;
            }
        }
        if(q>=warmup && q<warmup+measured) {
            summary.gpu+=delivery==Session::Delivery::Ready;
            summary.fallback+=delivery!=Session::Delivery::Ready;
            summary.rejected+=!row.accepted;summary.error=std::max(summary.error,row.error);
            summary.misses+=row.end>row.deadline;
        }
        if(q+1==warmup+measured)summary.cpu=double(std::clock()-cpu_start)/CLOCKS_PER_SEC;
    }
    stop.store(true,std::memory_order_release);wake.notify_one();worker.join();
    const auto stats=provider->stats();
    auto base=session.release_to_owner();owner.reset(static_cast<Provider*>(base.release()));
    require(bool(owner),"paired physical drain failed");require(owner->device_owner_generation()==generation,"paired device replaced");
    const auto after=owner->stats();
    const auto writes=stats.runtime_write_buffer_calls-before.runtime_write_buffer_calls;
    const auto copies=stats.runtime_copy_buffer_calls-before.runtime_copy_buffer_calls;
    const auto maps=stats.runtime_map_async_calls-before.runtime_map_async_calls;
    const bool transfer_ok=kind==Kind::Staged ? writes>0&&writes==copies&&writes==maps : writes==0&&copies==0&&maps==0;
    const auto name=std::to_string(ordinal)+(kind==Kind::Staged?"-staged":"-shared");
    std::ofstream csv(directory/(name+".csv"));csv<<"sequence,measured,scheduled_ns,start_ns,end_ns,deadline_ns,accepted,delivery,max_error\n";
    for(const auto& r:rows)csv<<r.sequence<<','<<(r.sequence>=warmup&&r.sequence<warmup+measured)<<','<<r.scheduled<<','<<r.start<<','<<r.end<<','<<r.deadline<<','<<r.accepted<<','<<r.delivery<<','<<r.error<<'\n';
    csv.close();require(bool(csv),"paired CSV write failed");
    std::cout<<"trial="<<ordinal<<" mode="<<(kind==Kind::Staged?"staged":"shared")<<" owner="<<generation
             <<" process_cpu_seconds="<<summary.cpu<<" gpu="<<summary.gpu<<" fallback_reference="<<summary.fallback
             <<" callback_deadline_misses="<<summary.misses<<" rejected="<<summary.rejected<<" max_error="<<summary.error
             <<" writes="<<writes<<" copies="<<copies<<" maps="<<maps<<" retired="<<after.retired_success-before.retired_success
             <<" failed="<<after.retired_failure-before.retired_failure<<'\n';
    require(!fenced.load()&&transfer_ok&&summary.gpu>0&&summary.error<1e-4,"paired correctness or transport gate failed");
    require(after.allocations==after.host_frees&&after.slots_created==after.slots_destroyed,"paired allocation imbalance");
    return summary;
}
int main(int argc,char** argv) {
    if(argc!=2)return 64;
    try {
        const std::filesystem::path directory=argv[1];require(std::filesystem::create_directory(directory),"fresh campaign directory required");
        auto owner=create();const auto generation=owner->device_owner_generation();const auto reference=reference_audio();
        std::cout<<"schema=same-device-storage-screening-v1 owner="<<generation<<" dawn="<<owner->dawn_revision()
                 <<" callback=ordinary_thread fallback=precomputed_reference fft=1024 fir=257 frames=128 rate=48000 channels=2 lead=2 slots=3 capacity=8 warmup=32 measured=750\n";
        for(unsigned q=0;q<6;++q) {const bool staged=(q%2)^((q/2)%2);trial(owner,staged?Kind::Staged:Kind::ImportedHostPointer,q,reference,directory);}
        std::cout<<"paired_screening=passed performance_verdict=unassigned\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
