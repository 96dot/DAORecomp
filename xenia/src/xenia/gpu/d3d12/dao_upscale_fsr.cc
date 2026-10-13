// DAO: temporal upscaling's FidelityFX (FSR 3.1 / FSR 4) context and
// dispatch - see dao_upscale.h.
#include "xenia/gpu/d3d12/dao_upscale_impl.h"

#include <algorithm>
#include <iterator>
#include <string>
#include <string_view>

#include "xenia/base/logging.h"
#include "xenia/base/platform_win.h"
#include "xenia/gpu/d3d12/d3d12_command_processor.h"

// After platform_win.h (needs windows.h).
#include <psapi.h>

DECLARE_string(dao_temporal_upscaler);

namespace xe {
namespace gpu {
namespace d3d12 {

namespace {

// ASCII, anything else as '?'.
std::string NarrowAscii(const wchar_t* text) {
  std::string narrow;
  for (const wchar_t* c = text; c && *c; ++c) {
    narrow += char(*c < 128 ? *c : '?');
  }
  return narrow;
}

void FfxMessage(uint32_t type, const wchar_t* message) {
  static std::atomic<uint32_t> s_errors{0}, s_warnings{0};
  if (!LogNth(type == FFX_API_MESSAGE_TYPE_ERROR ? s_errors : s_warnings)) {
    return;
  }
  const std::string text = NarrowAscii(message);
  if (type == FFX_API_MESSAGE_TYPE_ERROR) {
    XELOGE("FSR: {}", text);
  } else {
    XELOGW("FSR: {}", text);
  }
}

// The first DLL loaded from the program folder under the name of a system one
// that mods load themselves as, or an empty string. The names are those
// OptiScaler's guide lists (it can take over FSR's calls); kept by hand.
std::string ProxyDllInProgramFolder() {
  wchar_t exe[MAX_PATH];
  const DWORD exe_length = GetModuleFileNameW(nullptr, exe, MAX_PATH);
  if (!exe_length || exe_length >= MAX_PATH) {
    return {};
  }
  const std::wstring_view exe_path(exe, exe_length);
  const std::wstring_view folder =
      exe_path.substr(0, exe_path.rfind(L'\\') + 1);
  // (A process with more modules isn't searched past these.)
  HMODULE modules[1024];
  DWORD bytes = 0;
  if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules),
                          &bytes)) {
    return {};
  }
  const DWORD count =
      std::min<DWORD>(bytes / sizeof(HMODULE), DWORD(std::size(modules)));
  for (DWORD i = 0; i < count; ++i) {
    wchar_t path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(modules[i], path, MAX_PATH);
    if (length <= folder.size() || length >= MAX_PATH ||
        _wcsnicmp(path, folder.data(), folder.size()) != 0) {
      continue;
    }
    const wchar_t* name = path + folder.size();
    for (const wchar_t* system_name :
         {L"dxgi.dll", L"d3d12.dll", L"winmm.dll", L"version.dll",
          L"dbghelp.dll", L"winhttp.dll", L"wininet.dll"}) {
      if (_wcsicmp(name, system_name) == 0) {
        return NarrowAscii(name);
      }
    }
  }
  return {};
}

}  // namespace

bool DaoUpscalerImpl::LoadFfx() {
  if (ffx_module_ || ffx_failed_) {
    return !ffx_failed_;
  }
  ffx_module_ = LoadLibraryW(L"amd_fidelityfx_loader_dx12.dll");
  if (ffx_module_) {
    ffx_create_ = reinterpret_cast<PfnFfxCreateContext>(
        GetProcAddress(ffx_module_, "ffxCreateContext"));
    ffx_destroy_ = reinterpret_cast<PfnFfxDestroyContext>(
        GetProcAddress(ffx_module_, "ffxDestroyContext"));
    ffx_query_ =
        reinterpret_cast<PfnFfxQuery>(GetProcAddress(ffx_module_, "ffxQuery"));
    ffx_dispatch_ = reinterpret_cast<PfnFfxDispatch>(
        GetProcAddress(ffx_module_, "ffxDispatch"));
    ffx_configure_ = reinterpret_cast<PfnFfxConfigure>(
        GetProcAddress(ffx_module_, "ffxConfigure"));
  }
  // (ffxConfigure only for frame generation: EnsureFrameGeneration.)
  if (!ffx_create_ || !ffx_destroy_ || !ffx_query_ || !ffx_dispatch_) {
    XELOGE(
        "FSR: amd_fidelityfx_loader_dx12.dll is missing or incomplete, "
        "temporal upscaling is off");
    ffx_failed_ = true;
    return false;
  }
  ffx_proxy_ = ProxyDllInProgramFolder();
  if (!ffx_proxy_.empty()) {
    XELOGW(
        "FSR: {} in the program folder isn't the system's (OptiScaler?); it "
        "may handle the upscaler's calls",
        ffx_proxy_);
  }

  // List the available upscaler versions for the log.
  ID3D12Device* device = cp_.GetD3D12Provider().GetDevice();
  ffxQueryDescGetVersions versions = {};
  versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
  versions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
  versions.device = device;
  uint64_t count = 0;
  versions.outputCount = &count;
  ffx_query_(nullptr, &versions.header);
  std::vector<uint64_t> ids(count);
  std::vector<const char*> names(count);
  versions.versionIds = ids.data();
  versions.versionNames = names.data();
  ffx_query_(nullptr, &versions.header);
  for (uint64_t i = 0; i < count; ++i) {
    XELOGI("FSR: available upscaler {}", names[i] ? names[i] : "?");
  }
  return true;
}

