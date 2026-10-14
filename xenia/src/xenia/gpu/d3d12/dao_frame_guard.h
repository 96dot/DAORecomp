// DAO: the frame rate guard. Neural rendering costs much GPU time (7 to 9.5 ms
// a frame on an RX 9070 XT), and it's a cost that can go at once: when the GPU
// can't hold the game's frame rate with it, its strength fades out (0 skips
// the network), and when the GPU has room for it again, it fades back in
// (dao_neural_frame_rate_guard).
//
// The GPU's time a frame is measured with two timestamps a submission (at its
// start and end), resolved as the frame's last submission ends and read a few
// frames later, once that submission has completed: the time the submissions
// ran, plus the network's (between the two submissions around it, as it runs
// on its own between them, along with anything else queued between them,
// such as a present), without the time the GPU waited for the CPU. It
// measures only frames the upscaler ran on (that asked the guard), while
// neural rendering would run, the guard is on and the frame rate is limited.
// All on the command processor's thread.
#pragma once

#include <chrono>
#include <cstdint>

#include "xenia/ui/d3d12/d3d12_api.h"

namespace xe::gpu::d3d12 {

class D3D12CommandProcessor;

class DaoFrameGuard {
 public:
  static DaoFrameGuard& Get();

  // The scale of neural rendering's strength this frame, 0 to 1, moving toward
  // what the guard wants; `wanted`: whether neural rendering would run at all
  // (the guard measures only then). Once a frame, before the network runs.
  float NeuralScale(bool wanted);
  // The network runs before the next submission: the GPU's time until that
  // submission starts is its.
  void BeforeNeural() { neural_next_ = true; }
  // It doesn't after all: the submission before it couldn't end.
  void NeuralNotRun() { neural_next_ = false; }

  // From the command processor: a submission's command list has begun, or is
  // about to be executed (`open`: there is one; `closes_frame`: the frame's
  // last, its swap).
  void SubmissionBegan(D3D12CommandProcessor& cp);
  void SubmissionEnding(D3D12CommandProcessor& cp, bool open,
                        bool closes_frame);

 private:
  using Clock = std::chrono::steady_clock;

  // Frames in flight (more than the command processor's kQueueFrames, so a
  // slot's frame has completed when it comes round again) and timestamps a
  // frame: two a submission (a frame with more isn't measured).
  static constexpr uint32_t kSlots = 4;
  static constexpr uint32_t kMaxMarks = 64;

  enum class MarkKind : uint8_t { kBegin, kEnd, kEndBeforeNeural };

  struct Slot {
    uint64_t submission = 0;  // the frame's last; 0: nothing to read
    uint32_t marks = 0;
    double interval_ms = 0.0;  // since the frame before's swap
    MarkKind kinds[kMaxMarks];
  };

  bool Initialize(D3D12CommandProcessor& cp);
  void Mark(D3D12CommandProcessor& cp, MarkKind kind);
  // Reads the frames whose submissions have completed, oldest first.
  void ReadCompleted(D3D12CommandProcessor& cp);
  // A measured frame: its GPU time and the network's part of it, in ms, and
  // the time since the frame before, at `rate` fps.
  void Decide(int32_t rate, double gpu_ms, double neural_ms, bool neural_ran,
              double interval_ms);

  bool failed_ = false;
  Microsoft::WRL::ComPtr<ID3D12QueryHeap> heap_;
  Microsoft::WRL::ComPtr<ID3D12Resource> readback_;
  double ticks_per_ms_ = 0.0;

  // Measuring (decided as the guard was last asked; not once a frame goes by
  // without asking, the upscaler not running), asked this frame (a frame not
  // asked isn't measured: a loading screen, a menu without a 3D scene), the
  // frame's first submission begun, whether every submission of the frame so
  // far was measured, and the network running before the next one.
  bool measuring_ = false;
  bool asked_ = false;
  // Whether a frame with too many submissions was logged.
  bool too_many_logged_ = false;
  bool frame_started_ = false;
  bool frame_whole_ = false;
  bool neural_next_ = false;
  Slot slots_[kSlots];
  uint32_t slot_ = 0;
  Clock::time_point last_frame_end_;

  // The decision: the strength's scale and where it's going.
  bool eased_off_ = false;
  float scale_ = 1.0f;
  Clock::time_point last_scale_;
  // Averages of the GPU's time a frame with the network and without it, of
  // the network's alone and of the frames' interval.
  double gpu_with_ms_ = 0.0;
  double gpu_without_ms_ = 0.0;
  double neural_ms_ = 0.0;
  double interval_ms_ = 0.0;
  // How long it's been over the budget (eases off past kOverFor), and with
  // room for the network (comes back past room_hold_s_: kRoomFor, doubled on
  // each relapse), in s.
  double over_s_ = 0.0;
  double room_s_ = 0.0;
  double room_hold_s_ = 0.0;
  // At easing off: the GPU's time with the network less the network's, and
  // (once settled, kSettle s later) its time without it.
  double settle_s_ = 0.0;
  double busy_at_ease_ms_ = 0.0;
  double without_at_ease_ms_ = 0.0;
  Clock::time_point came_back_;
};

}  // namespace xe::gpu::d3d12
