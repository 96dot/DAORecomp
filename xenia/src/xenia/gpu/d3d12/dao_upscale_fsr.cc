// DAO: temporal upscaling's FidelityFX (FSR 3.1 / FSR 4) backend, and the
// FidelityFX API it and frame generation call - see dao_upscale.h.
#include "xenia/gpu/d3d12/dao_upscale_impl.h"

#include <algorithm>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "xenia/base/logging.h"
#include "xenia/base/platform_win.h"
#include "xenia/gpu/d3d12/d3d12_command_processor.h"

// After platform_win.h (needs windows.h).
#include <psapi.h>

#include "dx12/ffx_api_dx12.h"
#include "ffx_upscale.h"

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
  // (Only the first modules, as many as the array holds, are searched.)
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

class FsrBackend : public UpscalerBackend {
 public:
  using UpscalerBackend::UpscalerBackend;
  ~FsrBackend() override;

  const char* name() const override { return "FSR"; }
  // FSR takes the game's image as it is
  // (FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB).
  bool LinearSdrColor() const override { return false; }
  bool Sharpens() const override { return true; }
  bool EnsureContext(const ContextDesc& desc) override;
  uint32_t JitterPhases() const override { return jitter_phase_count_; }
  void JitterOffset(uint32_t index, float& x, float& y) const override;
  void RecordDispatch(const Frame& frame) override;

 private:
  bool Load();
  // The upscaler versions available, newest first.
  void QueryVersions(std::vector<uint64_t>& ids,
                     std::vector<const char*>& names) const;
  void DestroyContext();
  static void ExecuteDispatch(void* context,
                              ID3D12GraphicsCommandList* command_list);

  FfxApi& ffx_ = upscaler_.ffx();
  bool loaded_ = false;
  // ffxQuery takes the context by non-const pointer, also from const methods.
  mutable ffxContext context_ = nullptr;
  ContextDesc context_desc_ = {};
  std::string version_name_;
  uint32_t jitter_phase_count_ = 8;
  // What the deferred dispatch needs when the command list is executed.
  ffxDispatchDescUpscale dispatch_desc_ = {};
  // Set by a dispatch that failed (when the command list is executed), seen
  // by the next EnsureContext, at the next upscaled swap.
  std::atomic<bool> dispatch_failed_{false};
  static constexpr uint32_t kMaxDispatchFailures = 3;
  uint32_t dispatch_failures_ = 0;
};

FsrBackend::~FsrBackend() { DestroyContext(); }

bool FsrBackend::Load() {
  if (loaded_ || failed_) {
    return !failed_;
  }
  if (!ffx_.Load()) {
    failed_ = true;
    return false;
  }
  loaded_ = true;
  // The available upscaler versions, for the log.
  std::vector<uint64_t> ids;
  std::vector<const char*> names;
  QueryVersions(ids, names);
  for (const char* name : names) {
    XELOGI("FSR: available upscaler {}", name ? name : "?");
  }
  return true;
}

void FsrBackend::QueryVersions(std::vector<uint64_t>& ids,
                               std::vector<const char*>& names) const {
  ffxQueryDescGetVersions versions = {};
  versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
  versions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
  versions.device = upscaler_.device();
  uint64_t count = 0;
  versions.outputCount = &count;
  ffx_.query(nullptr, &versions.header);
  ids.assign(count, 0);
  names.assign(count, nullptr);
  versions.versionIds = ids.data();
  versions.versionNames = names.data();
  ffx_.query(nullptr, &versions.header);
}

