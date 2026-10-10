#pragma once
// Built-in code patches the player can toggle (launcher "Patches" tab, [patches]
// in config.toml). Each patch checks Enabled() where it takes effect.

namespace dao::patches
{
    struct Patch
    {
        std::string_view id; // config.toml key
        std::string_view name;
        std::string_view description;
        bool defaultEnabled;
    };

    std::span<const Patch> All();

    // Whether a patch is on in the loaded settings (its default if unset).
    bool Enabled(std::string_view id);

    // skip_intro_logos: the publisher/developer logo movies are reported missing.
    bool IsHiddenFile(std::string_view guestPath);
}
