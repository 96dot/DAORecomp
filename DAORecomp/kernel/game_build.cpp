#include "stdafx.h"
#include "game_build.h"
#include "hash.h"
#include "text.h"

namespace fs = std::filesystem;

namespace dao::kernel
{
    namespace
    {
        // Title Update 6's default.xexp (installer/deps.py TU6_XEXP_SHA1).
        constexpr std::string_view kTu6XexpSha1 = "4141B608E44C4ED35EB4E24BA2C05844A2F57A54";
    }

    fs::path UpdateRoot(const fs::path& gameRoot)
    {
        // Not update/: the launcher's updater empties that at every start.
        return gameRoot.parent_path() / "title_update";
    }

    bool IsTu6Patch(const fs::path& xexp)
    {
        return FileSha1(xexp) == kTu6XexpSha1;
    }

    void SelectBuild(const fs::path& gameRoot)
    {
        std::error_code ec;
        const fs::path xexp = UpdateRoot(gameRoot) / "default.xexp";
        const bool installed = fs::is_regular_file(xexp, ec);
        const char* forced = getenv("DAO_BUILD");
        const bool disc = forced && _stricmp(forced, "disc") == 0;
#if DAO_HAS_TU6
        if (installed && !disc && !IsTu6Patch(xexp))
        {
            LOGW("Game build: {} isn't Title Update 6's; running the disc's", Utf8(xexp));
        }
        else if (installed && !disc)
        {
            g_build = { GameVersion::Tu6, DaoBuildTu6 };
            LOGI("Game build: Title Update 6 ({})", Utf8(UpdateRoot(gameRoot)));
            return;
        }
#else
        if (installed)
            LOGW("Game build: Title Update 6 is installed, but this executable was built without it");
#endif
        g_build = { GameVersion::Disc, DaoBuildDisc };
        LOGI("Game build: disc{}", installed && disc ? " (DAO_BUILD=disc)" : "");
    }
}
