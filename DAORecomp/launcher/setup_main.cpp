// The launcher-only build's entry point (DAO_LAUNCHER_ONLY): what players download.
// It has no game code; its Install view runs the installer, which recompiles the
// player's game into the full DAORecomp.exe, and that one is started in its place.
#include "stdafx.h"
#include "launcher/install_runner.h"
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
}

int main(int, char**)
{
    // DLLs from System32 only, not from the folder this was downloaded to (where
    // anything may have been put); with the /DEPENDENTLOADFLAG it's linked with,
    // which covers the ones loaded before main.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // --dest <folder> and --update, as the launcher's updater passes them (launchers
    // before this also pass the old installer's --gui, which needs nothing here); --iso <image>, or
    // an .iso file dropped on the exe. The wide command line, as argv is in the ANSI
    // code page.
    fs::path dest, iso;
    bool update = false;
    int count = 0;
    if (LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &count))
    {
        for (int i = 1; i < count; i++)
        {
            if (wcscmp(args[i], L"--dest") == 0 && i + 1 < count)
                dest = args[++i];
            else if (wcscmp(args[i], L"--iso") == 0 && i + 1 < count)
                iso = args[++i];
            else if (wcscmp(args[i], L"--update") == 0)
                update = true;
            else if (dao::Lower(dao::Utf8(fs::path(args[i]).extension())) == ".iso")
                iso = args[i];
        }
        LocalFree(args);
    }
    if (dest.empty())
        dest = DefaultDest();

    // The log goes to the installer's work folder: the install folder may not exist yet.
    std::error_code ec;
    const fs::path work = dao::launcher::InstallerWorkFolder();
    fs::create_directories(work, ec);
    dao::log::Init(work / "setup.log");
    LOGI("Dragon Age: Origins recomp setup - install folder {}", dao::Utf8(dest));

    const dao::launcher::SetupResult setup = dao::launcher::RunSetup(dest, iso, update);
    if (setup.installed.empty())
        return 0;
    // The new game's launcher, shown even if the install's settings skip it; it adds
    // the game to Steam if chosen (it has the game's art for that).
    if (!dao::StartProgram(setup.installed, setup.addToSteam ? L"--launcher --add-to-steam" : L"--launcher", 0,
            setup.installed.parent_path()))
    {
        LOGE("Setup: can't start {} (error {})", dao::Utf8(setup.installed), GetLastError());
        return 1;
    }
    return 0;
}
