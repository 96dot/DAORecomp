// DAO: frame generation's presentation (the frames: gpu/d3d12/
// dao_upscale_fg.cc). A guest output with a generated frame is shown as two,
// both painted by the pacer thread: the generated frame when it arrives, the
// real one half a frame later. The guest output thread only hands them over,
// never waiting for room in the swap chain itself (DaoWaitForFrameLatency):
// with vsync, a display that can't show twice the frame rate holds back only
// the pacer. When it falls behind often, only real frames are shown for a
// while, painted by the guest output thread as without frame generation (and
// the GPU thread doesn't make generated ones meanwhile).
#include "xenia/ui/presenter.h"

#include <system_error>

#include "xenia/base/logging.h"
#include "xenia/base/platform_win.h"

namespace xe {
namespace ui {

namespace {

// New guest outputs further apart than this are a pause (a load, a menu
// without a 3D scene): not counted in the interval.
constexpr double kMaxFrameIntervalMs = 100.0;
// The interval's smoothing: how much of each new one is taken.
constexpr double kFrameIntervalSmoothing = 0.125;
// Falling behind: how much of each guest output is taken into how often it
// happens, and how often is too often. Now and then is a frame that came
// early or a paint that waited for the display a little longer, not a display
// that can't keep up.
constexpr double kBehindSmoothing = 1.0 / 16.0;
constexpr double kBehindTooOften = 0.25;
// While the UI thread paints (presenter UI shown), how long only real frames
// are shown before looking again.
constexpr auto kRealOnlyWhileUiPaints = std::chrono::seconds(1);
// The pacer sleeps until this long before the real frame is due, then spins:
// sleeps aren't that precise.
constexpr auto kSpin = std::chrono::microseconds(500);

// What the pacer thread is painting: a Presenter::DaoPacerStage (protected
// there), and of which guest output.
thread_local int t_pacer_stage = 0;
thread_local uint64_t t_pacer_serial = 0;

}  // namespace

Presenter::DaoPacerStage Presenter::DaoPacerPainting() {
  return DaoPacerStage(t_pacer_stage);
}

bool Presenter::DaoKeepAcquired(const GuestOutputProperties& acquired) {
  // Only if it's the guest output whose generated frame was painted: if that
  // paint didn't take it (no room in the swap chain in time), the newer one.
  return DaoPacerStage(t_pacer_stage) == DaoPacerStage::kReal &&
         acquired.dao_serial == t_pacer_serial;
}

bool Presenter::DaoGeneratedFramesShown() const {
  return std::chrono::steady_clock::now().time_since_epoch().count() >=
         dao_real_only_until_.load(std::memory_order_relaxed);
}

void Presenter::DaoResetFramePacing() {
  std::lock_guard<std::mutex> lock(dao_pacer_mutex_);
  dao_real_only_until_.store(0, std::memory_order_relaxed);
  dao_real_only_for_ = kDaoRealOnlyMin;
  dao_pacer_behind_ = 0.0;
}

bool Presenter::DaoQueueFramePaints(const GuestOutputProperties& properties) {
  const auto now = std::chrono::steady_clock::now();
  std::unique_lock<std::mutex> lock(dao_pacer_mutex_);
  if (dao_last_new_frame_ != std::chrono::steady_clock::time_point()) {
    const double interval_ms =
        std::chrono::duration<double, std::milli>(now - dao_last_new_frame_)
            .count();
    if (interval_ms < kMaxFrameIntervalMs) {
      dao_frame_interval_ms_ +=
          (interval_ms - dao_frame_interval_ms_) * kFrameIntervalSmoothing;
    }
  }
  // Behind: the previous guest output's generated frame, or its real one
  // when it was due, not even started (a paint waiting for the display's
  // refresh is not behind: with vsync, that's every paint while the swap
  // chain is full; a real frame not due yet is only dropped: this guest
  // output came early).
  const bool behind =
      dao_pacer_stage_ == DaoPacerStage::kGenerated ||
      (dao_pacer_stage_ == DaoPacerStage::kReal &&
       now - dao_pacer_arrival_ >=
           std::chrono::duration<double, std::milli>(dao_frame_interval_ms_ *
                                                     0.5));
  dao_last_new_frame_ = now;
  const bool queued_last = dao_pacer_queued_last_;
  dao_pacer_queued_last_ = false;
  dao_pacer_stage_ = DaoPacerStage::kNone;
  if (!properties.dao_generated || dao_pacer_shutdown_) {
    return false;
  }
  if (queued_last) {
    dao_pacer_behind_ +=
        ((behind ? 1.0 : 0.0) - dao_pacer_behind_) * kBehindSmoothing;
    if (dao_pacer_behind_ > kBehindTooOften) {
      if (!dao_pacer_behind_logged_) {
        dao_pacer_behind_logged_ = true;
        XELOGI(
            "Frame generation: the display can't show twice the frame rate, "
            "showing only real frames for now");
      }
      dao_real_only_until_.store(
          (now + dao_real_only_for_).time_since_epoch().count(),
          std::memory_order_relaxed);
      dao_real_only_for_ = std::min<std::chrono::steady_clock::duration>(
          dao_real_only_for_ * 2, kDaoRealOnlyMax);
      dao_pacer_behind_ = 0.0;
      return false;
    }
    if (now.time_since_epoch().count() -
            dao_real_only_until_.load(std::memory_order_relaxed) >=
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            kDaoRealOnlyMax)
            .count()) {
      // Kept up long enough since: it's not the display.
      dao_real_only_for_ = kDaoRealOnlyMin;
    }
  } else if (now.time_since_epoch().count() <
             dao_real_only_until_.load(std::memory_order_relaxed)) {
    return false;
  }
  if (!dao_pacer_thread_.joinable()) {
    dao_pacer_wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    try {
      dao_pacer_thread_ = std::thread(&Presenter::DaoPacerThread, this);
    } catch (const std::system_error& error) {
      XELOGE("Frame generation: no pacer thread ({}), showing real frames",
             error.what());
      dao_pacer_shutdown_ = true;
      return false;
    }
  }
  dao_pacer_queued_last_ = true;
  dao_pacer_serial_ = properties.dao_serial;
  dao_pacer_stage_ = DaoPacerStage::kGenerated;
  dao_pacer_arrival_ = now;
  lock.unlock();
  dao_pacer_cv_.notify_one();
  // (Closed by DaoStopPacer, which the presenter's destruction calls, never
  // while the guest output thread refreshes.)
  if (dao_pacer_wake_) {
    SetEvent(dao_pacer_wake_);
  }
  return true;
}

void Presenter::DaoStopPacer() {
  {
    std::lock_guard<std::mutex> lock(dao_pacer_mutex_);
    dao_pacer_shutdown_ = true;
  }
  dao_pacer_cv_.notify_one();
  if (dao_pacer_wake_) {
    SetEvent(dao_pacer_wake_);
  }
  if (dao_pacer_thread_.joinable()) {
    dao_pacer_thread_.join();
  }
  if (dao_pacer_wake_) {
    CloseHandle(dao_pacer_wake_);
    dao_pacer_wake_ = nullptr;
  }
}

void Presenter::DaoPacerThread() {
  // Above the game's threads: a late present is a visible stutter.
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
  HANDLE timer = CreateWaitableTimerExW(
      nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
      TIMER_ALL_ACCESS);
  // (Made before the thread, closed after it.)
  HANDLE wake = dao_pacer_wake_;
  for (;;) {
    DaoPacerStage stage;
    uint64_t serial;
    std::chrono::steady_clock::time_point due;
    {
      std::unique_lock<std::mutex> lock(dao_pacer_mutex_);
      dao_pacer_cv_.wait(lock, [this] {
        return dao_pacer_shutdown_ || dao_pacer_stage_ != DaoPacerStage::kNone;
      });
      if (dao_pacer_shutdown_) {
        break;
      }
      stage = dao_pacer_stage_;
      serial = dao_pacer_serial_;
      due = dao_pacer_arrival_;
      if (stage == DaoPacerStage::kReal) {
        due += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double, std::milli>(dao_frame_interval_ms_ *
                                                      0.5));
      }
    }

    // Until it's due: a sleep, then a spin. A newer guest output handed over
    // meanwhile ends the sleep: its generated frame is due now.
    bool superseded = false;
    for (;;) {
      const auto remaining = due - std::chrono::steady_clock::now();
      if (remaining <= std::chrono::steady_clock::duration::zero()) {
        break;
      }
      if (remaining > kSpin) {
        // In 100 ns units, negative: relative.
        LARGE_INTEGER sleep;
        sleep.QuadPart = -std::max<LONGLONG>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(remaining -
                                                                 kSpin)
                    .count() /
                100,
            1);
        if (timer && SetWaitableTimer(timer, &sleep, 0, nullptr, nullptr,
                                      FALSE)) {
          const HANDLE handles[] = {timer, wake};
          WaitForMultipleObjects(wake ? 2 : 1, handles, FALSE, INFINITE);
        } else if (wake) {
          WaitForSingleObject(wake, 1);
        } else {
          Sleep(1);
        }
        std::lock_guard<std::mutex> lock(dao_pacer_mutex_);
        if (dao_pacer_shutdown_ || dao_pacer_stage_ != stage ||
            dao_pacer_serial_ != serial) {
          superseded = true;
          break;
        }
      } else {
        YieldProcessor();
      }
    }
    if (superseded) {
      continue;
    }

