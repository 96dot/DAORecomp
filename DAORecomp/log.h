#pragma once

#include <fmt/core.h>
#include <string_view>

namespace dao::log
{
    enum class Level { Trace, Info, Warn, Error };

    void Init(const char* path);
    void Write(Level level, std::string_view message);
    bool TraceEnabled();
}

#define LOGT(...) do { if (::dao::log::TraceEnabled()) ::dao::log::Write(::dao::log::Level::Trace, fmt::format(__VA_ARGS__)); } while (0)
#define LOGI(...) ::dao::log::Write(::dao::log::Level::Info, fmt::format(__VA_ARGS__))
#define LOGW(...) ::dao::log::Write(::dao::log::Level::Warn, fmt::format(__VA_ARGS__))
#define LOGE(...) ::dao::log::Write(::dao::log::Level::Error, fmt::format(__VA_ARGS__))
