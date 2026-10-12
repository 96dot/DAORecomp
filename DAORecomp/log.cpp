#include "stdafx.h"
#ifndef DAO_LAUNCHER_ONLY
#include "kernel/thread.h"
#endif

namespace dao::log
{
    static std::mutex s_mutex;
    static FILE* s_file;
    static bool s_trace = getenv("DAO_TRACE") != nullptr;
    static const auto s_start = std::chrono::steady_clock::now();

    void Init(const std::filesystem::path& path)
    {
        s_file = _wfopen(path.c_str(), L"w");
    }

    bool TraceEnabled()
    {
        return s_trace;
    }

    void Write(Level level, std::string_view message)
    {
        static constexpr const char* prefixes[] = { "T", "I", "W", "E" };
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - s_start).count();
#ifdef DAO_LAUNCHER_ONLY
        const uint32_t threadId = 0;
#else
        auto* thread = kernel::GuestThread::Current();
        const uint32_t threadId = thread ? thread->threadId : 0;
#endif
        const auto line = fmt::format("[{:9.3f}] {} {:>3} | {}\n", t, prefixes[int(level)], threadId, message);

        std::lock_guard lock(s_mutex);
        fputs(line.c_str(), stdout);
        fflush(stdout);
        if (s_file)
        {
            fputs(line.c_str(), s_file);
            fflush(s_file);
        }
    }
}
