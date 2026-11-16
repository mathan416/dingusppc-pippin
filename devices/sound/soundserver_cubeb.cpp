/*
DingusPPC - The Experimental PowerPC Macintosh emulator
Copyright (C) 2018-26 The DingusPPC Development Team
          (See CREDITS.MD for more details)

(You may also contact divingkxt or powermax2286 on Discord)

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#ifndef NOMINMAX
#define NOMINMAX
#endif // NOMINMAX

#include <core/endianswap.h>
#include <cpu/ppc/ppcemu.h>
#include <core/timermanager.h>
#include <devices/common/dmacore.h>
#include <devices/sound/soundserver.h>
#include <cubeb/cubeb.h>
#include <loguru.hpp>
#include <algorithm>
#include <atomic>
#include <deque>
#include <chrono>
#include <devices/sound/guestpcm.h>
#include <devices/sound/audiobudget.h>
#include <devices/sound/guestpace.h>
#include <devices/sound/guestpcm_assembler.h>
#include <mutex>
#include <fstream>
#include <cstdlib>
#include <string>

// Only the emulation thread may advance guest DMA or raise its interrupts.
// Cubeb's host thread consumes already-converted PCM, never a guest DMA object.
struct PippinAudioQueue {
    std::mutex mutex;
    std::deque<int16_t> samples;
    std::atomic<bool> running{false};
    uint64_t generated=0, played=0, underrun=0, dropped=0, active_silence=0, idle_silence=0;
    bool guest_active=false;
    uint32_t rate=0;
    bool guest_sync=false, ready=false;
    double pace=1.0, phase=0;
};
class SoundServer::Impl {
public:
    cubeb* ctx=nullptr;
    cubeb_stream* stream=nullptr;
    PippinAudioQueue queue;
    DmaOutChannel* dma=nullptr;
    TimerInfo pump;
    timer_cb pump_cb;
    uint32_t sequence=0;
    GuestAudioBudget budget;
    GuestPcmAssembler assembler;
    GuestPace estimator;
    uint64_t last_guest=0;
    std::chrono::steady_clock::time_point last_wall;
    std::ofstream trace;
};

static long queued_audio(cubeb_stream*,void* data,void const*,void* output,long frames) {
    auto* q=static_cast<PippinAudioQueue*>(data);
    auto* dst=static_cast<int16_t*>(output);
    std::lock_guard<std::mutex> lock(q->mutex);
    if(q->guest_sync){
        q->ready=guest_pcm_ready(q->ready,q->samples.size(),q->rate,q->guest_active);
        if(!q->ready){std::fill(dst,dst+size_t(frames)*2,int16_t(0));q->underrun+=frames;
            if(q->guest_active)q->active_silence+=frames;else q->idle_silence+=frames;
            return frames;}
        double fill=double(q->samples.size()/2)/(double(q->rate)*.075);
        double step=std::clamp(q->pace*std::clamp(fill,.8,1.2),.025,1.0);
        auto used=consume_guest_pcm(q->samples,q->phase,step,dst,frames,!q->guest_active);
        q->played+=used.source;q->underrun+=used.silence;
        if(q->guest_active)q->active_silence+=used.silence;else q->idle_silence+=used.silence;
        if(used.silence)q->ready=false;
        return frames;
    }
    size_t count=std::min(q->samples.size(),size_t(frames)*2);
    for(size_t i=0;i<count;i++){dst[i]=q->samples.front();q->samples.pop_front();}
    std::fill(dst+count,dst+size_t(frames)*2,int16_t(0));
    q->played+=count/2;q->underrun+=frames-count/2;
    if(q->guest_active)q->active_silence+=frames-count/2;else q->idle_silence+=frames-count/2;
    // Empty/paused guest DMA is not end-of-stream. Keep the host stream alive
    // until explicit close, otherwise Cubeb drains and ignores later samples.
    return frames;
}
static void queue_state(cubeb_stream*,void* data,cubeb_state state) {
    static_cast<PippinAudioQueue*>(data)->running.store(state==CUBEB_STATE_STARTED);
}
SoundServer::SoundServer():impl(std::make_unique<Impl>()) {
    supports_types(HWCompType::SND_SERVER);start();
}
SoundServer::~SoundServer(){shutdown();}
int SoundServer::start(){
    if(impl->ctx)return 0;
    int r=cubeb_init(&impl->ctx,"Dingus sound server",nullptr);
    if(r!=CUBEB_OK){LOG_F(ERROR,"Could not initialize Cubeb: %d",r);return r;}
    LOG_F(INFO,"Connected to backend: %s",cubeb_get_backend_id(impl->ctx));return 0;
}
void SoundServer::shutdown(){
    close_out_stream();
    if(impl->ctx){cubeb_destroy(impl->ctx);impl->ctx=nullptr;}
    LOG_F(INFO,"Sound Server shut down.");
}
int SoundServer::open_out_stream(uint32_t rate,DmaOutChannel* dma){
    close_out_stream();
    impl->dma=dma;impl->budget.remainder=0;impl->assembler.reset();impl->estimator=GuestPace{};
    impl->last_guest=get_virt_time_ns();impl->last_wall=std::chrono::steady_clock::now();
    {std::lock_guard<std::mutex> lock(impl->queue.mutex);
     impl->queue.samples.clear();impl->queue.rate=rate;
     impl->queue.guest_sync=std::getenv("DINGUS_AUDIO_GUEST_SYNC")!=nullptr;
     impl->queue.ready=false;impl->queue.phase=0;impl->queue.pace=1;
     impl->queue.generated=impl->queue.played=impl->queue.underrun=impl->queue.dropped=0;
     impl->queue.active_silence=impl->queue.idle_silence=0;impl->queue.guest_active=dma->is_out_active();}
    if(const char* prefix=std::getenv("DINGUS_AUDIO_TRACE")) {
        std::string name=std::string(prefix)+"-"+std::to_string(rate)+"-"+std::to_string(impl->sequence++)+".pcm";
        impl->trace.open(name,std::ios::binary|std::ios::trunc);
    }
    impl->pump_cb=[this,rate](uint64_t,uint64_t){
        auto now=std::chrono::steady_clock::now();uint64_t guest=get_virt_time_ns();
        uint64_t elapsed=guest>impl->last_guest?guest-impl->last_guest:0;
        uint64_t host_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(now-impl->last_wall).count();
        impl->estimator.update(elapsed,host_ns);
        {
            std::lock_guard<std::mutex> lock(impl->queue.mutex);
            impl->queue.pace=impl->estimator.rate;
            impl->queue.guest_active=impl->dma && impl->dma->is_out_active();
        }
        impl->last_guest=guest;impl->last_wall=now;
        if(!impl->dma || !impl->dma->is_out_active())return;
        // Cyclic timers intentionally skip missed deadlines. Consume the actual
        // elapsed guest duration, not one nominal callback period.
        uint32_t remain=impl->budget.frames(rate,std::min(elapsed,uint64_t(1000000000)))*4;
        while(remain){
            uint8_t* bytes=nullptr;uint32_t got=0;
            if(impl->dma->pull_data(remain,&got,&bytes)!=DmaPullResult::MoreData || !got)break;
            if(got>remain){LOG_F(ERROR,"Audio DMA exceeded requested length");break;}
            remain-=got;
            if(!bytes)continue;
            std::lock_guard<std::mutex> lock(impl->queue.mutex);
            impl->assembler.append(bytes,got,[&](int16_t left,int16_t right){
                int16_t pair[2]={left,right};
                if(impl->trace)impl->trace.write(reinterpret_cast<char*>(pair),sizeof(pair));
                if(!is_deterministic){impl->queue.samples.push_back(pair[0]);impl->queue.samples.push_back(pair[1]);}
                impl->queue.generated++;
            });
            size_t limit=size_t(rate)*4;
            while(impl->queue.samples.size()>limit){impl->queue.samples.pop_front();impl->queue.samples.pop_front();impl->queue.dropped++;}
        }
    };
    if(is_deterministic)return 0;
    if(!impl->ctx)return CUBEB_ERROR;
    cubeb_stream_params params={.format=CUBEB_SAMPLE_S16NE,.rate=rate,.channels=2,.layout=CUBEB_LAYOUT_STEREO,.prefs=CUBEB_STREAM_PREF_NONE};
    uint32_t latency=0;int r=cubeb_get_min_latency(impl->ctx,&params,&latency);
    if(r!=CUBEB_OK)return r;
    r=cubeb_stream_init(impl->ctx,&impl->stream,"SndOut stream",nullptr,nullptr,nullptr,&params,latency,queued_audio,queue_state,&impl->queue);
    LOG_F(INFO,"Guest-clock audio stream: %u Hz stereo, host open=%d",rate,r);
    return r;
}
int SoundServer::start_out_stream(){
    if(!impl->pump.active && impl->dma)
        TimerManager::get_instance()->add_cyclic_timer(impl->pump,MSECS_TO_NSECS(10),impl->pump_cb);
    if(is_deterministic)return 0;
    if(!impl->stream)return CUBEB_ERROR;
    if(impl->queue.running.load())return 0;
    cubeb_stream_stop(impl->stream);
    int r=cubeb_stream_start(impl->stream);
    if(r==CUBEB_OK)impl->queue.running.store(true);
    return r;
}
void SoundServer::close_out_stream(){
    if(impl->pump.active)TimerManager::get_instance()->cancel_timer(impl->pump);
    if(impl->stream){cubeb_stream_stop(impl->stream);cubeb_stream_destroy(impl->stream);impl->stream=nullptr;}
    impl->queue.running.store(false);
    if(impl->dma){
        std::lock_guard<std::mutex> lock(impl->queue.mutex);
        LOG_F(INFO,"Audio frames generated=%llu played=%llu silence=%llu dropped=%llu active_silence=%llu idle_silence=%llu",(unsigned long long)impl->queue.generated,(unsigned long long)impl->queue.played,(unsigned long long)impl->queue.underrun,(unsigned long long)impl->queue.dropped,(unsigned long long)impl->queue.active_silence,(unsigned long long)impl->queue.idle_silence);
        impl->queue.samples.clear();
    }
    impl->trace.close();impl->trace.clear();impl->dma=nullptr;
}
