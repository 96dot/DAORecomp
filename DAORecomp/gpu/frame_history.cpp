#include "stdafx.h"
#include <fstream>
#include "gpu/frame_history.h"
#include "gpu/video.h"
#include "gpu/xenia_host.h"

#include "xenia/gpu/d3d12/dao_frame_trace.h"

namespace dao::gpu
{
    namespace
    {
        constexpr size_t kHistoryFrames = 180;
        constexpr int kMaxFlickerReports = 200;
        constexpr int kMaxFlickerImages = 40;

        struct Frame
        {
            std::vector<uint8_t> rgb;
            uint64_t swap = 0;
            uint32_t gpuSwap = 0; // GPU swaps processed when captured
        };

        std::mutex s_mutex;
        std::deque<std::shared_ptr<Frame>> s_frames;
        uint32_t s_width = 0, s_height = 0;
        std::filesystem::path s_userRoot;

        // Number of (sampled) pixels whose luma differs by more than a threshold.
        uint32_t ChangedPixels(const Frame& a, const Frame& b)
        {
            uint32_t changed = 0;
            const size_t pixels = a.rgb.size() / 3;
            for (size_t i = 0; i < pixels; i += 2)
            {
                const uint8_t* p = &a.rgb[i * 3];
                const uint8_t* q = &b.rgb[i * 3];
                const int la = p[0] * 2 + p[1] * 5 + p[2];
                const int lb = q[0] * 2 + q[1] * 5 + q[2];
                changed += std::abs(la - lb) > 40 * 8;
            }
            return changed;
        }

        void Save(const std::filesystem::path& path, const Frame& frame, uint32_t width, uint32_t height)
        {
            WritePng(path, frame.rgb.data(), width, height);
        }

        void Record()
        {
            int reports = 0;
            uint64_t lastHash = 0;
            for (;;)
            {
                auto frame = std::make_shared<Frame>();
                uint32_t width, height;
                if (!CaptureHalfRes(frame->rgb, width, height))
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    continue;
                }
                frame->swap = g_video.swapCount.load();
                frame->gpuSwap = xe::gpu::dao_trace::SwapCount();
                const uint64_t hash = std::hash<std::string_view>{}(
                    std::string_view(reinterpret_cast<const char*>(frame->rgb.data()), frame->rgb.size()));
                if (hash == lastHash)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    continue;
                }
                lastHash = hash;

                std::shared_ptr<Frame> a, b;
                {
                    std::lock_guard lock(s_mutex);
                    if (width != s_width || height != s_height)
                        s_frames.clear();
                    s_width = width;
                    s_height = height;
                    if (s_frames.size() >= 2)
                    {
                        a = s_frames[s_frames.size() - 2];
                        b = s_frames.back();
                    }
                    s_frames.push_back(frame);
                    if (s_frames.size() > kHistoryFrames)
                        s_frames.pop_front();
                }
                if (!a || reports >= kMaxFlickerReports)
                    continue;

                // b is a transient if it differs from both neighbours far more than they
                // differ from each other.
                const uint32_t ab = ChangedPixels(*a, *b), bc = ChangedPixels(*b, *frame), ac = ChangedPixels(*a, *frame);
                const uint32_t pixels = width * height / 2;
                if (std::min(ab, bc) > ac * 4 + pixels / 2000)
                {
                    const int n = reports++;
                    LOGW("Flicker suspect {} at swap {}: changed pixels prev->it {}, it->next {}, prev->next {} "
                         "(of {} sampled)", n, b->swap, ab, bc, ac, pixels);
                    // Draw statistics of the GPU frames around the flicker.
                    std::string stats;
                    for (uint32_t swap = a->gpuSwap > 4 ? a->gpuSwap - 4 : 0; swap <= frame->gpuSwap; swap++)
                    {
                        xe::gpu::dao_trace::FrameStats fs;
                        if (xe::gpu::dao_trace::GetFrameStats(swap, fs))
                            stats += fmt::format(" | {}{}: sub {} viz {} fail {} issued {}", swap,
                                swap == b->gpuSwap ? "*" : "", fs.submitted, fs.viz_skipped, fs.failed, fs.issued);
                    }
                    LOGW("Flicker {} draws (GPU swaps prev {} it {} next {}){}", n, a->gpuSwap, b->gpuSwap,
                        frame->gpuSwap, stats);
                    const auto dir = s_userRoot / "flicker";
                    std::filesystem::create_directories(dir);
                    // Draw lists of the same frames, for diffing (scripts/diff_draws.py).
                    {
                        std::ofstream file(dir / fmt::format("{:03}_draws.txt", n));
                        for (uint32_t swap = a->gpuSwap > 3 ? a->gpuSwap - 3 : 0; swap <= frame->gpuSwap; swap++)
                        {
                            std::vector<xe::gpu::dao_trace::DrawRecord> draws;
                            if (!xe::gpu::dao_trace::GetFrameDraws(swap, draws))
                                continue;
                            file << "swap " << swap << (swap == b->gpuSwap ? " flicker" : "") << '\n';
                            for (const auto& d : draws)
                                file << fmt::format("{:08X} {:08X} {} {}\n", d.index_base, d.vertex_fetch0, d.count,
                                    d.prim);
                        }
                    }
                    if (n >= kMaxFlickerImages)
                        continue;
                    Save(dir / fmt::format("{:02}_0_prev.png", n), *a, width, height);
                    Save(dir / fmt::format("{:02}_1_flicker.png", n), *b, width, height);
                    Save(dir / fmt::format("{:02}_2_next.png", n), *frame, width, height);
                }
            }
        }
    }

    void StartFrameHistory(const std::filesystem::path& userRoot)
    {
        s_userRoot = userRoot;
        std::filesystem::remove_all(userRoot / "flicker");
        std::thread(Record).detach();
        LOGI("Frame history: recording (F12 saves the last {} frames)", kHistoryFrames);
    }

    void SaveFrameHistory()
    {
        std::deque<std::shared_ptr<Frame>> frames;
        uint32_t width, height;
        {
            std::lock_guard lock(s_mutex);
            frames = s_frames;
            width = s_width;
            height = s_height;
        }
        if (frames.empty())
            return;
        std::thread([frames = std::move(frames), width, height] {
            const auto dir = s_userRoot / "history" / fmt::format("{}", std::time(nullptr));
            std::filesystem::create_directories(dir);
            for (size_t i = 0; i < frames.size(); i++)
                Save(dir / fmt::format("{:03}_swap{}.png", i, frames[i]->swap), *frames[i], width, height);
            LOGI("Frame history: saved {} frames to {}", frames.size(), dir.string());
        }).detach();
    }
}
