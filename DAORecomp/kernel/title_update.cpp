#include "stdafx.h"
#include "kernel/title_update.h"
#include "hash.h"

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
}
