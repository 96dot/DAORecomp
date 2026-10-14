// DAO: the frame rate guard - see dao_frame_guard.h.
#include "xenia/gpu/d3d12/dao_frame_guard.h"

#include <algorithm>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/dao_live_settings.h"
#include "xenia/gpu/d3d12/d3d12_command_processor.h"

DEFINE_bool(dao_neural_frame_rate_guard, true,
            "Neural rendering: its strength fades out while the GPU can't "
            "hold the frame rate with it, and back in when there's room for "
            "it again (not with the frame rate unlimited).",
            "GPU");

namespace xe::gpu::d3d12 {

namespace {
// A frame's weight in the averages.
constexpr double kAverage = 0.1;
// Over budget: the frames are this much slower than the rate, and the GPU is
// what holds them back, busy for more than this part of their time (it isn't
// when the display's refresh does, the frame rate above it with vsync, or the
// CPU; low latency leaves the GPU short gaps even when it is), for kOverFor s
// (less the time they weren't, as the averages waver about the limits).
constexpr double kLate = 1.03;
constexpr double kGpuBound = 0.9;
constexpr double kOverFor = 0.5;
// Room for the network: the GPU's time a frame with it, as predicted (see
// Decide), under this part of a frame, for kRoomFor s; for twice as long (up
// to kRoomForMax) each time it eases off again within kRelapse s of coming
// back. The prediction starts kSettle s after easing off.
constexpr double kRoom = 0.85;
constexpr double kSettle = 1.0;
constexpr double kRoomFor = 3.0;
constexpr double kRoomForMax = 60.0;
constexpr double kRelapse = 20.0;
// The strength's fade, out or in, in s.
constexpr double kFade = 0.5;
// A frame's time at most, in s (longer: a stall, a loading screen). So a
// frame's step of the fade is at most kMaxFrame / kFade (0.2), under the
// change that resets the upscaler's history (kNeuralStrengthJump, 0.25).
constexpr double kMaxFrame = 0.1;

double Average(double average, double value) {
  return average > 0.0 ? average + (value - average) * kAverage : value;
}

double Seconds(std::chrono::steady_clock::duration duration) {
  return std::chrono::duration<double>(duration).count();
}
}  // namespace

DaoFrameGuard& DaoFrameGuard::Get() {
  static DaoFrameGuard guard;
  return guard;
}

float DaoFrameGuard::NeuralScale(bool wanted) {
  const Clock::time_point now = Clock::now();
  const double dt =
      last_scale_ == Clock::time_point{}
          ? 0.0
          : std::clamp(Seconds(now - last_scale_), 0.0, kMaxFrame);
  last_scale_ = now;
  asked_ = true;
  const bool on = dao_live::Load(dao_live::values.neural_frame_rate_guard) &&
                  dao_live::Load(dao_live::values.frame_rate) > 0;
  const bool was_measuring = measuring_;
  measuring_ = on && wanted && !failed_;
  if (measuring_ && !was_measuring) {
    // Measuring again (after F6, strength 0, the guard off or no 3D scene):
    // the averages start again, of the scene now. (Eased off, the prediction
    // keeps its times at easing off: it follows the change of scene.)
    over_s_ = 0.0;
    room_s_ = 0.0;
    interval_ms_ = 0.0;
    gpu_with_ms_ = 0.0;
    gpu_without_ms_ = 0.0;
  }
  if (!on && eased_off_) {
    eased_off_ = false;
    XELOGI("Frame rate guard: off, neural rendering back");
  }
  const float target = eased_off_ ? 0.0f : 1.0f;
  const float step = float(dt / kFade);
  scale_ = scale_ < target ? std::min(scale_ + step, target)
                           : std::max(scale_ - step, target);
  return scale_;
}

bool DaoFrameGuard::Initialize(D3D12CommandProcessor& cp) {
  if (heap_ || failed_) {
    return !failed_;
  }
  const ui::d3d12::D3D12Provider& provider = cp.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  D3D12_QUERY_HEAP_DESC heap_desc = {};
  heap_desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
  heap_desc.Count = kSlots * kMaxMarks;
  D3D12_HEAP_PROPERTIES heap_properties = {};
  heap_properties.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC buffer_desc = {};
  buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer_desc.Width = kSlots * kMaxMarks * sizeof(uint64_t);
  buffer_desc.Height = 1;
  buffer_desc.DepthOrArraySize = 1;
  buffer_desc.MipLevels = 1;
  buffer_desc.SampleDesc.Count = 1;
  buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  UINT64 frequency = 0;
  if (FAILED(provider.GetDirectQueue()->GetTimestampFrequency(&frequency)) ||
      !frequency ||
      FAILED(device->CreateQueryHeap(&heap_desc, IID_PPV_ARGS(&heap_))) ||
      FAILED(device->CreateCommittedResource(
          &heap_properties, D3D12_HEAP_FLAG_NONE, &buffer_desc,
          D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
          IID_PPV_ARGS(&readback_)))) {
    XELOGW("Frame rate guard: can't measure the GPU's time; off");
    heap_.Reset();
    readback_.Reset();
    failed_ = true;
    measuring_ = false;
    return false;
  }
  ticks_per_ms_ = double(frequency) / 1000.0;
  return true;
}

void DaoFrameGuard::Mark(D3D12CommandProcessor& cp, MarkKind kind) {
  Slot& slot = slots_[slot_];
  if (slot.marks >= kMaxMarks) {
    frame_whole_ = false;
    if (!too_many_logged_) {
      too_many_logged_ = true;
      XELOGW(
          "Frame rate guard: a frame with more than {} submissions isn't "
          "measured",
          kMaxMarks / 2);
    }
    return;
  }
  cp.GetDeferredCommandList().D3DEndQuery(
      heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot_ * kMaxMarks + slot.marks);
  slot.kinds[slot.marks++] = kind;
}

void DaoFrameGuard::SubmissionBegan(D3D12CommandProcessor& cp) {
  if (!frame_started_) {
    frame_started_ = true;
    ReadCompleted(cp);
    // A frame still not read (its submission still running, as at a stall)
    // gives its slot up.
    Slot& slot = slots_[slot_];
    slot.submission = 0;
    slot.marks = 0;
    frame_whole_ = measuring_ && Initialize(cp);
  }
  if (frame_whole_) {
    Mark(cp, MarkKind::kBegin);
  }
}

void DaoFrameGuard::SubmissionEnding(D3D12CommandProcessor& cp, bool open,
                                     bool closes_frame) {
  if (frame_whole_ && open) {
    Mark(cp, neural_next_ ? MarkKind::kEndBeforeNeural : MarkKind::kEnd);
  }
  neural_next_ = false;
  if (!closes_frame) {
    return;
  }
  frame_started_ = false;
  const bool asked = asked_;
  measuring_ = measuring_ && asked;
  asked_ = false;
  const Clock::time_point now = Clock::now();
  const double interval_ms =
      last_frame_end_ == Clock::time_point{}
          ? 0.0
          : std::min(Seconds(now - last_frame_end_), kMaxFrame) * 1000.0;
  last_frame_end_ = now;
  // (Without a submission open, nothing to resolve the timestamps in.)
  Slot& slot = slots_[slot_];
  if (!frame_whole_ || !open || !asked || !slot.marks) {
    slot.marks = 0;
    return;
  }
  cp.GetDeferredCommandList().D3DResolveQueryData(
      heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot_ * kMaxMarks, slot.marks,
      readback_.Get(), slot_ * kMaxMarks * sizeof(uint64_t));
  slot.submission = cp.GetCurrentSubmission();
  slot.interval_ms = interval_ms;
  slot_ = (slot_ + 1) % kSlots;
}

void DaoFrameGuard::ReadCompleted(D3D12CommandProcessor& cp) {
  const uint64_t completed = cp.GetCompletedSubmission();
  // Oldest first: the one slot_ is about to be used for.
  for (uint32_t i = 0; i < kSlots; ++i) {
    const uint32_t index = (slot_ + i) % kSlots;
    Slot& slot = slots_[index];
    if (!slot.submission || slot.submission > completed) {
      continue;
    }
    slot.submission = 0;
    const D3D12_RANGE range = {
        index * kMaxMarks * sizeof(uint64_t),
        (index * kMaxMarks + slot.marks) * sizeof(uint64_t)};
    void* mapping;
    if (FAILED(readback_->Map(0, &range, &mapping))) {
      continue;
    }
    const uint64_t* ticks =
        static_cast<const uint64_t*>(mapping) + index * kMaxMarks;
    // The submissions' times, and the network's between the two around it.
    // (Timestamps going back, a GPU clock reset, count as no time.)
    uint64_t busy = 0;
    uint64_t neural = 0;
    bool neural_ran = false;
    uint64_t begin = 0;
    uint64_t end_before_neural = 0;
    for (uint32_t mark = 0; mark < slot.marks; ++mark) {
      const uint64_t tick = ticks[mark];
      if (slot.kinds[mark] == MarkKind::kBegin) {
        if (end_before_neural) {
          neural += tick > end_before_neural ? tick - end_before_neural : 0;
          neural_ran = true;
          end_before_neural = 0;
        }
        begin = tick;
        continue;
      }
      busy += tick > begin ? tick - begin : 0;
      if (slot.kinds[mark] == MarkKind::kEndBeforeNeural) {
        end_before_neural = tick;
      }
    }
    const D3D12_RANGE written = {0, 0};
    readback_->Unmap(0, &written);
    // (Not after measuring stopped: a frame still in flight then.)
    const int32_t rate = dao_live::Load(dao_live::values.frame_rate);
    if (measuring_ && rate > 0) {
      Decide(rate, double(busy + neural) / ticks_per_ms_,
             double(neural) / ticks_per_ms_, neural_ran, slot.interval_ms);
    }
  }
}

void DaoFrameGuard::Decide(int32_t rate, double gpu_ms, double neural_ms,
                           bool neural_ran, double interval_ms) {
  if (interval_ms <= 0.0) {
    return;
  }
  const double budget_ms = 1000.0 / rate;
  interval_ms_ = Average(interval_ms_, interval_ms);
  if (neural_ran) {
    gpu_with_ms_ = Average(gpu_with_ms_, gpu_ms);
    neural_ms_ = Average(neural_ms_, neural_ms);
  } else {
    gpu_without_ms_ = Average(gpu_without_ms_, gpu_ms);
  }
  const double dt = interval_ms / 1000.0;
  const Clock::time_point now = Clock::now();
  if (!eased_off_) {
    const bool over = neural_ran && interval_ms_ > kLate * budget_ms &&
                      gpu_with_ms_ > kGpuBound * interval_ms_;
    over_s_ = over ? over_s_ + dt : std::max(over_s_ - dt, 0.0);
    if (over_s_ < kOverFor) {
      return;
    }
    eased_off_ = true;
    over_s_ = 0.0;
    room_s_ = 0.0;
    settle_s_ = 0.0;
    gpu_without_ms_ = 0.0;
    busy_at_ease_ms_ = std::max(gpu_with_ms_ - neural_ms_, 0.0);
    room_hold_s_ = came_back_ != Clock::time_point{} &&
                           Seconds(now - came_back_) < kRelapse
                       ? std::min(room_hold_s_ * 2.0, kRoomForMax)
                       : kRoomFor;
    XELOGI(
        "Frame rate guard: neural rendering eases off (the GPU {:.1f} ms a "
        "frame, {:.1f} of it the network's, for {} fps: {:.1f} ms)",
        gpu_with_ms_, neural_ms_, rate, budget_ms);
    return;
  }
  // Without the network the GPU has time to spare and slows its clock, so the
  // rest of the frame takes it longer than it did with the network. The
  // prediction: the rest of the frame's time with the network at easing off
  // (busy_at_ease_ms_), scaled by how the time without it has changed since
  // it settled (gpu_without_ms_ / without_at_ease_ms_, both at the slower
  // clock), plus the network's time.
  if (neural_ran || gpu_without_ms_ <= 0.0) {
    room_s_ = 0.0;
    return;
  }
  if (settle_s_ < kSettle) {
    settle_s_ += dt;
    without_at_ease_ms_ = gpu_without_ms_;
    return;
  }
  const double predicted_ms =
      busy_at_ease_ms_ * gpu_without_ms_ / without_at_ease_ms_ + neural_ms_;
  const bool room = predicted_ms < kRoom * budget_ms;
  room_s_ = room ? room_s_ + dt : 0.0;
  if (room_s_ < room_hold_s_) {
    return;
  }
  eased_off_ = false;
  room_s_ = 0.0;
  gpu_with_ms_ = 0.0;
  came_back_ = now;
  XELOGI(
      "Frame rate guard: neural rendering comes back (the GPU about {:.1f} ms "
      "a frame with it, for {} fps: {:.1f} ms)",
      predicted_ms, rate, budget_ms);
}

}  // namespace xe::gpu::d3d12
