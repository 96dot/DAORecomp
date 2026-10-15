#pragma once
// The console settings the game reads (XGetLanguage, ExGetXConfigSetting) that come
// from the player's config.toml.
#include "settings.h"

namespace dao::kernel
{
    // The console's language numbers (the Xbox 360 SDK's XC_LANGUAGE_*, as Xenia's
    // XLanguage has them: English 1, Spanish 5, Italian 6), in Language's order. The
    // game picks its text by them.
    inline constexpr uint32_t kConsoleLanguages[] = { 1, 6, 5 };
    static_assert(std::size(kConsoleLanguages) == size_t(kMaxLanguage) + 1);

    // Settings::language as the console's language number.
    inline uint32_t ConsoleLanguage()
    {
        return kConsoleLanguages[size_t(GetSettings().language)];
    }
}
