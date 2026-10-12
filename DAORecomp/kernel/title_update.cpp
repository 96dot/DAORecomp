#include "stdafx.h"
#include "kernel/title_update.h"
#include "hash.h"

namespace fs = std::filesystem;

namespace dao::kernel
{
    namespace
    {
        // Title Update 6's and 7's default.xexp (installer/deps.py TU6_XEXP_SHA1,
        // TU7_XEXP_SHA1).
        constexpr std::string_view kTu6XexpSha1 = "4141B608E44C4ED35EB4E24BA2C05844A2F57A54";
        constexpr std::string_view kTu7XexpSha1 = "DD5B493F4FE2FB4C99E6EF7ED6923DDDDEEE8291";

        int NumberOf(std::string_view sha1)
        {
            return sha1 == kTu6XexpSha1 ? 6 : sha1 == kTu7XexpSha1 ? 7 : 0;
        }
    }

    fs::path UpdateRoot(const fs::path& gameRoot)
    {
        // Not update/: the launcher's updater empties that at every start.
        return gameRoot.parent_path() / "title_update";
    }

    int TitleUpdateNumber(const fs::path& xexp)
    {
        return NumberOf(FileSha1(xexp));
    }

    int TitleUpdateNumber(std::span<const uint8_t> xexp)
    {
        return NumberOf(Sha1(xexp));
    }
}
