#include "stdafx.h"
#include "patches/patches.h"
#include "settings.h"

namespace dao::patches
{
    namespace
    {
        constexpr Patch kPatches[] = {
            { "skip_intro_logos", "Skip intro logos",
                "Skips the EA, BioWare and Edge of Reality logo movies at startup.", false },
        };

        constexpr std::string_view kIntroLogos[] = {
            "dragon_age_ea_logo.bik",
            "bwlogo.bik",
            "edge_logo_withaudio.bik",
        };

        bool EndsWithNoCase(std::string_view text, std::string_view suffix)
        {
            if (text.size() < suffix.size())
                return false;
            text = text.substr(text.size() - suffix.size());
            for (size_t i = 0; i < suffix.size(); i++)
            {
                if (tolower(uint8_t(text[i])) != tolower(uint8_t(suffix[i])))
                    return false;
            }
            return true;
        }
    }

    std::span<const Patch> All()
    {
        return kPatches;
    }

    bool Enabled(std::string_view id)
    {
        const auto& saved = GetSettings().patches;
        if (const auto it = saved.find(id); it != saved.end())
            return it->second;
        for (const Patch& patch : kPatches)
        {
            if (patch.id == id)
                return patch.defaultEnabled;
        }
        return false;
    }

    bool IsHiddenFile(std::string_view guestPath)
    {
        if (!EndsWithNoCase(guestPath, ".bik") || !Enabled("skip_intro_logos"))
            return false;
        for (std::string_view logo : kIntroLogos)
        {
            if (EndsWithNoCase(guestPath, logo))
            {
                LOGI("Patch skip_intro_logos: hiding {}", guestPath);
                return true;
            }
        }
        return false;
    }
}
