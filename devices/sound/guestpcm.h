// GPL-3.0-or-later. Caller owns synchronization; samples are stereo frames.
#pragma once
#include <deque>
#include <cstdint>
#include <cmath>
#include <algorithm>
struct PcmConsumed { uint64_t source=0, silence=0; };
inline PcmConsumed consume_guest_pcm(std::deque<int16_t>& q,double& phase,double step,int16_t* dst,size_t frames,bool drain=false){
    PcmConsumed used;
    for(size_t i=0;i<frames;i++){
        if(q.size()<4){
            if(drain && q.size()==2){
                dst[i*2]=q[0];dst[i*2+1]=q[1];phase+=step;
                if(phase>=1){q.clear();phase=0;used.source++;}
                continue;
            }
            dst[i*2]=dst[i*2+1]=0;used.silence++;continue;
        }
        for(int ch=0;ch<2;ch++)dst[i*2+ch]=int16_t(std::lrint(q[ch]*(1-phase)+q[ch+2]*phase));
        phase+=step;
        while(phase>=1 && q.size()>=2){q.pop_front();q.pop_front();phase-=1;used.source++;}
    }
    return used;
}

// Completed DMA may leave a short sound below the normal startup watermark.
inline bool guest_pcm_ready(bool ready,size_t samples,uint32_t rate,bool active){
    return ready || samples>=size_t(rate)*2/20 || (!active && samples>=2);
}
