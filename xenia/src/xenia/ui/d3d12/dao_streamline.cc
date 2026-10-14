// DAO: NVIDIA Streamline - see dao_streamline.h.
#include "xenia/ui/d3d12/dao_streamline.h"

#include <bcrypt.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/ui/graphics_provider.h"

#include "sl.h"

DECLARE_int32(dao_low_latency);
DECLARE_string(dao_temporal_upscaler);

namespace xe {
namespace ui {
namespace d3d12 {
namespace dao_streamline {

namespace {

// sl.interposer.dll of Streamline 2.14.1 (installer/deps.py STREAMLINE): the
// one the installer puts next to the executable, which checks its plug-ins'
// signatures as it loads them.
constexpr std::string_view kInterposerSha256 =
    "8c87c9499461da561edd529aa9bf7831d67d7b94ebb1c1a5ed54ef4934e1ea4c";
// Far larger than it (652,928 bytes): a file this large isn't read.
constexpr uint64_t kInterposerMaxSize = 16 * 1024 * 1024;
// This project's ID: it identifies a game that has no NVIDIA application ID.
constexpr char kProjectId[] = "9b94314d-6da2-4822-a757-c0426536dca3";

// Set by Initialize, SetDevice and CreateDirectQueue on the provider's thread
// before the others start (Stop too, if they fail), and by Shutdown after
// they've stopped.
HMODULE s_module = nullptr;
bool s_active = false;
bool s_starts_after_restart = false;
bool s_reflex = false;
bool s_dlss = false;
// Its proxies, a reference each kept for its lifetime (never released: the
// process ends without).
ID3D12Device* s_proxy_device = nullptr;
IDXGIFactory2* s_proxy_factory = nullptr;
ID3D12CommandQueue* s_proxy_queue = nullptr;

PFun_slInit* s_init = nullptr;
PFun_slShutdown* s_shutdown = nullptr;
PFun_slIsFeatureSupported* s_is_feature_supported = nullptr;
PFun_slSetD3DDevice* s_set_d3d_device = nullptr;
PFun_slUpgradeInterface* s_upgrade_interface = nullptr;
PFun_slGetNativeInterface* s_get_native_interface = nullptr;
PFun_slGetFeatureFunction* s_get_feature_function = nullptr;
PFun_slGetNewFrameToken* s_get_new_frame_token = nullptr;

std::filesystem::path ExecutableDirectory() {
  std::wstring path(MAX_PATH, L'\0');
  for (;;) {
    const DWORD length =
        GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
    if (!length) {
      return {};
    }
    if (length < path.size()) {
      path.resize(length);
      return std::filesystem::path(path).parent_path();
    }
    path.resize(path.size() * 2);
  }
}

// A file handle, closed when it goes.
struct File {
  HANDLE handle;
  ~File() {
    if (handle != INVALID_HANDLE_VALUE) {
      CloseHandle(handle);
    }
  }
};

// The open file's SHA-256 as lower-case hex, or "" if it can't be read or is
// larger than max_size. (DAORecomp's hash.cpp has its own: Xenia doesn't use
// DAORecomp's code.)
std::string FileSha256(HANDLE file, uint64_t max_size) {
  LARGE_INTEGER size;
  if (!GetFileSizeEx(file, &size) || uint64_t(size.QuadPart) > max_size) {
    return {};
  }
  std::vector<uint8_t> data(size_t(size.QuadPart));
  DWORD read = 0;
  if (!ReadFile(file, data.data(), DWORD(data.size()), &read, nullptr) ||
      read != data.size()) {
    return {};
  }
  std::array<uint8_t, 32> digest;
  if (!BCRYPT_SUCCESS(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
                                 data.data(), ULONG(data.size()),
                                 digest.data(), ULONG(digest.size())))) {
    return {};
  }
  std::string hex;
  for (const uint8_t byte : digest) {
    hex += "0123456789abcdef"[byte >> 4];
    hex += "0123456789abcdef"[byte & 15];
  }
  return hex;
}

// Streamline's warnings and errors, into the log (no log file of its own).
void LogMessage(sl::LogType type, const char* message) {
  if (type == sl::LogType::eInfo || !message) {
    return;
  }
  std::string_view text(message);
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
    text.remove_suffix(1);
  }
  if (type == sl::LogType::eError) {
    XELOGE("Streamline: {}", text);
  } else {
    XELOGW("Streamline: {}", text);
  }
}

template <typename Function>
bool Import(Function*& function, const char* name) {
  function = reinterpret_cast<Function*>(GetProcAddress(s_module, name));
  return function != nullptr;
}

// Stops it, after slInit, when what it's for can't be had.
void Stop() {
  s_shutdown();
  s_active = false;
  s_reflex = false;
  s_dlss = false;
  s_proxy_device = nullptr;
  s_proxy_factory = nullptr;
  s_proxy_queue = nullptr;
}

// Replaces *object with Streamline's proxy of it, a reference of its own for
// this file; false if it can't.
template <typename Interface>
bool Upgrade(Interface*& object) {
  // The reference this file keeps: the proxy takes it over, or it stays on
  // the object if Streamline hooks the object itself (as it may a factory).
  object->AddRef();
  void* proxy = object;
  const sl::Result result = s_upgrade_interface(&proxy);
  if (result != sl::Result::eOk || !proxy) {
    object->Release();
    XELOGW("Streamline: no proxy of the {} ({})",
           std::is_same_v<Interface, ID3D12Device> ? "device" : "factory",
           uint32_t(result));
    return false;
  }
  object = static_cast<Interface*>(proxy);
  return true;
}

}  // namespace

void Initialize(const DXGI_ADAPTER_DESC& adapter) {
  const bool nvidia = GraphicsProvider::GpuVendorID(adapter.VendorId) ==
                      GraphicsProvider::GpuVendorID::kNvidia;
  // DAO_STREAMLINE_ANY_GPU=1 (testing): on any GPU, where Reflex's sleep does
  // nothing but the rest of it runs.
  const char* any_gpu = std::getenv("DAO_STREAMLINE_ANY_GPU");
  if (!nvidia && !(any_gpu && std::string_view(any_gpu) == "1")) {
    return;
  }
  const std::filesystem::path directory = ExecutableDirectory();
  const std::filesystem::path interposer = directory / L"sl.interposer.dll";
  // Open, so it can't be written, renamed or deleted, from its check until
  // it's loaded.
  const File file{directory.empty()
                      ? INVALID_HANDLE_VALUE
                      : CreateFileW(interposer.c_str(), GENERIC_READ,
                                    FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr)};
  if (file.handle == INVALID_HANDLE_VALUE) {
    XELOGW("Streamline: no sl.interposer.dll next to the game, so no NVIDIA "
           "Reflex or DLSS");
    return;
  }
  if (FileSha256(file.handle, kInterposerMaxSize) != kInterposerSha256) {
    XELOGW("Streamline: sl.interposer.dll isn't the one the installer put "
           "there, so it isn't loaded (install again to restore it)");
    return;
  }
  // Only what needs it: NVIDIA Reflex (low latency) or DLSS (the temporal
  // upscaler, chosen for the next start; only on an NVIDIA GPU, as its license
  // has it). Low latency turned on in the game starts it with the next start.
  const bool reflex = cvars::dao_low_latency != 0;
  const bool dlss = nvidia && cvars::dao_temporal_upscaler == "dlss";
  if (!reflex && !dlss) {
    s_starts_after_restart = true;
    return;
  }
  // Its own dependencies from its folder and System32 only.
  s_module = LoadLibraryExW(
      interposer.c_str(), nullptr,
      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!s_module) {
    XELOGW("Streamline: sl.interposer.dll couldn't be loaded ({})",
           GetLastError());
    return;
  }
  if (!Import(s_init, "slInit") || !Import(s_shutdown, "slShutdown") ||
      !Import(s_is_feature_supported, "slIsFeatureSupported") ||
      !Import(s_set_d3d_device, "slSetD3DDevice") ||
      !Import(s_upgrade_interface, "slUpgradeInterface") ||
      !Import(s_get_native_interface, "slGetNativeInterface") ||
      !Import(s_get_feature_function, "slGetFeatureFunction") ||
      !Import(s_get_new_frame_token, "slGetNewFrameToken")) {
    XELOGW("Streamline: sl.interposer.dll lacks its functions");
    FreeLibrary(s_module);
    s_module = nullptr;
    return;
  }

  const std::wstring plugins = directory.wstring();
  const wchar_t* plugin_paths[] = {plugins.c_str()};
  // Reflex's plug-ins whatever's on (NVIDIA asks for its markers with any
  // feature; with low latency off it doesn't wait); DLSS's only for it
  // (loading it takes time at start).
  sl::Feature features[3] = {sl::kFeatureReflex, sl::kFeaturePCL};
  uint32_t feature_count = 2;
  if (dlss) {
    features[feature_count++] = sl::kFeatureDLSS;
  }
  sl::Preferences preferences;
  preferences.showConsole = false;
  preferences.logLevel = sl::LogLevel::eDefault;
  preferences.pathsToPlugins = plugin_paths;
  preferences.numPathsToPlugins = 1;
  preferences.pathToLogsAndData = nullptr;
  preferences.logMessageCallback = LogMessage;
  // Manual hooking; and no updates over the air (nothing downloaded while the
  // game runs: the installer's DLLs only), as eAllowOTA and
  // eLoadDownloadedPlugins, which the default has, aren't set.
  preferences.flags = sl::PreferenceFlags::eDisableCLStateTracking |
                      sl::PreferenceFlags::eUseManualHooking;
  preferences.featuresToLoad = features;
  preferences.numFeaturesToLoad = feature_count;
  preferences.engine = sl::EngineType::eCustom;
  preferences.engineVersion = "1.0";
  preferences.projectId = kProjectId;
  preferences.renderAPI = sl::RenderAPI::eD3D12;
  const sl::Result result = s_init(preferences, sl::kSDKVersion);
  if (result != sl::Result::eOk) {
    XELOGW("Streamline: couldn't start ({})", uint32_t(result));
    FreeLibrary(s_module);
    s_module = nullptr;
    return;
  }
  s_active = true;

  LUID luid = adapter.AdapterLuid;
  sl::AdapterInfo adapter_info;
  adapter_info.deviceLUID = reinterpret_cast<uint8_t*>(&luid);
  adapter_info.deviceLUIDSizeInBytes = sizeof(luid);
  const sl::Result reflex_supported =
      s_is_feature_supported(sl::kFeatureReflex, adapter_info);
  s_reflex = reflex_supported == sl::Result::eOk;
  if (!s_reflex) {
    XELOGW("Streamline: NVIDIA Reflex isn't supported here ({})",
           uint32_t(reflex_supported));
  }
  if (dlss) {
    const sl::Result dlss_supported =
        s_is_feature_supported(sl::kFeatureDLSS, adapter_info);
    s_dlss = dlss_supported == sl::Result::eOk;
    if (!s_dlss) {
      XELOGW("Streamline: NVIDIA DLSS isn't supported here ({})",
             uint32_t(dlss_supported));
    }
  }
  // Nothing wanted that it has: not its proxies and markers every frame for
  // nothing.
  if (!(reflex && s_reflex) && !s_dlss) {
    // (With low latency off, turning it on would start it next time.)
    s_starts_after_restart = !reflex && s_reflex;
    Stop();
    // Nothing of it is in use yet.
    FreeLibrary(s_module);
    s_module = nullptr;
    return;
  }
  XELOGI("Streamline: started (NVIDIA Reflex {}, DLSS {})",
         s_reflex ? "supported" : "unsupported",
         s_dlss                                   ? "on"
         : cvars::dao_temporal_upscaler == "dlss" ? "unsupported"
                                                  : "not chosen");
}

void SetDevice(IDXGIFactory2* factory, ID3D12Device* device) {
  if (!s_active) {
    return;
  }
  const sl::Result result = s_set_d3d_device(device);
  if (result != sl::Result::eOk) {
    XELOGW("Streamline: couldn't use the device ({})", uint32_t(result));
    Stop();
    return;
  }
  ID3D12Device* proxy_device = device;
  IDXGIFactory2* proxy_factory = factory;
  if (!Upgrade(proxy_device)) {
    Stop();
    return;
  }
  if (!Upgrade(proxy_factory)) {
    proxy_device->Release();
    Stop();
    return;
  }
  s_proxy_device = proxy_device;
  s_proxy_factory = proxy_factory;
}

HRESULT CreateDirectQueue(ID3D12Device* device,
                          const D3D12_COMMAND_QUEUE_DESC& desc,
                          ID3D12CommandQueue** queue) {
  if (!s_proxy_device) {
    return device->CreateCommandQueue(&desc, IID_PPV_ARGS(queue));
  }
  ID3D12CommandQueue* proxy;
  const HRESULT result =
      s_proxy_device->CreateCommandQueue(&desc, IID_PPV_ARGS(&proxy));
  if (FAILED(result)) {
    return result;
  }
  void* native = nullptr;
  if (s_get_native_interface(proxy, &native) != sl::Result::eOk || !native) {
    // Without the native queue, others would get Streamline's device from
    // it: not Streamline, then.
    XELOGW("Streamline: no native queue");
    proxy->Release();
    Stop();
    return device->CreateCommandQueue(&desc, IID_PPV_ARGS(queue));
  }
  // The proxy (kept for the swap chain), and the native queue for the caller
  // (slGetNativeInterface's reference).
  s_proxy_queue = proxy;
  *queue = static_cast<ID3D12CommandQueue*>(native);
  return result;
}

IDXGIFactory2* SwapChainFactory() { return s_proxy_factory; }

ID3D12CommandQueue* SwapChainQueue() { return s_proxy_queue; }

void Shutdown() {
  if (s_active) {
    Stop();
  }
}

bool Active() { return s_active; }

bool StartsAfterRestart() { return s_starts_after_restart; }

bool ReflexSupported() { return s_reflex; }

bool DlssSupported() { return s_dlss; }

void* CoreFunction(const char* name) {
  return s_active ? reinterpret_cast<void*>(GetProcAddress(s_module, name))
                  : nullptr;
}

void* FeatureFunction(uint32_t feature, const char* name) {
  void* function = nullptr;
  if (!s_active ||
      s_get_feature_function(feature, name, function) != sl::Result::eOk) {
    return nullptr;
  }
  return function;
}

void* FrameToken(uint32_t index) {
  sl::FrameToken* token = nullptr;
  if (!s_active || s_get_new_frame_token(token, &index) != sl::Result::eOk) {
    return nullptr;
  }
  return token;
}

}  // namespace dao_streamline
}  // namespace d3d12
}  // namespace ui
}  // namespace xe
