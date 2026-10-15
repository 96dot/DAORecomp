#pragma once
// Updates from the launcher: the newest release on GitHub newer than this build,
// and its installer, downloaded into the install folder (the program's folder),
// checked against the SHA-256 GitHub publishes for it and run on this install (it
// rebuilds the game from the files already extracted and keeps userdata). The
// checksum guards against a damaged or altered download, not against a release
// made by someone who controls the repository.

#include "launcher/release_list.h"

namespace dao::launcher
{
    class Updater
    {
    public:
        enum class State
        {
            Idle,
            Checking,
            UpToDate,
            Available,
            Downloading,
            // The installer is downloaded and verified (see Launch).
            Ready,
            Failed,
        };

        Updater();
        // Stops a check or download (it ends on its own thread, soon after).
        ~Updater();
        Updater(const Updater&) = delete;
        Updater& operator=(const Updater&) = delete;

        // Whether this build has a version to compare (a release or dev release,
        // not a build from source).
        static bool IsRelease();
        // Whether the installer can be run on this install: not under Wine/Proton,
        // and only on an install made by the installer. Otherwise the release page
        // is offered instead.
        bool CanInstall() const { return installable_; }

        // Looks for a newer release on `channel` in the background (not while
        // busy, nor once one is found on the same channel).
        void Check(UpdateChannel channel);
        // Downloads the available release's installer in the background.
        void Download();
        // Starts the downloaded installer on this install; the launcher must then
        // quit so the game's files can be replaced.
        bool Launch();
        // Opens the available release's page in the browser.
        void OpenReleasePage() const;

        State GetState() const;
        // Whether a newer release was found (it stays found, also if its download
        // fails, until a check on another channel).
        bool HasRelease() const;
        // The newer release, from Available on.
        Release GetRelease() const;
        // Why it failed, for Failed.
        std::string GetError() const;
        // How much of the installer is downloaded (0..1).
        float GetProgress() const;

    private:
        struct Shared;
        std::shared_ptr<Shared> shared_;
        std::filesystem::path installRoot_;
        bool installable_ = false;
    };
}