bool FsrBackend::EnsureContext(const ContextDesc& desc) {
  // A dispatch failed in the commands run since the last swap (that frame
  // showed the upscaled image as it was before). FSR is started again (an
  // overlay such as OptiScaler may have failed once, switching upscalers),
  // and given up for the session after kMaxDispatchFailures, as when it can't
  // be created: the image is scaled instead.
  if (dispatch_failed_.exchange(false, std::memory_order_relaxed) &&
      context_) {
    DestroyContext();
    if (++dispatch_failures_ >= kMaxDispatchFailures) {
      XELOGE("FSR: the upscaler failed {} times, scaling the image instead{}",
             dispatch_failures_, ffx_.ProxyNote());
      failed_ = true;
      return false;
    }
    XELOGW("FSR: the upscaler failed, starting it again");
    restarted_ = true;
  }
  if (context_ && context_desc_ == desc) {
    return true;
  }
  if (!Load()) {
    return false;
  }
  DestroyContext();

  // Pick the version: the newest by default (FSR 4 on RDNA 4), or the one
  // asked for.
  uint64_t version_id = 0;
  const std::string& wanted = cvars::dao_temporal_upscaler;
  if (wanted == "fsr3" || wanted == "fsr4") {
    std::vector<uint64_t> ids;
    std::vector<const char*> names;
    QueryVersions(ids, names);
    const char prefix = wanted == "fsr3" ? '3' : '4';
    for (size_t i = 0; i < names.size(); ++i) {
      if (names[i] && names[i][0] == prefix) {
        version_id = ids[i];
        break;
      }
    }
    if (!version_id) {
      XELOGW("FSR: no {} upscaler available, using the default", wanted);
    }
  }

  ffxCreateContextDescUpscale create = {};
  create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
  // The input is the tonemapped (gamma-encoded) image, or the HDR one
  // (linear).
  create.flags = desc.hdr ? FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE
                          : FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
  create.maxRenderSize = {desc.render_size[0], desc.render_size[1]};
  create.maxUpscaleSize = {desc.display_size[0], desc.display_size[1]};
  create.fpMessage = FfxMessage;
  ffxCreateContextDescUpscaleVersion version = {};
  version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
  version.version = FFX_UPSCALER_VERSION;
  ffxCreateBackendDX12Desc backend = {};
  backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
  backend.device = upscaler_.device();
  ffxOverrideVersion override_version = {};
  override_version.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
  override_version.versionId = version_id;
  create.header.pNext = &version.header;
  version.header.pNext = &backend.header;
  if (version_id) {
    backend.header.pNext = &override_version.header;
  }
  ffxReturnCode_t result = ffx_.create(&context_, &create.header, nullptr);
  if (result != FFX_API_RETURN_OK) {
    XELOGE("FSR: creating the upscaler failed ({}){}", result,
           ffx_.ProxyNote());
    context_ = nullptr;
    failed_ = true;
    return false;
  }
  context_desc_ = desc;

  ffxQueryGetProviderVersion provider = {};
  provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
  ffx_.query(&context_, &provider.header);
  version_name_ = provider.versionName ? provider.versionName : "?";

  int32_t phases = 0;
  ffxQueryDescUpscaleGetJitterPhaseCount phase_query = {};
  phase_query.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT;
  phase_query.renderWidth = desc.render_size[0];
  phase_query.displayWidth = desc.display_size[0];
  phase_query.pOutPhaseCount = &phases;
  jitter_phase_count_ =
      ffx_.query(&context_, &phase_query.header) == FFX_API_RETURN_OK &&
              phases > 0
          ? uint32_t(phases)
          : 8;
  // (Through a proxy, the version it reports, not necessarily what runs.)
  XELOGI("FSR: upscaler {}{} - {}x{} -> {}x{}, {} jitter phases",
         version_name_,
         ffx_.proxy.empty() ? "" : " as " + ffx_.proxy + " reports it",
         desc.render_size[0], desc.render_size[1], desc.display_size[0],
         desc.display_size[1], jitter_phase_count_);
  return true;
}

void FsrBackend::DestroyContext() {
  if (!context_) {
    return;
  }
  if (!gpu_done_) {
    upscaler_.AwaitSubmittedFrames();
  }
  ffx_.destroy(&context_, nullptr);
  context_ = nullptr;
  jitter_phase_count_ = 8;
}

void FsrBackend::JitterOffset(uint32_t index, float& x, float& y) const {
  if (context_) {
    ffxQueryDescUpscaleGetJitterOffset offset = {};
    offset.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET;
    offset.index = int32_t(index);
    offset.phaseCount = int32_t(jitter_phase_count_);
    offset.pOutX = &x;
    offset.pOutY = &y;
    if (ffx_.query(&context_, &offset.header) == FFX_API_RETURN_OK) {
      return;
    }
  }
  HaltonJitter(index, x, y);
}

