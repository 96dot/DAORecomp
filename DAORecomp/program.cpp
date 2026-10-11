#include "stdafx.h"
#include "program.h"

namespace fs = std::filesystem;

namespace dao
{
    fs::path ProgramPath()
    {
        std::wstring path(MAX_PATH, L'\0');
        for (;;)
        {
            const DWORD length = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
            if (length == 0)
                return {};
            if (length < path.size())
            {
                path.resize(length);
                return path;
            }
            path.resize(path.size() * 2);
        }
    }

    std::wstring QuoteArgument(const fs::path& path)
    {
        std::wstring text = path.wstring();
        if (text.ends_with(L'\\'))
            text += L'.';
        return L"\"" + text + L"\"";
    }

    bool StartProgram(const fs::path& program, std::wstring_view arguments, DWORD flags, const fs::path& folder)
    {
        std::wstring command = QuoteArgument(program) + L" " + std::wstring(arguments);
        STARTUPINFOW startup = { .cb = sizeof(startup) };
        PROCESS_INFORMATION process = {};
        if (!CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, FALSE, flags, nullptr,
                folder.empty() ? nullptr : folder.c_str(), &startup, &process))
            return false;
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return true;
    }
}
