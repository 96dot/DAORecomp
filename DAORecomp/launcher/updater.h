#pragma once
// Updates from the launcher: the newest release on GitHub newer than this build,
// and its installer, downloaded into the install folder (the program's folder),
// checked against the SHA-256 GitHub publishes for it and run on this install (it
// rebuilds the game from the files already extracted and keeps userdata). The
// checksum guards against a damaged or altered download, not against a release
// made by someone who controls the repository.

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

        // Whether this build has a version to compare (a release, not "dev").
        static bool IsRelease();
        // Whether the installer can be run on this install: not under Wine/Proton,
        // and only on an install made by the installer. Otherwise the release page
        // is offered instead.
        bool CanInstall() const { return installable_; }

        // Looks for a newer release in the background (not while busy, nor once
        // one is found).
        void Check();
        // Downloads the available release's installer in the background.
        void Download();
        // Starts the downloaded installer on this install; the launcher must then
        // quit so the game's files can be replaced.
        bool Launch();
        // Opens the available release's page in the browser.
        void OpenReleasePage() const;

        State GetState() const;
        // Whether a newer release was found (it stays found, also if its download
        // fails).
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

    // "vX.Y.Z" as numbers; nothing for anything else.
    std::optional<std::array<int, 3>> ParseVersion(std::string_view tag);
    // Sets `update` to the newest release in GitHub's release list (JSON) newer
    // than `current` with a verifiable installer, or nothing if there is none;
    // false if the list can't be read.
    bool FindUpdate(std::string_view json, std::string_view current, std::optional<Release>& update);
}
