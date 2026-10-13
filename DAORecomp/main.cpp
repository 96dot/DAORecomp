#include "stdafx.h"
#include "kernel/function.h"
#include "kernel/memory.h"
#include "kernel/module.h"
#include "kernel/thread.h"
#include "kernel/vfs.h"
#include "gpu/xenia_host.h"
#include "apu/audio_output.h"
#include "input/input.h"
#include "content/stfs.h"
#include "launcher/launcher.h"
#include "mods/mods.h"
#include "mods/convert/convert.h"
#include "ui/graphics_menu.h"
#include "ui/keyboard.h"
#include "program.h"
#include "settings.h"
#include "text.h"

#include <shellapi.h>
#include <timeapi.h>
#include <tlhelp32.h>
#include <csignal>
#include <exception>
#include <sstream>
#include <fstream>

#include "gpu/video.h"
#include "xenia/dao_live_settings.h"
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
    }

    // This sees exceptions first, ones other code (such as a driver) handles too: a
    // few are logged, not a flood of them.
    constexpr uint32_t kMaxLogged = 32;
    static std::atomic<uint32_t> s_logged{ 0 };
    const uint32_t logged = s_logged.fetch_add(1, std::memory_order_relaxed);
    if (logged >= kMaxLogged)
        return EXCEPTION_CONTINUE_SEARCH;
    if (logged == kMaxLogged - 1)
        LOGE("(Exceptions after this one are not logged.)");

    if (code == EXCEPTION_ACCESS_VIOLATION)
    {
        const auto* p = info->ExceptionRecord->ExceptionInformation;

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

// Names an unnamed thread by where it started, "<module>+<offset>" (the
// driver's and the system's threads have no names); `module` gets the module.
static std::string ThreadStartName(HANDLE thread, HMODULE& module)
{
    using QueryFn = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    static const auto query = reinterpret_cast<QueryFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
    constexpr ULONG kThreadQuerySetWin32StartAddress = 9;
    void* start = nullptr;
    if (!query || query(thread, kThreadQuerySetWin32StartAddress, &start, sizeof(start), nullptr) < 0 || !start)
        return {};
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCWSTR>(start), &module) ||
        !GetModuleFileNameW(module, path, MAX_PATH))
        return "?";
    const std::wstring file = fs::path(path).filename().wstring();
    return fmt::format("{}+{:X}", std::string(file.begin(), file.end()),
        reinterpret_cast<uintptr_t>(start) - reinterpret_cast<uintptr_t>(module));
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