bool DaoUpscalerImpl::EnsureContext(uint32_t render_width,
                                    uint32_t render_height,
                                    uint32_t display_width,
                                    uint32_t display_height) {
  if (context_ && context_size_[0] == render_width &&
      context_size_[1] == render_height && context_size_[2] == display_width &&
      context_size_[3] == display_height && context_hdr_ == true_hdr_) {
    return true;
  }
  if (!LoadFfx()) {
    return false;
  }
  DestroyContext();

  ID3D12Device* device = cp_.GetD3D12Provider().GetDevice();

  // Pick the version: the newest by default (FSR 4 on RDNA 4), or the one
  // asked for.
  uint64_t version_id = 0;
  const std::string& wanted = cvars::dao_temporal_upscaler;
  if (wanted == "fsr3" || wanted == "fsr4") {
    ffxQueryDescGetVersions versions = {};
    versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    versions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    versions.device = device;
    uint64_t count = 0;
    versions.outputCount = &count;
    ffx_query_(nullptr, &versions.header);
    std::vector<uint64_t> ids(count);
    std::vector<const char*> names(count);
    versions.versionIds = ids.data();
    versions.versionNames = names.data();
    ffx_query_(nullptr, &versions.header);
    const char prefix = wanted == "fsr3" ? '3' : '4';
    for (uint64_t i = 0; i < count; ++i) {
      if (names[i] && names[i][0] == prefix) {
        version_id = ids[i];
        break;
      }
    }
    if (!version_id) {
      XELOGW("FSR: no {} upscaler available, using the default", wanted);
    }
  }

  ffxCreateContextDescUpscale desc = {};
  desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
  // The input is the tonemapped (gamma-encoded) image, or the HDR one
  // (linear).
  desc.flags = true_hdr_ ? FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE
                         : FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
  desc.maxRenderSize = {render_width, render_height};
  desc.maxUpscaleSize = {display_width, display_height};
  desc.fpMessage = FfxMessage;
  ffxCreateContextDescUpscaleVersion version = {};
  version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
  version.version = FFX_UPSCALER_VERSION;
  ffxCreateBackendDX12Desc backend = {};
  backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
  backend.device = device;
  ffxOverrideVersion override_version = {};
  override_version.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
  override_version.versionId = version_id;
  desc.header.pNext = &version.header;
  version.header.pNext = &backend.header;
  if (version_id) {
    backend.header.pNext = &override_version.header;
  }
  ffxReturnCode_t result = ffx_create_(&context_, &desc.header, nullptr);
  if (result != FFX_API_RETURN_OK) {
    XELOGE("FSR: creating the upscaler failed ({}){}", result, ProxyNote());
    context_ = nullptr;
    ffx_failed_ = true;
    return false;
  }
  context_size_[0] = render_width;
  context_size_[1] = render_height;
  context_size_[2] = display_width;
  context_size_[3] = display_height;
  context_hdr_ = true_hdr_;

  ffxQueryGetProviderVersion provider = {};
  provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
  ffx_query_(&context_, &provider.header);
  version_name_ = provider.versionName ? provider.versionName : "?";

  ffxQueryDescUpscaleGetJitterPhaseCount phases = {};
  phases.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT;
  phases.renderWidth = render_width;
  phases.displayWidth = display_width;
  phases.pOutPhaseCount = &jitter_phase_count_;
  if (ffx_query_(&context_, &phases.header) != FFX_API_RETURN_OK ||
      jitter_phase_count_ <= 0) {
    jitter_phase_count_ = 8;
  }
  // (Through a proxy, the version it reports, not necessarily what runs.)
  XELOGI("FSR: upscaler {}{} - {}x{} -> {}x{}, {} jitter phases",
         version_name_,
         ffx_proxy_.empty() ? "" : " as " + ffx_proxy_ + " reports it",
         render_width, render_height, display_width, display_height,
         jitter_phase_count_);
  reset_history_ = true;
  return true;
}

void DaoUpscalerImpl::DestroyContext() {
  if (!context_) {
    return;
  }
  // The context's resources may still be in use by submitted frames: awaited.
  // Not the open submission, which hasn't used it (its dispatches are recorded
  // at swaps, which end their submissions) and which this may be called in the
  // middle of: ending it would drop the rest of the frame's commands.
  const uint64_t current_submission = cp_.GetCurrentSubmission();
  if (current_submission > 1) {  // Otherwise nothing was submitted yet.
    cp_.CheckSubmissionCompletion(current_submission - 1);
  }
  ffx_destroy_(&context_, nullptr);
  context_ = nullptr;
}

void DaoUpscalerImpl::ExecuteDispatch(void* context,
                                      ID3D12GraphicsCommandList* command_list) {
  auto& params = *static_cast<DispatchParams*>(context);
  DaoUpscalerImpl& self = *params.self;
  if (!self.context_) {
    return;
  }
  params.desc.commandList = command_list;
  ffxReturnCode_t result = self.ffx_dispatch_(&self.context_, &params.desc.header);
  if (result != FFX_API_RETURN_OK) {
    // Seen at the next swap (see OnSwap).
    self.dispatch_failed_.store(true, std::memory_order_relaxed);
    static std::atomic<uint32_t> s_failures{0};
    if (LogNth(s_failures)) {
      XELOGE("FSR: dispatch failed ({}){}", result, self.ProxyNote());
    }
  }
}

}  // namespace d3d12
}  // namespace gpu
}  // namespace xe
