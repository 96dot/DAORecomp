// DAO: GPU time of each part of a frame, for finding what to optimize.
// DAO_GPU_TIMING=1 writes GPU timestamps at the first draw of a frame, at every
// resolve and around the swap's upscaling, and every 300 frames logs the
// average time between them to xenia.log with the "GT" prefix. A section is
// named after the event ending it (a resolve: its source and destination), so
// "resolve ..." is the time of the draws before that resolve (and of the
// previous resolve).
#pragma once

#include <algorithm>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

#include "xenia/base/logging.h"
#include "xenia/gpu/d3d12/d3d12_command_processor.h"
#include "xenia/gpu/register_file.h"
#include "xenia/gpu/registers.h"
#include "xenia/gpu/xenos.h"

namespace xe::gpu::d3d12::dao_timing {

class GpuTiming {
 public:
  static bool Enabled() {
    static const bool enabled = [] {
      const char* env = std::getenv("DAO_GPU_TIMING");
      return env && std::atoi(env) != 0;
    }();
    return enabled;
  }

  static GpuTiming& Get() {
    static GpuTiming timing;
    return timing;
  }

  // Called after BeginSubmission for every draw.
  void OnDraw(D3D12CommandProcessor& cp) {
    if (!frame_marks_) {
      Mark(cp, "frame start");
    }
    ++draws_;
  }

  void OnResolve(D3D12CommandProcessor& cp, const RegisterFile& regs) {
    const auto control = regs.Get<reg::RB_COPY_CONTROL>();
    const auto dest = regs.Get<reg::RB_COPY_DEST_INFO>();
    std::string source = "depth";
    if (control.copy_src_select < 4) {
      const auto info = regs.Get<reg::RB_COLOR_INFO>(
          reg::RB_COLOR_INFO::rt_register_indices[control.copy_src_select]);
      source = fmt::format(
          "RT{} {} base {}", uint32_t(control.copy_src_select),
          xenos::GetColorRenderTargetFormatName(info.color_format),
          info.color_base | (info.color_base_bit_11 << 11));
    }
    Mark(cp, fmt::format("resolve {} msaa {} -> {:08X} fmt {}{}", source,
                         uint32_t(regs.Get<reg::RB_SURFACE_INFO>().msaa_samples),
                         regs[XE_GPU_REG_RB_COPY_DEST_BASE],
                         uint32_t(dest.copy_dest_format),
                         control.color_clear_enable || control.depth_clear_enable
                             ? " +clear"
                             : ""));
  }

  void Mark(D3D12CommandProcessor& cp, std::string label) {
    if (!Initialize(cp)) {
      return;
    }
    if (!frame_marks_) {
      ReadSlot(cp, slot_);
    }
    if (frame_marks_ >= kMaxMarks) {
      return;
    }
    cp.GetDeferredCommandList().D3DEndQuery(
        heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot_ * kMaxMarks + frame_marks_);
    Slot& slot = slots_[slot_];
    slot.labels.resize(frame_marks_ + 1);
    slot.draws.resize(frame_marks_ + 1);
    slot.labels[frame_marks_] = std::move(label);
    slot.draws[frame_marks_] = draws_;
    draws_ = 0;
    ++frame_marks_;
  }

  // Called in the swap's submission before it ends.
  void EndFrame(D3D12CommandProcessor& cp) {
    if (!heap_ || !frame_marks_) {
      return;
    }
    Mark(cp, "swap end");
    cp.GetDeferredCommandList().D3DResolveQueryData(
        heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot_ * kMaxMarks,
        frame_marks_, readback_.Get(), slot_ * kMaxMarks * sizeof(uint64_t));
    Slot& slot = slots_[slot_];
    slot.submission = cp.GetCurrentSubmission();
    slot.marks = frame_marks_;
    frame_marks_ = 0;
    draws_ = 0;
    slot_ = (slot_ + 1) % kSlots;
  }

