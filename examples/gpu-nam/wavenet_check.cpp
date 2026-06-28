// Correctness + value proof for the fused GPU conv-stack vs the CPU WaveNet
// reference: same synthetic model, same input block, from zero state. GPU must
// reproduce the CPU output, and must win at model sizes the CPU can't sustain.
#include "wavenet.hpp"
#include <pulp/render/gpu_compute.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

using clk = std::chrono::steady_clock;
static double us(clk::time_point t){ return std::chrono::duration<double,std::micro>(clk::now()-t).count(); }

static double xcorr(const std::vector<float>&a,const std::vector<float>&b){
    double sxy=0,sxx=0,syy=0; size_t n=std::min(a.size(),b.size());
    for(size_t i=0;i<n;i++){sxy+=a[i]*b[i];sxx+=a[i]*a[i];syy+=b[i]*b[i];}
    return sxy/std::sqrt(sxx*syy+1e-30);
}

int main(){
    using namespace pulp;
    auto gpu=render::GpuCompute::create();
    if(!gpu||!gpu->initialize_standalone()){std::printf("no gpu\n");return 0;}
    std::printf("device: %s %s\n",gpu->capabilities().vendor.c_str(),gpu->capabilities().backend.c_str());

    const double SR=48000.0;
    struct Cfg{const char*name;uint32_t C,K,L;};
    Cfg cfgs[]={
        {"standard C16 K3 L16",16,3,16},
        {"big      C32 K3 L24",32,3,24},
        {"huge     C64 K3 L24",64,3,24},
    };
    const uint32_t B=512; const double budget=B/SR*1e6;
    std::printf("block=%u  real-time budget=%.0f us/block\n\n",B,budget);
    for(auto&cf:cfgs){
        examples::WaveNetConfig cfg; cfg.channels=cf.C; cfg.kernel=cf.K;
        uint32_t d=1; for(uint32_t l=0;l<cf.L;l++){cfg.dilations.push_back(d); d = (d>=512)?1:d*2;}
        auto w=examples::make_synthetic_wavenet(cfg);
        examples::WaveNetCpu net; net.load(cfg,w);

        std::vector<float> in(B),oc(B),og(B);
        for(uint32_t i=0;i<B;i++) in[i]=0.2f*std::sin(0.03f*i)+0.05f*std::sin(0.21f*i);

        net.reset(); net.process(in.data(),oc.data(),B);
        if(!gpu->prepare_conv_stack(cf.C,cf.K,cfg.dilations.data(),cf.L,w.data(),
                                    (uint32_t)w.size(),B,cfg.head_scale)){
            std::printf("%-22s prepare FAILED\n",cf.name); continue; }
        if(!gpu->conv_stack_forward(in.data(),og.data(),B)){
            std::printf("%-22s forward FAILED\n",cf.name); continue; }
        double corr=xcorr(oc,og);

        // timing
        for(int i=0;i<3;i++){net.reset();net.process(in.data(),oc.data(),B);gpu->conv_stack_forward(in.data(),og.data(),B);}
        int it=50;
        auto t0=clk::now(); for(int i=0;i<it;i++){net.reset();net.process(in.data(),oc.data(),B);} double cu=us(t0)/it;
        t0=clk::now(); for(int i=0;i<it;i++)gpu->conv_stack_forward(in.data(),og.data(),B); double gu=us(t0)/it;
        // Streaming continuity: process two consecutive blocks. CPU carries its
        // history (no reset between); GPU carries the slid window. Block 2 must
        // match — proving cross-block context, not a per-block restart.
        std::vector<float> in2(B), c1(B), c2(B), g1(B), g2(B);
        for (uint32_t i = 0; i < B; ++i) in2[i] = 0.2f*std::sin(0.03f*(i+B)) + 0.05f*std::sin(0.21f*(i+B));
        net.reset();
        net.process(in.data(), c1.data(), B);
        net.process(in2.data(), c2.data(), B);    // continues, no reset
        gpu->prepare_conv_stack(cf.C,cf.K,cfg.dilations.data(),cf.L,w.data(),(uint32_t)w.size(),B,cfg.head_scale);
        gpu->conv_stack_forward(in.data(), g1.data(), B);
        gpu->conv_stack_forward(in2.data(), g2.data(), B);   // carries history
        double scorr = xcorr(c2, g2);

        std::printf("%-22s  xcorr=%.5f %s  stream=%.5f %s | CPU %8.1f us %s | GPU %8.1f us %s | %.1fx\n",
            cf.name,corr, corr>0.99?"[match]":"[MISMATCH]",
            scorr, scorr>0.99?"[ok]":"[BROKEN]",
            cu, cu<budget?"<bud":"OVER", gu, gu<budget?"<bud":"OVER", cu/gu);
    }
    return 0;
}
