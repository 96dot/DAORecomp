// The launcher-only build's entry point (DAO_LAUNCHER_ONLY): what players download.
// It has no game code; its Install view runs the installer, which recompiles the
// player's game into the full DAORecomp.exe, and that one is started in its place.
#include "stdafx.h"
#include "launcher/launcher.h"
#include "program.h"
#include "text.h"

#include <shellapi.h>

namespace fs = std::filesystem;

namespace
{
    fs::path EnvPath(const wchar_t* name)
    {
        const wchar_t* value = _wgetenv(name);
        return value && *value ? fs::path(value) : fs::path();
    }

    // Where to install unless told: the folder this program is in when it's an
    // install (to update it), else the old installer's default.
    fs::path DefaultDest()
    {
        std::error_code ec;
        const fs::path here = dao::ProgramPath().parent_path();
        if (!here.empty() && fs::is_regular_file(here / "game" / "default.xex", ec))
            return here;
        const fs::path home = EnvPath(L"USERPROFILE");
        return home.empty() ? fs::path() : home / "Games" / "DAORecomp";
    }

    // The installer's work folder, as installer/util.py has it (the log goes there,
    // as the install folder may not exist yet).
    fs::path WorkFolder()
    {
        const fs::path local = EnvPath(L"LOCALAPPDATA");
        return local.empty() ? fs::temp_directory_path() : local / "DAORecomp-build";
    }
}

int main(int, char**)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // --dest <folder> and --update, as the launcher's updater passes them (it also
    // passes the old installer's --gui, which needs nothing here). The wide command
    // line, as argv is in the ANSI code page.
    fs::path dest;
    bool update = false;
    int count = 0;
    if (LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &count))
    {
        for (int i = 1; i < count; i++)
        {
            if (wcscmp(args[i], L"--dest") == 0 && i + 1 < count)
                dest = args[++i];
            else if (wcscmp(args[i], L"--update") == 0)
                update = true;
        }
        LocalFree(args);
    }
    if (dest.empty())
        dest = DefaultDest();

    std::error_code ec;
    const fs::path work = WorkFolder();
    fs::create_directories(work, ec);
    dao::log::Init(work / "setup.log");
    LOGI("Dragon Age: Origins recomp setup - install folder {}", dao::Utf8(dest));

    const fs::path installed = dao::launcher::RunSetup(dest, update);
    if (installed.empty())
        return 0;
    // The new game's launcher, shown even if the install's settings skip it.
    if (!dao::StartProgram(installed, L"--launcher", 0, installed.parent_path()))
    {
        LOGE("Setup: can't start {} (error {})", dao::Utf8(installed), GetLastError());
        return 1;
    }
    return 0;
}