 private:
  static constexpr uint32_t kSlots = 4;
  static constexpr uint32_t kMaxMarks = 512;
  static constexpr uint32_t kLogInterval = 300;

  struct Slot {
    uint64_t submission = 0;
    uint32_t marks = 0;
    std::vector<std::string> labels;
    std::vector<uint32_t> draws;
  };

  struct Section {
    double milliseconds = 0.0;
    double draws = 0.0;
    double position = 0.0;
    uint32_t count = 0;
  };

  bool Initialize(D3D12CommandProcessor& cp) {
    if (heap_) {
      return true;
    }
    if (failed_) {
      return false;
    }
    failed_ = true;
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
      XELOGE("GT: failed to create the timestamp query heap");
      heap_.Reset();
      return false;
    }
    ticks_per_ms_ = double(frequency) / 1000.0;
    failed_ = false;
    XELOGI("GT: GPU timing enabled");
    return true;
  }

  // Accumulates the timestamps of a frame resolved earlier into `index`.
  void ReadSlot(D3D12CommandProcessor& cp, uint32_t index) {
    Slot& slot = slots_[index];
    if (!slot.marks) {
      return;
    }
    cp.CheckSubmissionCompletion(slot.submission);
    const D3D12_RANGE range = {index * kMaxMarks * sizeof(uint64_t),
                               (index * kMaxMarks + slot.marks) *
                                   sizeof(uint64_t)};
    void* mapping;
    if (SUCCEEDED(readback_->Map(0, &range, &mapping))) {
      const uint64_t* ticks =
          reinterpret_cast<const uint64_t*>(mapping) + index * kMaxMarks;
      std::unordered_map<std::string, uint32_t> occurrences;
      for (uint32_t i = 1; i < slot.marks; ++i) {
        std::string label = slot.labels[i];
        if (const uint32_t n = ++occurrences[label]; n > 1) {
          label += fmt::format(" #{}", n);
        }
        Section& section = sections_[label];
        section.milliseconds += double(ticks[i] - ticks[i - 1]) / ticks_per_ms_;
        section.draws += slot.draws[i];
        section.position += double(i) / slot.marks;
        ++section.count;
      }
      frame_milliseconds_ +=
          double(ticks[slot.marks - 1] - ticks[0]) / ticks_per_ms_;
      const D3D12_RANGE written = {0, 0};
      readback_->Unmap(0, &written);
    }
    slot.marks = 0;
    if (++frames_ >= kLogInterval) {
      Log();
    }
  }

  void Log() {
    std::vector<std::pair<std::string, Section>> sections(sections_.begin(),
                                                          sections_.end());
    std::sort(sections.begin(), sections.end(), [](auto& a, auto& b) {
      return a.second.position / a.second.count <
             b.second.position / b.second.count;
    });
    XELOGI("GT: {} frames, GPU {:.2f} ms per frame (first draw to swap end)",
           frames_, frame_milliseconds_ / frames_);
    for (const auto& [label, section] : sections) {
      // Sections in fewer than a tenth of the frames are left out.
      if (section.count * 10 < frames_) {
        continue;
      }
      XELOGI("GT: {:7.3f} ms {:6.1f} draws  {}", section.milliseconds / frames_,
             section.draws / section.count, label);
    }
    sections_.clear();
    frame_milliseconds_ = 0.0;
    frames_ = 0;
  }

  Microsoft::WRL::ComPtr<ID3D12QueryHeap> heap_;
  Microsoft::WRL::ComPtr<ID3D12Resource> readback_;
  bool failed_ = false;
  double ticks_per_ms_ = 1.0;
  Slot slots_[kSlots];
  uint32_t slot_ = 0;
  uint32_t frame_marks_ = 0;
  uint32_t draws_ = 0;
  std::unordered_map<std::string, Section> sections_;
  double frame_milliseconds_ = 0.0;
  uint32_t frames_ = 0;
};

}  // namespace xe::gpu::d3d12::dao_timing
