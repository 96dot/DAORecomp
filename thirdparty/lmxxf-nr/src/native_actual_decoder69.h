#pragma once
#include "native_lab_paths.h"
#include "native_network_geometry.h"
#include "native_vit_gather.h"
#include "native_split_window.h"
#include "native_decoder_tail69.h"
#include "native_block_skip.h"
#include "native_vram_log.h"

// 1080p/720p processing extents. All sources remain resident on the GPU.
class NativeActualDecoder69 {
 NativeVitGather inverse;
 NativeVitLinear entry,up48;
 NativeSplitWindow split[8];
 NativeC64Shift body48;
 NativeDecoderTail69 tail;
 bool created{};
public:
 void Create(ID3D12Device*d,ID3D12Resource*vit38,ID3D12Resource*skip30,
             ID3D12Resource*skip22,ID3D12Resource*skip14,ID3D12Resource*skip8,
             ID3D12Resource*skip4,const std::wstring&dir,NativeMatrixWorkspace*workspace=nullptr){
  if(created)throw std::runtime_error("actual decoder already created");
  auto read=[&](const std::wstring&name){return NativeReadF32(dir+L"\\"+name,"actual decoder coefficient");};
  const auto geometry=NativeCurrentNetworkGeometry();const UINT W=geometry.processing_width,H=geometry.processing_height;
  const UINT tokens=geometry.VitTokens();
  auto map=NativeVitLogicalMap(tokens,true);
  if(tokens==640){auto captured=read(L"vit-to-hwc.i32");if(captured.size()!=map.size()||std::memcmp(captured.data(),map.data(),map.size()*4))throw std::runtime_error("decoder bridge disagrees with captured inverse map");}
  inverse.Create(d,vit38,map,dir);
  entry.Create(d,inverse.Output(),skip30,tokens,1024,512,false,read(L"decoder39-weights.f32"),dir,true);
  auto*source=entry.Output();
  for(UINT i=0;i<8;i++){
   auto prefix=L"block"+std::to_wstring(40+i)+L"-";
   split[i].Create(d,source,W/32,H/32,NativeDecoderShift(40+i),read(prefix+L"ffwd.f32"),
                   read(prefix+L"ffwd-projection.f32"),read(prefix+L"attention.f32"),dir,false,workspace,(i?1u:0u)|(i<7?2u:0u));
   source=split[i].Output();
  }
  NativeVramLog(d,"dec entry+split8");up48.Create(d,source,skip22,(W/32)*(H/32),512,256,false,read(L"block48-weights.f32"),dir,true);
  body48.Create(d,up48.Output(),W/16,H/16,NativeDecoderShift(48),read(L"block48-ffn.f32"),
                read(L"block48-attention.f32"),dir,false,256,false,workspace);
  NativeVramLog(d,"dec up48+body48");tail.Create(d,body48.Output(),skip14,skip8,skip4,dir,true,workspace);created=true;
 }
 // Callers may submit and fence between these stages without CPU feature copies.
 UINT StageCount()const{return 13;}
 void RecordStage(ID3D12GraphicsCommandList*c,UINT stage,NativeNetworkTimestamps*timer=nullptr){
  if(!created||!c||stage>=StageCount())throw std::runtime_error("actual decoder stage");
  if(stage==0)inverse.Record(c);
  else if(stage==1)entry.Record(c);
  else if(stage<10){if(NativeSkipBlock(40+stage-2))NativeSkipCopy(c,split[stage-2].Input(),split[stage-2].Output(),40+stage-2);else{split[stage-2].Record(c);if(NativeDupBlock(40+stage-2))split[stage-2].Record(c);}}
  else if(stage==10)up48.Record(c);
  else if(stage==11)body48.Record(c);
  else tail.Record(c,timer);
 }
 NativeDecoderTail69&Tail(){return tail;}
 ID3D12Resource*Output()const{return tail.Output();}
};
