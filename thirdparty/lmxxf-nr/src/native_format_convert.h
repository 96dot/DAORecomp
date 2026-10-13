#pragma once
#include "native_pso.h"
#include "native_format_fallback.h"
#include "native_shader_cache.h"
// Colour-format fallback pass (0.38): reads a game colour texture of a fallback format (native_format_fallback.h) through
// a typed SRV and writes a private RGBA16F texture, which then takes the existing RGBA16F route unchanged.
// Only used for formats outside NativeIsGameColor(); originally accepted formats never reach it.
// Descriptors live in a ring (the game may alternate colour textures; a slot is rewritten only kRing records later).
class NativeFormatConvert {
 static constexpr UINT kRing=16;
 ID3D12DescriptorHeap*heap{};ID3D12RootSignature*root{};ID3D12PipelineState*pso{};ID3D12Device*device{};UINT stride{},next{};bool failed{};
 static void ck(HRESULT h,const char*w){if(FAILED(h))throw std::runtime_error(std::string("format convert ")+w+" HRESULT="+std::to_string(unsigned(h)));}
 static void transition(ID3D12GraphicsCommandList*c,ID3D12Resource*r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){if(a==b)return;D3D12_RESOURCE_BARRIER t{};t.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;t.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};c->ResourceBarrier(1,&t);}
 void create(ID3D12Device*d){
  D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,2*kRing,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};ck(d->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)),"heap");stride=d->GetDescriptorHandleIncrementSize(hd.Type);
  D3D12_DESCRIPTOR_RANGE ranges[2]={{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,1}};
  D3D12_ROOT_PARAMETER p[2]{};p[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;p[0].DescriptorTable={2,ranges};p[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;p[1].Constants={0,0,3};
  D3D12_ROOT_SIGNATURE_DESC rd{};rd.NumParameters=2;rd.pParameters=p;ID3DBlob*b=nullptr,*e=nullptr;HRESULT hr=D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&b,&e);if(e)e->Release();ck(hr,"rootsig-serialize");
  hr=d->CreateRootSignature(0,b->GetBufferPointer(),b->GetBufferSize(),IID_PPV_ARGS(&root));b->Release();b=nullptr;ck(hr,"rootsig");
  hr=CompileNativeShader(shader_path.empty()?NativeLabPath(L"native-game-tiled-assets\\native_format_convert.hlsl"):shader_path,nullptr,"main",&b,&e);if(e)e->Release();ck(hr,"compile native_format_convert.hlsl");
  D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root;pd.CS={b->GetBufferPointer(),b->GetBufferSize()};hr=NativeCreateComputePipelineState(d,&pd,IID_PPV_ARGS(&pso));b->Release();ck(hr,"pso");
  device=d;device->AddRef();
 }
public:
 NativeFormatConvert()=default;NativeFormatConvert(const NativeFormatConvert&)=delete;
 ~NativeFormatConvert(){if(heap)heap->Release();if(root)root->Release();if(pso)pso->Release();if(device)device->Release();}
 std::wstring shader_path; /* empty: <lab>\native-game-tiled-assets\native_format_convert.hlsl (tests point it elsewhere) */
 /* Builds the pipeline once; false (and the reason in *why) when it cannot, e.g. the .hlsl is missing from an old package:
    the caller then treats the format as unsupported instead of failing the frame. */
 bool Available(ID3D12Device*d,std::string*why=nullptr){if(heap)return true;if(failed||!d)return false;try{create(d);return true;}catch(const std::exception&e){failed=true;if(why)*why=e.what();return false;}}
 // source: fallback-format texture in source_state (restored); view = NativeFallbackColorView(format).
 // target: RGBA16F texture with ALLOW_UNORDERED_ACCESS in target_state (restored). Converts the top-left width x height.
 void Record(ID3D12GraphicsCommandList*c,ID3D12Resource*source,DXGI_FORMAT view,D3D12_RESOURCE_STATES source_state,ID3D12Resource*target,D3D12_RESOURCE_STATES target_state,UINT width,UINT height){
  if(!c||!source||!target||view==DXGI_FORMAT_UNKNOWN||!width||!height)throw std::runtime_error("format convert contract");
  const auto td=target->GetDesc();if(td.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT||!(td.Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)||td.Width<width||td.Height<height)throw std::runtime_error("format convert target");
  if(!heap){if(failed)throw std::runtime_error("format convert unavailable");ID3D12Device*d=nullptr;ck(target->GetDevice(IID_PPV_ARGS(&d)),"device");try{create(d);}catch(...){d->Release();failed=true;throw;}d->Release();}
  const UINT slot=next;next=(next+1)%kRing;auto cpu=heap->GetCPUDescriptorHandleForHeapStart();cpu.ptr+=SIZE_T(2*slot)*stride;
  D3D12_SHADER_RESOURCE_VIEW_DESC sv{};sv.Format=view;sv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;sv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;sv.Texture2D.MipLevels=1;device->CreateShaderResourceView(source,&sv,cpu);cpu.ptr+=stride;
  D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};uv.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;uv.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;device->CreateUnorderedAccessView(target,nullptr,&uv,cpu);
  transition(c,source,source_state,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);transition(c,target,target_state,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  auto gpu=heap->GetGPUDescriptorHandleForHeapStart();gpu.ptr+=UINT64(2*slot)*stride;const UINT k[3]={width,height,NativeFallbackOpaque(view)?1u:0u};
  c->SetDescriptorHeaps(1,&heap);c->SetComputeRootSignature(root);c->SetPipelineState(pso);c->SetComputeRootDescriptorTable(0,gpu);c->SetComputeRoot32BitConstants(1,3,k,0);c->Dispatch((width+7)/8,(height+7)/8,1);
  transition(c,target,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,target_state);transition(c,source,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,source_state);
 }
};