    PaintResult paint_result = PaintResult::kNotPresented;
    {
      std::lock_guard<std::mutex> paint_mode_lock(paint_mode_mutex_);
      {
        std::lock_guard<std::mutex> lock(dao_pacer_mutex_);
        if (dao_pacer_shutdown_) {
          break;
        }
        // Behind a newer guest output meanwhile.
        if (dao_pacer_stage_ != stage || dao_pacer_serial_ != serial) {
          continue;
        }
        dao_pacer_stage_ = stage == DaoPacerStage::kGenerated
                               ? DaoPacerStage::kReal
                               : DaoPacerStage::kNone;
        if (paint_mode_ == PaintMode::kUIThreadOnRequest) {
          // The UI thread paints the real frames only: none generated
          // meanwhile.
          dao_real_only_until_.store(
              (std::chrono::steady_clock::now() + kRealOnlyWhileUiPaints)
                  .time_since_epoch()
                  .count(),
              std::memory_order_relaxed);
        }
      }
      // As RefreshGuestOutput paints.
      t_pacer_stage = int(stage);
      t_pacer_serial = serial;
      switch (paint_mode_) {
        case PaintMode::kNone:
          break;
        case PaintMode::kUIThreadOnRequest:
          if (stage == DaoPacerStage::kGenerated) {
            RequestPaintOrConnectionRecoveryViaWindow(true);
          }
          break;
        case PaintMode::kGuestOutputThreadImmediately:
          if (surface_paint_connection_state_ ==
              SurfacePaintConnectionState::kConnectedPaintable) {
            paint_result = PaintAndPresent(false);
            if (surface_paint_connection_state_ ==
                SurfacePaintConnectionState::kConnectedOutdated) {
              RequestPaintOrConnectionRecoveryViaWindow(true);
            }
          }
          break;
      }
      t_pacer_stage = int(DaoPacerStage::kNone);
    }
    if (host_gpu_loss_callback_) {
      if (paint_result == PaintResult::kGpuLostResponsible) {
        host_gpu_loss_callback_(true, false);
      } else if (paint_result == PaintResult::kGpuLostExternally) {
        host_gpu_loss_callback_(false, false);
      }
    }
  }
  if (timer) {
    CloseHandle(timer);
  }
}

}  // namespace ui
}  // namespace xe
