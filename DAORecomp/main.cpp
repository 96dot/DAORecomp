#include "stdafx.h"
#include "kernel/function.h"
#include "kernel/memory.h"
#include "kernel/module.h"
#include "kernel/thread.h"
#include "kernel/vfs.h"
#include "gpu/xenia_host.h"
#include "apu/audio_output.h"
#include "input/input.h"
#include "launcher/launcher.h"
#include "mods/mods.h"
#include "settings.h"

#include <timeapi.h>
#include <tlhelp32.h>
#include <csignal>
#include <exception>
#include <sstream>
#include <fstream>

#include "gpu/video.h"
#include "gpu/frame_history.h"

using namespace dao::kernel;
namespace fs = std::filesystem;

// mftb: the 360 timebase runs at 49.875 MHz.
uint64_t PPCQueryTimebase() noexcept
{
    static const uint64_t frequency = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return uint64_t(f.QuadPart);
    }();
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    const uint64_t ticks = uint64_t(counter.QuadPart);
    return (ticks / frequency) * 49875000u + (ticks % frequency) * 49875000u / frequency;
}

namespace dao
{
    void ReportStub(const char* name, PPCContext& ctx)
    {
        static std::mutex mutex;
        static std::unordered_map<std::string, uint32_t> counts;
        uint32_t count;
        {
            std::lock_guard lock(mutex);
            count = ++counts[name];
        }
        if (count <= 3 || log::TraceEnabled())
        {
            LOGW("Unimplemented import {} ({:08X}, {:08X}, {:08X}, {:08X}) lr {:08X}{}", name, ctx.r3.u32, ctx.r4.u32,
                ctx.r5.u32, ctx.r6.u32, uint32_t(ctx.lr), count == 3 ? " (further calls muted)" : "");
        }
    }
}

// Indirect calls to addresses without a recompiled function land here.
static PPC_FUNC(UnknownIndirectCall)
{
    LOGE("Indirect call to unrecompiled address {:08X} (lr {:08X})", ctx.ctr.u32, uint32_t(ctx.lr));
    if (IsDebuggerPresent())
        __debugbreak();
    TerminateProcess(GetCurrentProcess(), 0xDEAD0002);
}

