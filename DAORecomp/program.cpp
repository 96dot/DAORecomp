#include "stdafx.h"
#include "program.h"

#include <tlhelp32.h>

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

    bool OtherInstanceRunning()
    {
        const fs::path self = ProgramPath();
        if (self.empty())
            return false;
        // Its name, long and short (as a process started by its 8.3 path is listed).
        std::wstring shortPath(MAX_PATH, L'\0');
        shortPath.resize(GetShortPathNameW(self.c_str(), shortPath.data(), DWORD(shortPath.size())));
        const std::wstring names[] = { self.filename().native(), fs::path(shortPath).filename().native() };
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        // (Not knowing counts as running: better a refusal than a race with the game.)
        if (snapshot == INVALID_HANDLE_VALUE)
            return true;
        bool found = false;
        PROCESSENTRY32W entry = { .dwSize = sizeof(entry) };
        for (BOOL more = Process32FirstW(snapshot, &entry); more && !found; more = Process32NextW(snapshot, &entry))
        {
            if (entry.th32ProcessID == GetCurrentProcessId() ||
                std::ranges::none_of(names, [&](const std::wstring& name) {
                    return !name.empty() && _wcsicmp(entry.szExeFile, name.c_str()) == 0;
                }))
                continue;
            // The same name: the same program if it's the same file, however its path
            // is spelled (a junction, a subst drive). One that can't be asked counts.
            found = true;
            const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            if (!process)
                continue;
            std::wstring path(32768, L'\0');
            DWORD length = DWORD(path.size());
            if (QueryFullProcessImageNameW(process, 0, path.data(), &length))
            {
                path.resize(length);
                std::error_code ec;
                found = fs::equivalent(path, self, ec) || ec;
            }
            CloseHandle(process);
        }
        CloseHandle(snapshot);
        return found;
    }

    bool IsUnderWine()
    {
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        return ntdll && GetProcAddress(ntdll, "wine_get_version");
    }

    bool IsInstalledGame(const fs::path& installRoot)
    {
        std::error_code ec;
        const fs::path game = installRoot / "game";
        return !installRoot.empty() && fs::is_regular_file(game / "default.xex", ec) &&
            fs::is_regular_file(game / "dao-extracted.txt", ec);
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