static fs::path FindGameRoot()
{
    // The first argument that isn't a --flag is the game folder (read as UTF-16:
    // argv is in the ANSI code page, which may lack the folder's characters).
    int count = 0;
    wchar_t** args = CommandLineToArgvW(GetCommandLineW(), &count);
    for (int i = 1; args && i < count; i++)
    {
        if (wcsncmp(args[i], L"--", 2) != 0)
        {
            const fs::path folder = args[i];
            LocalFree(args);
            return folder;
        }
    }
    LocalFree(args);
    if (const wchar_t* env = _wgetenv(L"DAO_GAME"))
        return fs::path(env);

    for (fs::path dir = dao::ProgramPath().parent_path(); !dir.empty(); dir = dir.parent_path())
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
    // DLLs loaded by name (FidelityFX's and the optional dxcompiler/dxilconv, next to
    // the exe; the system's) come from this program's folder and System32, not the
    // current folder or PATH.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);

    // A window program: no console behind the launcher. The log is in dao.log and goes to
    // stdout too (so a redirected file or pipe gets it); DAO_CONSOLE=1 also shows it in a
    // console, the one it was started from or a new one.
    if (getenv("DAO_CONSOLE") && GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) == FILE_TYPE_UNKNOWN &&
        (AttachConsole(ATTACH_PARENT_PROCESS) || AllocConsole()))
    {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }

    // Real pixels everywhere (launcher, game window, 4K swap chain): without
    // this, Windows stretches the window on scaled displays and reports scaled
    // coordinates.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // Without a trailing separator (as DAO_GAME or an argument may have it), so that
    // parent_path() is the install folder.
    fs::path gameRoot = fs::absolute(FindGameRoot()).lexically_normal();
    if (!gameRoot.has_filename())
        gameRoot = gameRoot.parent_path();
    const fs::path userRoot = gameRoot.parent_path() / "userdata";
    fs::create_directories(userRoot);
    dao::log::Init(userRoot / "dao.log");

    LOGI("Dragon Age: Origins recomp - game {}, user data {}", dao::Utf8(gameRoot), dao::Utf8(userRoot));

    // An install replaced this exe while the one before ran: the installer renamed
    // that one to DAORecomp.old.exe (installer/pipeline.py). Deleted once it has
    // exited (it may be the setup that started this one).
    std::thread([] {
        const fs::path old = dao::ProgramPath().parent_path() / "DAORecomp.old.exe";
        std::error_code ec;
        for (int attempt = 0; attempt < 20 && fs::exists(old, ec); attempt++)
        {
            if (fs::remove(old, ec))
                break;
            Sleep(500);
        }
    }).detach();
    if (!fs::exists(gameRoot / "default.xex"))
    {
        LOGE("default.xex not found under {}", dao::Utf8(gameRoot));
        return 1;
    }

    // DAO_CONVERT_MOD=<mod folder>: converts a PC mod and exits (for testing the converter).
    if (const wchar_t* folder = _wgetenv(L"DAO_CONVERT_MOD"))
    {
        const auto result = dao::mods::ConvertPcMod(folder, gameRoot);
        for (const std::string& problem : result.problems)
            LOGW("Convert: {}", problem);
        for (const std::string& file : result.leftOut)
            LOGW("Convert: left out {}", file);
        for (const std::string& note : result.notes)
            LOGI("Convert: {}", note);
        LOGI("Convert: {} files, {}", result.files, result.converted ? "done" : "nothing changed");
        return result.converted ? 0 : 1;
    }

    // DAO_EXTRACT_PACKAGE=<package>|<new folder>: extracts an Xbox 360 package as the
    // launcher does and exits (scripts/test_stfs.py tests the reader with it).
    if (const wchar_t* spec = _wgetenv(L"DAO_EXTRACT_PACKAGE"))
    {
        const std::wstring_view text(spec);
        const size_t bar = text.find(L'|');
        std::string error = bar == text.npos ? "no folder given" : "";
        if (error.empty())
        {
            const auto package = dao::content::StfsPackage::Open(fs::path(text.substr(0, bar)), error);
            const fs::path into(text.substr(bar + 1));
            std::error_code ec;
            if (package && !fs::create_directory(into, ec))
                error = "can't create the folder";
            else if (package)
                package->Extract(into, {}, error);
        }
        LOGI("Package: {}", error.empty() ? "ok" : error);
        return error.empty() ? 0 : 1;
    }

    if (dao::launcher::ShouldShow(argc, argv, userRoot) &&
        !dao::launcher::Run(gameRoot, userRoot, dao::launcher::ShowsMods(argc, argv),
            dao::launcher::AddsToSteam(argc, argv)))
        return 0;

    AddVectoredExceptionHandler(0, CrashHandler);
    signal(SIGABRT, AbortHandler);
    std::set_terminate(TerminateHandler);
    timeBeginPeriod(1);
    // Windows 11 drops a hidden or minimized window's process back to its coarse
    // timer (~15.6 ms sleeps) unless the process opts out.
    PROCESS_POWER_THROTTLING_STATE throttling{ PROCESS_POWER_THROTTLING_CURRENT_VERSION,
        PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION, 0 };
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling, sizeof(throttling));

    SelectBuild(gameRoot);
    if (!g_memory.Init())
    {
        LOGE("Failed to reserve the 4 GB guest address space");
        return 1;
    }

    // Every code address without a recompiled function gets a reporting trampoline.
    PPCFunc** const slots = g_memory.FunctionSlot(g_build.info.codeBase);
    for (uint32_t i = 0; i < g_build.info.codeSize / 4; i++)
    {
        if (!slots[i])
            slots[i] = UnknownIndirectCall;
    }

    g_processBlock = KernelAlloc(0x60);
    VfsInit(gameRoot, userRoot);
    if (IsTitleUpdate())
    {
        // The console mounts a title update's package as update:.
        VfsMountDevice("\\Device\\Update", UpdateRoot(gameRoot));
        VfsCreateSymlink("update:", "\\Device\\Update");
    }

    if (!LoadXex(gameRoot / "default.xex", IsTitleUpdate() ? UpdateRoot(gameRoot) / "default.xexp" : fs::path()))
        return 1;
    StartTimeStampThread();

    dao::LoadSettings(userRoot);
    dao::gpu::LoadXeniaLiveSettings();
    {
        // Same rule as the launcher: mod folders not in config.toml yet are enabled.
        const auto modsRoot = dao::mods::ModsRoot(gameRoot);
        dao::mods::Apply(modsRoot,
            dao::mods::Scan(modsRoot, dao::GetSettings().mods, nullptr, dao::mods::Detail::Loading));
    }
    // The Options menu's Graphics tab is patched into the menu the mods leave.
    dao::ui::InitializeGraphicsMenu();
    // DAO_AUDIO=null runs without sound output.
    if (const char* audio = getenv("DAO_AUDIO"); !audio || strcmp(audio, "null") != 0)
        dao::apu::InitializeAudioOutput();
    dao::input::Initialize();

    // DAO_GPU=null runs without rendering (null command processor).
    const char* gpuMode = getenv("DAO_GPU");
    if (!(gpuMode && strcmp(gpuMode, "null") == 0))
        dao::gpu::InitializeXeniaGraphics(userRoot);

    // DAO_TEST_KEYBOARD=<seconds>: shows the on-screen keyboard that long after start
    // (to test it without reaching character creation) and logs the outcome.
    if (const char* keyboard = getenv("DAO_TEST_KEYBOARD"))
    {
        const int seconds = std::max(1, atoi(keyboard));
        std::thread([seconds] {
            std::this_thread::sleep_for(std::chrono::seconds(seconds));
            const auto text = dao::ui::RunKeyboard({ u"Name", u"Enter a name for your character.", u"Aedan", 15 });
            LOGI("Keyboard test: {}", text ? dao::WideToUtf8(std::wstring(text->begin(), text->end())) : "cancelled");
        }).detach();
    }

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
                LOGI("Capture {}: {}", dao::Utf8(path), dao::gpu::CaptureFrame(path) ? "ok" : "failed");
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

    // Hotkeys (game window focused): F6 pauses neural rendering and resumes it, F10
    // switches between the game's own 30 fps and the frame rate set, F12 saves the
    // frame history (DAO_FRAME_HISTORY). Polled at 20 Hz: each poll is a few window
    // system calls (round trips to wineserver on Proton), and a key press is longer.
    std::thread([frameHistory, neural = dao::GetSettings().neuralRendering] {
        constexpr int keys[] = { VK_F6, VK_F10, VK_F12 };
        bool wasDown[std::size(keys)] = {};
        for (;;)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            DWORD process = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &process);
            const bool focused = process == GetCurrentProcessId();
            for (size_t i = 0; i < std::size(keys); i++)
            {
                const bool down = focused && (GetAsyncKeyState(keys[i]) & 0x8000);
                if (down && !wasDown[i])
                {
                    if (keys[i] == VK_F6 && neural)
                    {
                        auto& paused = xe::dao_live::values.neural_paused;
                        const bool pause = !paused.load(std::memory_order_relaxed);
                        paused.store(pause, std::memory_order_relaxed);
                        LOGI("Neural rendering: {} (F6)", pause ? "paused" : "resumed");
                    }
                    else if (keys[i] == VK_F10)
                        dao::gpu::ToggleGameFrameRate();
                    else if (keys[i] == VK_F12 && frameHistory)
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
    // use over each interval (finds what limits the frame rate). Unnamed threads are
    // named by their start address, and our own ones over 10% get a few host stack
    // samples, for scripts/symbolize.py (others, the driver's, are left alone:
    // suspended in the loader, they would block the stack walk).
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
                std::vector<std::string> samples;
                for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry))
                {
                    if (entry.th32OwnerProcessID != GetCurrentProcessId())
                        continue;
                    // The full query right is for the start address.
                    HANDLE thread = OpenThread(THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
                    if (!thread)
                        thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
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
                            else
                            {
                                HMODULE module = nullptr;
                                if (std::string start = ThreadStartName(thread, module); !start.empty())
                                    name += " " + start;
                                // Three samples, a few milliseconds apart.
                                if (percent >= 10.0 && module == GetModuleHandleW(nullptr) &&
                                    entry.th32ThreadID != GetCurrentThreadId())
                                    for (int i = 0; i < 3; i++)
                                    {
                                        samples.push_back(fmt::format("  {} host stack:{}", name,
                                            SampleHostStack(entry.th32ThreadID, 24)));
                                        Sleep(3);
                                    }
                            }
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
                for (const std::string& sample : samples)
                    LOGI("{}", sample);
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
