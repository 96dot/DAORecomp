/**
 * DAO recomp: neural rendering of the 3D scenes before temporal upscaling -
 * lmxxf's HIP port of DLSS 5's neural rendering for AMD RDNA 4
 * (thirdparty/lmxxf-nr, MIT), LmxxfNrRuntime.dll next to the executable,
 * loaded when it's on. The network's weights are NVIDIA's: never shipped, the
 * player points the game at the ones in lmxxf's package (dao_neural_weights).
 */

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>

#include "LmxxfNrApi.h"
#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/string.h"
#include "xenia/dao_live_settings.h"
#include "xenia/gpu/d3d12/d3d12_command_processor.h"
#include "xenia/gpu/d3d12/dao_frame_guard.h"
#include "xenia/gpu/d3d12/dao_upscale_impl.h"

DEFINE_bool(dao_neural_rendering, false,
            "Neural rendering of the 3D scenes before temporal upscaling "
            "(lmxxf's HIP port of DLSS 5's neural rendering: AMD RDNA 4, the "
            "AMD driver's HIP 7), with the weights in dao_neural_weights.",
            "GPU");
// A string, not a path: a path's config value has repeated separators merged,
// which turns a network share's \\server\share into \server\share.
DEFINE_string(dao_neural_weights, "",
              "Neural rendering: the folder of its weights (block0-ffn.f16 "
              "and the rest) in lmxxf's package.",
              "GPU");
DEFINE_double(dao_neural_strength, 1.0,
              "Neural rendering's strength, 0 (none) to 1 (the network's).",
              "GPU");
// The options lmxxf's packages offer (its CONFIGURATION.md), read when the
// runtime loads.
DEFINE_string(dao_neural_network, "auto",
              "Neural rendering: the network's size, \"auto\" (the smallest "
              "the input fits), "
              "\"1080\", \"900\" or \"720\" (faster); other inputs are "
              "fitted to it.",
              "GPU");
DEFINE_int32(dao_neural_passes, 1,
             "Neural rendering: the network's passes a frame, 1 to 3 (a "
             "stronger style; each costs as much as the first).",
             "GPU");
DEFINE_bool(dao_neural_skin_protect, false,
            "Neural rendering: with more than one pass, skin colours keep the "
            "first pass's look.",
            "GPU");
DEFINE_int32(dao_neural_style, 1,
             "Neural rendering: one of NVIDIA's three styles of the network, 0 "
             "(NVIDIA's default), 1 (lmxxf's default) or 2.",
             "GPU");