static LONG CALLBACK CrashHandler(EXCEPTION_POINTERS* info)
{
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code == DBG_PRINTEXCEPTION_C || code == DBG_PRINTEXCEPTION_WIDE_C || code == 0x406D1388 || code == 0xE06D7363)
        return EXCEPTION_CONTINUE_SEARCH; // OutputDebugString, thread naming, C++ throw (ThreadExitException)

    if (code == EXCEPTION_ACCESS_VIOLATION)
    {
        const auto* p = info->ExceptionRecord->ExceptionInformation;
        // Writes to physical pages the GPU watches (to invalidate its copies).
        if (dao::gpu::HandleWatchedWrite(reinterpret_cast<void*>(p[1]), p[0] == 1))
            return EXCEPTION_CONTINUE_EXECUTION;

        const auto* address = reinterpret_cast<const uint8_t*>(p[1]);
        if (g_memory.IsGuest(address))
            LOGE("Guest access violation ({}) at guest {:08X}, host rip {:p}", p[0] ? "write" : "read",
                g_memory.ToGuest(address), info->ExceptionRecord->ExceptionAddress);
        else
            LOGE("Host access violation ({}) at {:p}, rip {:p}", p[0] ? "write" : "read", (const void*)address,
                info->ExceptionRecord->ExceptionAddress);
    }
    else
    {
        LOGE("Unhandled exception {:08X} at {:p}", code, info->ExceptionRecord->ExceptionAddress);
    }

    if (PPCContext* ctx = CurrentContext())
    {
        LOGE("  guest lr {:08X} ctr {:08X} r1 {:08X} r3 {:08X} r4 {:08X} r5 {:08X} r31 {:08X}", uint32_t(ctx->lr),
            ctx->ctr.u32, ctx->r1.u32, ctx->r3.u32, ctx->r4.u32, ctx->r5.u32, ctx->r31.u32);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// abort() (assertions, std::terminate) ends the process with a fast fail that
// the exception handler doesn't see: log where it came from first. The stack
// goes on a line of its own, after one saying what happened, in case walking
// it fails.
static void AbortHandler(int)
{
    LOGE("abort() called");
    CONTEXT context{};
    RtlCaptureContext(&context);
    LOGE("  host stack:{}", UnwindHostStack(context, 32));
}

// Only the main thread's (MSVC's terminate handlers are per thread); other
// threads' std::terminate calls abort() directly.
static void TerminateHandler()
{
    // Not again if logging throws or terminates.
    static std::atomic<bool> entered{false};
    if (!entered.exchange(true))
        LOGE("std::terminate called (the abort() below logs the stack)");
    abort();
}

static fs::path FindGameRoot(int argc, char** argv)
{
    // The first argument that isn't a --flag is the game folder.
    for (int i = 1; i < argc; i++)
    {
        if (strncmp(argv[i], "--", 2) != 0)
            return fs::path(argv[i]);
    }
    if (const char* env = getenv("DAO_GAME"))
        return fs::path(env);

    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    for (fs::path dir = fs::path(exePath).parent_path(); !dir.empty(); dir = dir.parent_path())
    {
        if (fs::exists(dir / "game" / "default.xex"))
            return dir / "game";
        if (dir == dir.parent_path())
            break;
    }
    return fs::current_path() / "game";
}

int main(int argc, char** argv)
{
    // Real pixels everywhere (launcher, game window, 4K swap chain): without
    // this, Windows stretches the window on scaled displays and reports scaled
    // coordinates.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const fs::path gameRoot = fs::absolute(FindGameRoot(argc, argv));
    const fs::path userRoot = gameRoot.parent_path() / "userdata";
    fs::create_directories(userRoot);
    dao::log::Init((userRoot / "dao.log").string().c_str());

    LOGI("Dragon Age: Origins recomp - game {}, user data {}", gameRoot.string(), userRoot.string());
    if (!fs::exists(gameRoot / "default.xex"))
    {
        LOGE("default.xex not found under {}", gameRoot.string());
        return 1;
    }

    if (dao::launcher::ShouldShow(argc, argv, userRoot) && !dao::launcher::Run(gameRoot, userRoot))
        return 0;

    AddVectoredExceptionHandler(0, CrashHandler);
    signal(SIGABRT, AbortHandler);
    std::set_terminate(TerminateHandler);
    timeBeginPeriod(1);

    if (!g_memory.Init())
    {
        LOGE("Failed to reserve the 4 GB guest address space");
        return 1;
    }

    // Every code address without a recompiled function gets a reporting trampoline.
    for (uint32_t address = PPC_CODE_BASE; address < PPC_CODE_BASE + PPC_CODE_SIZE; address += 4)
    {
        if (!g_memory.FindFunction(address))
            g_memory.InsertFunction(address, UnknownIndirectCall);
    }

    g_processBlock = KernelAlloc(0x60);
    VfsInit(gameRoot, userRoot);

    if (!LoadXex(gameRoot / "default.xex"))
        return 1;
    StartTimeStampThread();

    dao::LoadSettings(userRoot);
    {
        // Same rule as the launcher: mod folders not in config.toml yet are enabled.
        const auto modsRoot = dao::mods::ModsRoot(gameRoot);
        dao::mods::Apply(modsRoot, dao::mods::ToState(dao::mods::Scan(modsRoot, dao::GetSettings().mods)));
    }
    // DAO_AUDIO=null runs without sound output.
    if (const char* audio = getenv("DAO_AUDIO"); !audio || strcmp(audio, "null") != 0)
        dao::apu::InitializeAudioOutput();
    dao::input::Initialize();

    // DAO_GPU=null runs without rendering (null command processor).
    const char* gpuMode = getenv("DAO_GPU");
    if (!(gpuMode && strcmp(gpuMode, "null") == 0))
        dao::gpu::InitializeXeniaGraphics(userRoot);

    // DAO_DUMP_AFTER=<seconds>: periodically log what every guest thread is doing.
    if (const char* dump = getenv("DAO_DUMP_AFTER"))
    {
        const int seconds = std::max(1, atoi(dump));
        std::thread([seconds] {
            for (;;)
            {
                std::this_thread::sleep_for(std::chrono::seconds(seconds));
                LOGI("---- thread dump ----");
                DumpThreads();
            }
        }).detach();
    }

    // DAO_CAPTURE_EVERY=<seconds>: save the presented frame to userdata/capture_N.png.
    if (const char* every = getenv("DAO_CAPTURE_EVERY"))
    {
        const int seconds = std::max(1, atoi(every));
        std::thread([seconds, userRoot] {
            for (int n = 0;; n++)
            {
                std::this_thread::sleep_for(std::chrono::seconds(seconds));
                const auto path = userRoot / fmt::format("capture_{}.png", n);
                LOGI("Capture {}: {}", path.string(), dao::gpu::CaptureFrame(path) ? "ok" : "failed");
            }
        }).detach();
    }

    // DAO_FRAME_CHECK=1: every 10 s, count distinct guest frames over half a second.
    if (getenv("DAO_FRAME_CHECK"))
    {
        std::thread([] {
            for (;;)
            {
                std::this_thread::sleep_for(std::chrono::seconds(10));
                std::vector<uint64_t> hashes;
                const auto start = std::chrono::steady_clock::now();
                while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(500))
                    hashes.push_back(dao::gpu::HashFrame());
                const size_t samples = hashes.size();
                std::sort(hashes.begin(), hashes.end());
                hashes.erase(std::unique(hashes.begin(), hashes.end()), hashes.end());
                LOGI("Frame check: {} distinct frames in 0.5 s ({} samples)", hashes.size(), samples);
            }
        }).detach();
    }

    // DAO_FRAME_HISTORY=1: record recent frames and catch one-frame flicker (see frame_history.h).
    const bool frameHistory = getenv("DAO_FRAME_HISTORY") != nullptr;
    if (frameHistory)
        dao::gpu::StartFrameHistory(userRoot);

    // Debug hotkeys (game window focused): F10 toggles the frame-rate unlock, F12
    // saves the frame history.
    std::thread([frameHistory] {
        bool wasDown[2] = {};
        for (;;)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
            DWORD process = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &process);
            const bool focused = process == GetCurrentProcessId();
            const int keys[2] = { VK_F10, VK_F12 };
            for (int i = 0; i < 2; i++)
            {
                const bool down = focused && (GetAsyncKeyState(keys[i]) & 0x8000);
                if (down && !wasDown[i])
                {
                    if (keys[i] == VK_F10)
                        dao::gpu::SetFrameRateUnlocked(!dao::gpu::FrameRateUnlocked());
                    else if (frameHistory)
                        dao::gpu::SaveFrameHistory();
                }
                wasDown[i] = down;
            }
        }
    }).detach();

    // DAO_DUMP_MEM=<physical hex>:<hex length>[,...]@<seconds>: save guest physical
    // memory ranges to userdata/mem_<address>.bin (and a frame capture alongside).
    if (const char* spec = getenv("DAO_DUMP_MEM"))
    {
        std::string text = spec;
        const size_t at = text.find('@');
        const int seconds = at != std::string::npos ? atoi(text.c_str() + at + 1) : 10;
        std::vector<std::pair<uint32_t, uint32_t>> ranges;
        std::stringstream list(text.substr(0, at));
        for (std::string item; std::getline(list, item, ',');)
        {
            const size_t colon = item.find(':');
            if (colon != std::string::npos)
                ranges.emplace_back(uint32_t(strtoul(item.c_str(), nullptr, 16)),
                    uint32_t(strtoul(item.c_str() + colon + 1, nullptr, 16)));
        }
        std::thread([=] {
            std::this_thread::sleep_for(std::chrono::seconds(seconds));
            dao::gpu::CaptureFrame(userRoot / "mem_capture.png");
            for (auto [address, length] : ranges)
            {
                std::ofstream file(userRoot / fmt::format("mem_{:08X}.bin", address), std::ios::binary);
                file.write(reinterpret_cast<const char*>(g_memory.Translate(0xA0000000 + (address & 0x1FFFFFFF))),
                    length);
            }
            LOGI("Dumped {} memory ranges", ranges.size());
        }).detach();
    }

    // DAO_PROFILE=<guest thread ids, comma-separated>@<start seconds>[+<duration seconds>]:
    // sample those threads' stacks every 30 s into userdata/profile_<id>_<n>.txt
    // (scripts/profile.py reads them).
    if (const char* profile = getenv("DAO_PROFILE"))
    {
        std::string spec = profile;
        const size_t at = spec.find('@');
        const int start = at != std::string::npos ? atoi(spec.c_str() + at + 1) : 10;
        const size_t plus = spec.find('+');
        const int duration = plus != std::string::npos ? atoi(spec.c_str() + plus + 1) : 5;
        std::stringstream ids(spec.substr(0, at));
        for (std::string id; std::getline(ids, id, ',');)
        {
            const uint32_t threadId = uint32_t(atoi(id.c_str()));
            std::thread([=] {
                std::this_thread::sleep_for(std::chrono::seconds(start));
                for (int n = 0;; n++)
                {
                    LOGI("Profile {} of thread {} starting", n, threadId);
                    ProfileThread(threadId, duration * 1000,
                        userRoot / fmt::format("profile_{}_{}.txt", threadId, n));
                    std::this_thread::sleep_for(std::chrono::seconds(std::max(1, 30 - duration)));
                }
            }).detach();
        }
    }

    // DAO_THREAD_STATS=<seconds>: log the swap rate and every busy host thread's CPU
    // use over each interval (finds what limits the frame rate).
    if (const char* stats = getenv("DAO_THREAD_STATS"))
    {
        const int seconds = std::max(1, atoi(stats));
        std::thread([seconds] {
            std::unordered_map<DWORD, uint64_t> lastTimes;
            uint64_t lastSwaps = dao::gpu::g_video.swapCount.load();
            for (;;)
            {
                std::this_thread::sleep_for(std::chrono::seconds(seconds));
                const uint64_t swaps = dao::gpu::g_video.swapCount.load();
                std::string line = fmt::format("Thread stats: {:.1f} swaps/s |", double(swaps - lastSwaps) / seconds);
                lastSwaps = swaps;

                HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
                THREADENTRY32 entry{ sizeof(entry) };
                std::vector<std::pair<double, std::string>> busy;
                for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry))
                {
                    if (entry.th32OwnerProcessID != GetCurrentProcessId())
                        continue;
                    HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
                    if (!thread)
                        continue;
                    FILETIME create, exit, kernel, user;
                    if (GetThreadTimes(thread, &create, &exit, &kernel, &user))
                    {
                        const uint64_t total = (uint64_t(kernel.dwHighDateTime) << 32 | kernel.dwLowDateTime) +
                            (uint64_t(user.dwHighDateTime) << 32 | user.dwLowDateTime);
                        auto [it, inserted] = lastTimes.try_emplace(entry.th32ThreadID, 0);
                        const double percent = double(total - it->second) / (seconds * 1e5);
                        it->second = total;
                        if (!inserted && percent >= 5.0)
                        {
                            PWSTR wide = nullptr;
                            std::string name = fmt::format("tid {}", entry.th32ThreadID);
                            if (SUCCEEDED(GetThreadDescription(thread, &wide)) && wide && *wide)
                                name = std::string(wide, wide + wcslen(wide));
                            LocalFree(wide);
                            busy.emplace_back(percent, std::move(name));
                        }
                    }
                    ::CloseHandle(thread);
                }
                ::CloseHandle(snapshot);
                std::sort(busy.rbegin(), busy.rend());
                for (auto& [percent, name] : busy)
                    line += fmt::format(" {} {:.0f}%,", name, percent);
                LOGI("{}", line);
            }
        }).detach();
    }

    ThreadParams params;
    params.stackSize = g_module.stackSize;
    params.startAddress = g_module.entryPoint;
    params.isMain = true;
    params.name = "Guest main";
    auto mainThread = GuestThread::Create(params);
    if (!mainThread)
        return 1;

    const uint32_t code = mainThread->RunOnCurrentHostThread();
    LOGI("Main thread returned {:#x}", code);
    return int(code);
}
