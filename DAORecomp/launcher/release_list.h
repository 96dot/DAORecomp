#pragma once
// Which release the launcher offers as an update: GitHub's list of releases read,
// and the newest one newer than this build on its update channel (see
// UpdateChannel in settings.h).

#include <climits>

#include "settings.h"

namespace dao::launcher
{
    struct Release
    {
        std::string tag;
        std::string name;
        // Its notes (Markdown), shortened if very long.
        std::string notes;
        std::string pageUrl;
        std::string installerUrl;
        uint64_t installerSize = 0;
        std::array<uint8_t, 32> installerSha256 = {};
    };

    // Release pages are only opened at this address (and installers only taken
    // from its download/ folder, see release_list.cpp).
    inline constexpr std::string_view kReleasePagePrefix = "https://github.com/96dot/DAORecomp/releases/";
    // The exe players download (setup, scripts/build_setup_exe.py): it updates
    // the install it's given. Releases also carry it as DAORecomp-Installer.exe,
    // the name launchers up to v0.5.1 look for.
    inline constexpr std::string_view kInstallerName = "DAORecomp.exe";

    // A version tag as numbers {X, Y, Z, N}: "vX.Y.Z" (a release, N is
    // kNotDevRelease) or "vX.Y.Z-dev.N" (dev release N on the way to vX.Y.Z,
    // so before it); in that order, compared as an array.
    using VersionNumber = std::array<int, 4>;
    inline constexpr int kNotDevRelease = INT_MAX;
    // Nothing for any other tag (such as "dev", or a "-dirty" build from source).
    // (Settings' DefaultUpdateChannel is defined in release_list.cpp, with this.)
    std::optional<VersionNumber> ParseVersion(std::string_view tag);
    inline bool IsDevRelease(const VersionNumber& version) { return version[3] != kNotDevRelease; }

    // Sets `update` to the newest release in GitHub's release list (JSON) newer
    // than `current` with a verifiable installer, on `channel` (Release: not dev
    // releases), or nothing if there is none; false if the list can't be read.
    bool FindUpdate(std::string_view json, std::string_view current, UpdateChannel channel,
        std::optional<Release>& update);
}
