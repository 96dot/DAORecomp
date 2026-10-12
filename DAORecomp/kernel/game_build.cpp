#include "stdafx.h"
#include "game_build.h"
#include "text.h"

namespace fs = std::filesystem;

namespace dao::kernel
{

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
