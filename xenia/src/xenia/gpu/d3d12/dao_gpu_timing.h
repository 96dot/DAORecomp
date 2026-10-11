// DAO: GPU time of each part of a frame, for finding what to optimize.
// DAO_GPU_TIMING=1 writes GPU timestamps at the first draw of a frame, at every
// resolve and around the swap's upscaling, and every kLogInterval frames logs
// the average time between them to xenia.log with the "GT" prefix. A section
// is named after the event ending it (a resolve: its source and destination),
// so "resolve ..." is the time of the draws before that resolve (and of the
// previous resolve). Its time is averaged over all the frames, its draw count
// over the frames that have it. Marks past kMaxMarks in a frame are dropped
// (counted in the log).
// Each draw is also wrapped in a pipeline statistics query (which serializes
// the draws, so the times are inflated while timing): the vertices, vertex
// shader invocations, primitives and pixel shader invocations are logged per
// section, per kind of draw (primitive, index buffer and vertex shader types)
// and for the shader pairs doing the most vertex and pixel work.
#pragma once

#include <algorithm>
#include <cstring>
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

  // Around each draw: its pipeline statistics, summed into its section.
  void BeginDrawStatistics(D3D12CommandProcessor& cp) {
    if (!stats_heap_ || stats_draws_ >= kMaxDraws) {
      return;
    }
    cp.GetDeferredCommandList().D3DBeginQuery(
        stats_heap_.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS,
        slot_ * kMaxDraws + stats_draws_);
  }
  void EndDrawStatistics(D3D12CommandProcessor& cp, std::string kind = {},
                         std::string shaders = {}, uint32_t indices = 0) {
    if (!stats_heap_ || stats_draws_ >= kMaxDraws) {
      return;
    }
    cp.GetDeferredCommandList().D3DEndQuery(
        stats_heap_.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS,
        slot_ * kMaxDraws + stats_draws_);
    Slot& slot = slots_[slot_];
    slot.stats_marks.resize(stats_draws_ + 1);
    slot.stats_marks[stats_draws_] = frame_marks_;
    slot.stats_kinds.resize(stats_draws_ + 1);
    slot.stats_kinds[stats_draws_] = std::move(kind);
    slot.stats_shaders.resize(stats_draws_ + 1);
    slot.stats_shaders[stats_draws_] = std::move(shaders);
    slot.stats_indices.resize(stats_draws_ + 1);
    slot.stats_indices[stats_draws_] = indices;
    ++stats_draws_;
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
      truncated_ = true;
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
    if (stats_heap_ && stats_draws_) {
      cp.GetDeferredCommandList().D3DResolveQueryData(
          stats_heap_.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS,
          slot_ * kMaxDraws, stats_draws_, stats_readback_.Get(),
          slot_ * kMaxDraws * sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS));
    }
    slot.stats_draws = stats_draws_;
    stats_draws_ = 0;
    slot.submission = cp.GetCurrentSubmission();
    slot.marks = frame_marks_;
    slot.truncated = truncated_;
    truncated_ = false;
    frame_marks_ = 0;
    draws_ = 0;
    slot_ = (slot_ + 1) % kSlots;
  }

 private:
  static constexpr uint32_t kSlots = 4;
  static constexpr uint32_t kMaxMarks = 512;
  static constexpr uint32_t kLogInterval = 300;
  static constexpr uint32_t kMaxDraws = 4096;

  struct Slot {
    uint64_t submission = 0;
    uint32_t marks = 0;
    bool truncated = false;
    std::vector<std::string> labels;
    std::vector<uint32_t> draws;
    uint32_t stats_draws = 0;
    std::vector<uint32_t> stats_marks;
    std::vector<std::string> stats_kinds, stats_shaders;
    std::vector<uint32_t> stats_indices;
  };
  struct TagStats {
    double draws = 0, indices = 0, ia = 0, vs = 0, prim = 0, ps = 0;
  };
  std::unordered_map<std::string, TagStats> kind_stats_, shader_stats_;
  static void AddTag(TagStats& t, const D3D12_QUERY_DATA_PIPELINE_STATISTICS& s,
                     uint32_t indices) {
    t.draws += 1;
    t.indices += indices;
    t.ia += double(s.IAVertices);
    t.vs += double(s.VSInvocations);
    t.prim += double(s.CPrimitives);
    t.ps += double(s.PSInvocations);
  }
  void LogTags(const char* name,
               std::unordered_map<std::string, TagStats>& tags, bool by_ps) {
    std::vector<std::pair<std::string, TagStats>> v(tags.begin(), tags.end());
    std::sort(v.begin(), v.end(), [by_ps](auto& a, auto& b) {
      return by_ps ? a.second.ps > b.second.ps : a.second.vs > b.second.vs;
    });
    for (size_t i = 0; i < v.size() && i < 14; ++i) {
      const TagStats& t = v[i].second;
      XELOGI(
          "GT {}: {:6.1f} draws {:7.0f}k idx {:7.0f}k ia {:7.0f}k vs {:7.0f}k "
          "prim {:7.0f}k ps  {}",
          name, t.draws / frames_, t.indices / frames_ / 1000.0,
          t.ia / frames_ / 1000.0, t.vs / frames_ / 1000.0,
          t.prim / frames_ / 1000.0, t.ps / frames_ / 1000.0, v[i].first);
    }
  }

  struct Section {
    double milliseconds = 0.0;
    double draws = 0.0;
    double position = 0.0;
    uint32_t count = 0;
    double ia_vertices = 0.0, vs = 0.0, primitives = 0.0, ps = 0.0;
  };

  bool Initialize(D3D12CommandProcessor& cp) {
    if (heap_) {
      return true;
    }
    if (failed_) {
      return false;
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
      XELOGE("GT: failed to create the timestamp query heap");
      heap_.Reset();
      failed_ = true;
      return false;
    }
    ticks_per_ms_ = double(frequency) / 1000.0;
    D3D12_QUERY_HEAP_DESC stats_heap_desc = {};
    stats_heap_desc.Type = D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS;
    stats_heap_desc.Count = kSlots * kMaxDraws;
    buffer_desc.Width =
        kSlots * kMaxDraws * sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS);
    if (FAILED(device->CreateQueryHeap(&stats_heap_desc,
                                       IID_PPV_ARGS(&stats_heap_))) ||
        FAILED(device->CreateCommittedResource(
            &heap_properties, D3D12_HEAP_FLAG_NONE, &buffer_desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&stats_readback_)))) {
      stats_heap_.Reset();
    }
    XELOGI("GT: GPU timing enabled");
    return true;
  }

  // Accumulates the timestamps of a frame resolved earlier into `index`.
  void ReadSlot(D3D12CommandProcessor& cp, uint32_t index) {
    Slot& slot = slots_[index];
    if (!slot.marks) {
      return;
    }
    // Normally long done; not awaited if that would end the open submission
    // (when swaps fail to submit), then the frame is dropped.
    if (slot.submission >= cp.GetCurrentSubmission()) {
      slot.marks = 0;
      return;
    }
    cp.CheckSubmissionCompletion(slot.submission);
    std::vector<D3D12_QUERY_DATA_PIPELINE_STATISTICS> stats(slot.stats_draws);
    if (slot.stats_draws) {
      const D3D12_RANGE stats_range = {
          index * kMaxDraws * sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS),
          (index * kMaxDraws + slot.stats_draws) *
              sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS)};
      void* stats_mapping;
      if (SUCCEEDED(stats_readback_->Map(0, &stats_range, &stats_mapping))) {
        std::memcpy(stats.data(),
                    reinterpret_cast<const D3D12_QUERY_DATA_PIPELINE_STATISTICS*>(
                        stats_mapping) +
                        index * kMaxDraws,
                    stats.size() * sizeof(stats[0]));
        const D3D12_RANGE written = {0, 0};
        stats_readback_->Unmap(0, &written);
      }
    }
    std::vector<D3D12_QUERY_DATA_PIPELINE_STATISTICS> mark_stats(slot.marks);
    for (uint32_t i = 0; i < slot.stats_draws; ++i) {
      const uint32_t mark = slot.stats_marks[i];
      AddTag(kind_stats_[slot.stats_kinds[i]], stats[i], slot.stats_indices[i]);
      AddTag(shader_stats_[slot.stats_shaders[i]], stats[i],
             slot.stats_indices[i]);
      if (mark < slot.marks) {
        mark_stats[mark].IAVertices += stats[i].IAVertices;
        mark_stats[mark].VSInvocations += stats[i].VSInvocations;
        mark_stats[mark].CPrimitives += stats[i].CPrimitives;
        mark_stats[mark].PSInvocations += stats[i].PSInvocations;
      }
    }
    slot.stats_draws = 0;
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
        // Timestamps going back (a GPU clock reset) count as no time.
        section.milliseconds +=
            ticks[i] > ticks[i - 1]
                ? double(ticks[i] - ticks[i - 1]) / ticks_per_ms_
                : 0.0;
        section.draws += slot.draws[i];
        section.position += double(i) / slot.marks;
        ++section.count;
        section.ia_vertices += double(mark_stats[i].IAVertices);
        section.vs += double(mark_stats[i].VSInvocations);
        section.primitives += double(mark_stats[i].CPrimitives);
        section.ps += double(mark_stats[i].PSInvocations);
      }
      if (ticks[slot.marks - 1] > ticks[0]) {
        frame_milliseconds_ +=
            double(ticks[slot.marks - 1] - ticks[0]) / ticks_per_ms_;
      }
      truncated_frames_ += slot.truncated ? 1 : 0;
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
    XELOGI(
        "GT: {} frames ({} with marks dropped), GPU {:.2f} ms per frame (first "
        "draw to swap end)",
        frames_, truncated_frames_, frame_milliseconds_ / frames_);
    for (const auto& [label, section] : sections) {
      // Sections in fewer than a tenth of the frames are left out.
      if (section.count * 10 < frames_) {
        continue;
      }
      XELOGI(
          "GT: {:7.3f} ms {:6.1f} draws {:7.0f}k ia {:7.0f}k vs {:7.0f}k prim "
          "{:7.0f}k ps  {}",
          section.milliseconds / frames_, section.draws / section.count,
          section.ia_vertices / section.count / 1000.0,
          section.vs / section.count / 1000.0,
          section.primitives / section.count / 1000.0,
          section.ps / section.count / 1000.0, label);
    }
    LogTags("kind", kind_stats_, false);
    LogTags("vs", shader_stats_, false);
    LogTags("ps", shader_stats_, true);
    kind_stats_.clear();
    shader_stats_.clear();
    sections_.clear();
    frame_milliseconds_ = 0.0;
    frames_ = 0;
    truncated_frames_ = 0;
  }

  Microsoft::WRL::ComPtr<ID3D12QueryHeap> heap_;
  Microsoft::WRL::ComPtr<ID3D12Resource> readback_;
  Microsoft::WRL::ComPtr<ID3D12QueryHeap> stats_heap_;
  Microsoft::WRL::ComPtr<ID3D12Resource> stats_readback_;
  uint32_t stats_draws_ = 0;
  bool failed_ = false;
  double ticks_per_ms_ = 1.0;
  Slot slots_[kSlots];
  uint32_t slot_ = 0;
  uint32_t frame_marks_ = 0;
  bool truncated_ = false;
  uint32_t draws_ = 0;
  std::unordered_map<std::string, Section> sections_;
  double frame_milliseconds_ = 0.0;
  uint32_t frames_ = 0;
  uint32_t truncated_frames_ = 0;
};

}  // namespace xe::gpu::d3d12::dao_timing