void FsrBackend::RecordDispatch(const Frame& frame) {
  ffxDispatchDescUpscale& desc = dispatch_desc_;
  desc = {};
  desc.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
  desc.color = ffxApiGetResourceDX12(frame.color);
  desc.depth = ffxApiGetResourceDX12(frame.depth);
  desc.motionVectors = ffxApiGetResourceDX12(frame.motion_vectors);
  desc.output = ffxApiGetResourceDX12(frame.output,
                                      FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
  desc.jitterOffset = {frame.jitter[0], frame.jitter[1]};
  desc.motionVectorScale = {1.0f, 1.0f};
  desc.renderSize = {frame.render_size[0], frame.render_size[1]};
  desc.upscaleSize = {frame.display_size[0], frame.display_size[1]};
  desc.enableSharpening = frame.sharpness > 0.0f;
  desc.sharpness = frame.sharpness;
  desc.frameTimeDelta = frame.frame_ms;
  desc.preExposure = 1.0f;
  desc.reset = frame.reset;
  // Rough camera values; FSR uses them for its depth-based heuristics.
  desc.cameraNear = 0.1f;
  desc.cameraFar = 1000.0f;
  desc.cameraFovAngleVertical = 1.0f;
  desc.viewSpaceToMetersFactor = 1.0f;
  desc.flags = context_desc_.hdr ? 0 : FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;
  upscaler_.command_processor().GetDeferredCommandList().DaoCallback(
      ExecuteDispatch, this);
}

void FsrBackend::ExecuteDispatch(void* context,
                                 ID3D12GraphicsCommandList* command_list) {
  FsrBackend& self = *static_cast<FsrBackend*>(context);
  if (!self.context_) {
    return;
  }
  self.dispatch_desc_.commandList = command_list;
  ffxReturnCode_t result =
      self.ffx_.dispatch(&self.context_, &self.dispatch_desc_.header);
  if (result != FFX_API_RETURN_OK) {
    // Seen by the next EnsureContext.
    self.dispatch_failed_.store(true, std::memory_order_relaxed);
    static std::atomic<uint32_t> s_failures{0};
    if (LogNth(s_failures)) {
      XELOGE("FSR: dispatch failed ({}){}", result, self.ffx_.ProxyNote());
    }
  }
}

}  // namespace

FfxApi::~FfxApi() {
  if (module) {
    FreeLibrary(module);
  }
}

bool FfxApi::Load() {
  if (module || failed) {
    return !failed;
  }
  module = LoadLibraryW(L"amd_fidelityfx_loader_dx12.dll");
  if (module) {
    create = reinterpret_cast<PfnFfxCreateContext>(
        GetProcAddress(module, "ffxCreateContext"));
    destroy = reinterpret_cast<PfnFfxDestroyContext>(
        GetProcAddress(module, "ffxDestroyContext"));
    query = reinterpret_cast<PfnFfxQuery>(GetProcAddress(module, "ffxQuery"));
    dispatch =
        reinterpret_cast<PfnFfxDispatch>(GetProcAddress(module, "ffxDispatch"));
    configure = reinterpret_cast<PfnFfxConfigure>(
        GetProcAddress(module, "ffxConfigure"));
  }
  if (!create || !destroy || !query || !dispatch) {
    XELOGE(
        "FSR: amd_fidelityfx_loader_dx12.dll is missing or incomplete, FSR "
        "and frame generation are off");
    failed = true;
    return false;
  }
  proxy = ProxyDllInProgramFolder();
  if (!proxy.empty()) {
    XELOGW(
        "FSR: {} in the program folder isn't the system's (OptiScaler?); it "
        "may handle FidelityFX's calls",
        proxy);
  }
  return true;
}

std::unique_ptr<UpscalerBackend> CreateFsrBackend(DaoUpscalerImpl& upscaler) {
  return std::make_unique<FsrBackend>(upscaler);
}

}  // namespace d3d12
}  // namespace gpu
}  // namespace xe
