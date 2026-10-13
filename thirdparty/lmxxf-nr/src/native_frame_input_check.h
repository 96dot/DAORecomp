#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
enum class NativeFrameInputCheck { valid, wrong_size, nonfinite, black };
inline bool NativeFrameRequestValid(uint32_t current_pid,uint32_t request_pid,uint32_t request,uint32_t last){
 return current_pid&&current_pid==request_pid&&request>last&&request<=1000000;
}
inline NativeFrameInputCheck CheckNativeFrameInput(const std::vector<unsigned char>&bytes,unsigned width=1920,unsigned height=1080,unsigned bpp=0){
 /* 8-bit UNORM frames (Magpie, 4 bytes per pixel): no NaN possible; black = every RGB byte zero */
 if(!width||!height)return NativeFrameInputCheck::wrong_size; /* size is checked against the byte count below; large fitted inputs (DLSS5_FIT_LARGE) are legal */
 if((!bpp||bpp==4)&&bytes.size()==uint64_t(width)*height*4){for(size_t i=0;i<bytes.size();i++)if(i%4!=3&&bytes[i])return NativeFrameInputCheck::valid;return NativeFrameInputCheck::black;}
 if((bpp&&bpp!=8)||bytes.size()!=uint64_t(width)*height*8)return NativeFrameInputCheck::wrong_size;
 bool rgb=false;
 for(size_t i=0;i<bytes.size();i+=2){uint16_t h;std::memcpy(&h,bytes.data()+i,2);
  if((h&0x7c00)==0x7c00)return NativeFrameInputCheck::nonfinite;
  if((i/2)%4!=3&&(h&0x7fff))rgb=true;
 }
 return rgb?NativeFrameInputCheck::valid:NativeFrameInputCheck::black;
}