namespace xe {
namespace gpu {
namespace d3d12 {

namespace {
// The first of the weights' files, which marks their folder.
constexpr const char* kWeightsFile = "block0-ffn.f16";
// The runtime also takes them in single precision.
constexpr const char* kWeightsFileF32 = "block0-ffn.f32";

// The runtime's options, as its packages' settings (DLSS5_*): set in the
// environment before it loads (it copies the environment then), where they
// override its default-config.txt (DLSS5-AMD/, the installer's).
void SetRuntimeOptions() {
  const std::string& network = cvars::dao_neural_network;
  const std::string_view height = network == "1080" || network == "900" ||
                                          network == "720"
                                      ? std::string_view(network)
                                      : "auto";
  // The ranges DAORecomp's settings.h has (kMaxNeuralPasses, kMaxNeuralStyle).
  const int passes = std::clamp(cvars::dao_neural_passes, 1, 3);
  const int style = std::clamp(cvars::dao_neural_style, 0, 2);
  const std::pair<const wchar_t*, std::wstring> options[] = {
      // The game's 3D scenes are larger than 1920x1080 above 1x: fitted.
      {L"DLSS5_FIT_LARGE", L"1"},
      {L"DLSS5_NETWORK_HEIGHT", std::wstring(height.begin(), height.end())},
      {L"DLSS5_MULTI_PASS", std::to_wstring(passes)},
      {L"DLSS5_MULTI_PASS_SKIN_PROTECT",
       cvars::dao_neural_skin_protect ? L"1" : L"0"},
      {L"DLSS5_STYLE", std::to_wstring(style)},
      // The settings are these: its config files aren't watched (a stat a
      // second, in the frame's submission).
      {L"DLSS5_HOT_RELOAD", L"0"},
  };
  for (const auto& [name, value] : options) {
    SetEnvironmentVariableW(name, value.c_str());
  }
  XELOGI(
      "Neural rendering: network {}, {} pass(es){}, style {}", height,
      passes,
      passes > 1 && cvars::dao_neural_skin_protect ? " (skin protected)" : "",
      style);
}

// The runtime's error text.
std::string LastError(const LmxxfNrApi& api) {
  char text[512] = {};
  if (api.GetLastError) {
    api.GetLastError(text, uint32_t(std::size(text)));
  }
  return text;
}
}  // namespace

NeuralRendering::~NeuralRendering() {
  if (session_) {
    // (The submitted frames are done: the upscaler waits for them first.)
    if (job_pending_) {
      api_.Retire(session_, job_.handle);
    }
    api_.Destroy(session_);
  }
}

bool NeuralRendering::Fail(const char* what) {
  XELOGE("Neural rendering: {} - {}; off from now on", what, LastError(api_));
  failed_ = true;
  return false;
}

bool NeuralRendering::Load() {
  std::error_code ec;
  // Absolute, as the runtime gets it: it makes it absolute itself, in a
  // MAX_PATH buffer, and opens its files by MAX_PATH names.
  std::filesystem::path weights = xe::to_path(cvars::dao_neural_weights);
  if (!weights.empty()) {
    weights = std::filesystem::absolute(weights, ec);
  }
  if (weights.empty() || ec ||
      (weights / kWeightsFile).native().size() >= MAX_PATH ||
      !std::filesystem::is_regular_file(weights / kWeightsFile, ec)) {
    XELOGE(
        "Neural rendering: no weights in \"{}\" (the folder of lmxxf's "
        "package with block0-ffn.f16); off",
        cvars::dao_neural_weights);
    failed_ = true;
    return false;
  }
  wchar_t exe[MAX_PATH];
  const DWORD length = GetModuleFileNameW(nullptr, exe, DWORD(std::size(exe)));
  if (!length || length == std::size(exe)) {
    failed_ = true;
    return false;
  }
  const std::filesystem::path folder =
      std::filesystem::path(exe).parent_path();
  // The installer's: the kernels compiled for this GPU (HIP/) and the shaders
  // the runtime compiles.
  const std::filesystem::path assets = folder / "neural";
  // And its settings: the runtime's folder for them and its logs is DLSS5-AMD
  // when they're there, else lmxxf's development folder on D:. (SHA256SUMS is
  // the installer's last: the kernels are all there.)
  if (!std::filesystem::is_regular_file(assets / "HIP" / "SHA256SUMS", ec) ||
      !std::filesystem::is_regular_file(
          folder / "DLSS5-AMD" / "default-config.txt", ec)) {
    XELOGE(
        "Neural rendering: not installed for this GPU (neural\\HIP or "
        "DLSS5-AMD\\default-config.txt is missing: install again); off");
    failed_ = true;
    return false;
  }
  // The runtime looks in these before the chosen folder (its FindWeightsDir).
  for (const std::filesystem::path& first :
       {assets, assets / "weights", folder}) {
    if (std::filesystem::is_regular_file(first / kWeightsFile, ec) ||
        std::filesystem::is_regular_file(first / kWeightsFileF32, ec)) {
      XELOGW(
          "Neural rendering: the weights in {} are used, not the chosen "
          "folder's (the runtime looks there first)",
          xe::path_to_utf8(first));
      break;
    }
  }
  SetRuntimeOptions();
  module_ = LoadLibraryExW((folder / "LmxxfNrRuntime.dll").c_str(), nullptr,
                           LOAD_WITH_ALTERED_SEARCH_PATH);
  auto get_api = module_ ? reinterpret_cast<decltype(&LmxxfNrGetApi)>(
                               GetProcAddress(module_, "LmxxfNrGetApi"))
                         : nullptr;
  api_.struct_size = sizeof(api_);
  if (!get_api || get_api(LMXXF_NR_ABI_VERSION, &api_) != LMXXF_NR_OK) {
    XELOGE("Neural rendering: LmxxfNrRuntime.dll is missing or unusable; off");
    failed_ = true;
    return false;
  }
  // The runtime looks for the weights there (its FindWeightsDir, at Create:
  // not one of the options it copies as it loads).
  SetEnvironmentVariableW(L"LMXXF_WEIGHTS_DIR", weights.c_str());
  D3D12CommandProcessor& cp = upscaler_.command_processor();
  const ui::d3d12::D3D12Provider& provider = cp.GetD3D12Provider();
  LmxxfNrCreateInfo create = {};
  create.struct_size = sizeof(create);
  create.device = provider.GetDevice();
  create.queue = provider.GetDirectQueue();
  const std::wstring assets_text = assets.wstring();
  create.assets_directory = assets_text.c_str();
  if (api_.Create(&create, &session_) != LMXXF_NR_OK) {
    session_ = nullptr;
    return Fail("can't start (are its kernels compiled for this GPU?)");
  }
  if (api_.PrepareSession(session_) != LMXXF_NR_OK) {
    return Fail("can't prepare its session");
  }
  XELOGI("Neural rendering: on, weights in {}", xe::path_to_utf8(weights));
  return true;
}

LmxxfNrFrameInfo NeuralRendering::FrameInfo(ID3D12Resource* color,
                                            uint32_t width, uint32_t height,
                                            float strength) {
  LmxxfNrFrameInfo info = {};
  info.struct_size = sizeof(info);
  // One game, one view: its history isn't shared with another session.
  info.session_id = 1;
  info.frame_id = ++frame_;
  info.color_width = width;
  info.color_height = height;
  info.color = color;
  info.color_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  info.flags = LMXXF_NR_FRAME_FLAG_STRENGTH;
  info.transfer_strength = strength;
  info.color_strength = strength;
  // The network at its own scale (the input fitted to it).
  info.model_scale = 1.0f;
  return info;
}

void NeuralRendering::Prepare(ID3D12Resource* color, uint32_t width,
                              uint32_t height) {
  if (failed_ || session_) {
    return;
  }
  const auto start = std::chrono::steady_clock::now();
  if (!Load()) {
    return;
  }
  // A frame prepared and cancelled: the network built for the size, its
  // weights uploaded and its shaders compiled. Nothing is recorded, so
  // cancelling it only releases the job. (Prepared at strength 1: the strength
  // is set per frame, in LmxxfNrFrameInfo, so the slider's doesn't matter.)
  const LmxxfNrFrameInfo info = FrameInfo(color, width, height, 1.0f);
  job_ = {};
  job_.struct_size = sizeof(job_);
  if (api_.PrepareFrame(session_, &info, &job_) != LMXXF_NR_OK ||
      !job_.private_output) {
    Fail("can't prepare a frame");
    return;
  }
  api_.CancelUnsubmitted(session_, job_.handle);
  job_ = {};
  prepared_size_[0] = width;
  prepared_size_[1] = height;
  XELOGI("Neural rendering: ready for {}x{} in {} ms", width, height,
         std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - start)
             .count());
}

ID3D12Resource* NeuralRendering::Run(ID3D12Resource* color, uint32_t width,
                                     uint32_t height, float strength,
                                     bool reset) {
  if (failed_ || (!session_ && !Load())) {
    return nullptr;
  }
  D3D12CommandProcessor& cp = upscaler_.command_processor();
  // The last frame's outputs were submitted (its swap ended its submission).
  if (job_pending_) {
    api_.Retire(session_, job_.handle);
    job_pending_ = false;
  }
  // The network runs between two submissions: it waits for the first, the
  // queue for it. Ending one mid-frame needs its pipelines created.
  // (A reset asked for on a frame it doesn't run on waits for the next.)
  reset_pending_ |= reset;
  if (!cp.CanEndSubmissionImmediately()) {
    return nullptr;
  }
  if (reset_pending_) {
    api_.ResetHistory(session_);
    reset_pending_ = false;
  }
  if (prepared_size_[0] &&
      (prepared_size_[0] != width || prepared_size_[1] != height)) {
    XELOGW(
        "Neural rendering: the scenes are {}x{}, not {}x{} as it was made "
        "ready for: it's built again now",
        width, height, prepared_size_[0], prepared_size_[1]);
  }
  prepared_size_[0] = 0;
  const LmxxfNrFrameInfo info = FrameInfo(color, width, height, strength);
  job_ = {};
  job_.struct_size = sizeof(job_);
  if (api_.PrepareFrame(session_, &info, &job_) != LMXXF_NR_OK ||
      !job_.private_output) {
    Fail("can't prepare a frame");
    return nullptr;
  }
  inputs_recorded_ = false;
  cp.GetDeferredCommandList().DaoCallback(ExecuteInputs, this);
  // The commands so far, then the network, then the rest of the frame.
  DaoFrameGuard::Get().BeforeNeural();
  const bool ended = cp.EndSubmission(false);
  if (!ended) {
    DaoFrameGuard::Get().NeuralNotRun();
  }
  if (!ended || !inputs_recorded_ ||
      api_.EnqueueHip(session_, job_.handle,
                      cp.GetD3D12Provider().GetDirectQueue()) != LMXXF_NR_OK) {
    api_.CancelUnsubmitted(session_, job_.handle);
    if (ended) {
      cp.BeginSubmission(false);
    }
    Fail("can't run the network");
    return nullptr;
  }
  if (!cp.BeginSubmission(false)) {
    // The network was queued: this only retires the job, and destroying the
    // session waits for its work.
    api_.CancelUnsubmitted(session_, job_.handle);
    Fail("can't continue the frame");
    return nullptr;
  }
  cp.GetDeferredCommandList().DaoCallback(ExecuteOutputs, this);
  // Its output's commands set their own descriptor heap and root signature.
  upscaler_.InvalidateCommandProcessorState();
  job_pending_ = true;
  return static_cast<ID3D12Resource*>(job_.private_output);
}

void NeuralRendering::ExecuteInputs(void* context,
                                    ID3D12GraphicsCommandList* command_list) {
  NeuralRendering& self = *static_cast<NeuralRendering*>(context);
  self.inputs_recorded_ =
      self.api_.RecordInputs(self.session_, self.job_.handle, command_list) ==
      LMXXF_NR_OK;
  if (!self.inputs_recorded_) {
    XELOGE("Neural rendering: can't record its inputs - {}",
           LastError(self.api_));
  }
}

void NeuralRendering::ExecuteOutputs(void* context,
                                     ID3D12GraphicsCommandList* command_list) {
  NeuralRendering& self = *static_cast<NeuralRendering*>(context);
  if (self.api_.RecordOutputs(self.session_, self.job_.handle,
                              command_list) != LMXXF_NR_OK) {
    // (The upscaler reads the output as it was.)
    self.Fail("can't record its output");
  }
}

}  // namespace d3d12
}  // namespace gpu
}  // namespace xe
