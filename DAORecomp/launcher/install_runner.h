#pragma once
// Installing from the launcher: runs the installer (the Python build pipeline in
// installer/, as an executable or a script) hidden, with --progress-json, and keeps
// what it reports for the Install view.

namespace dao::launcher
{
    struct InstallRequest
    {
        std::filesystem::path iso;          // Empty to update an install whose game files are extracted.
        std::filesystem::path titleUpdate;  // Optional.
        std::filesystem::path dest;
        bool acceptLicense = false;
        bool shortcuts = false;
    };

    class InstallRunner
    {
    public:
        enum class State
        {
            Idle,
            Running,
            Done,
            Failed,
        };

        InstallRunner() = default;
        InstallRunner(const InstallRunner&) = delete;
        InstallRunner& operator=(const InstallRunner&) = delete;
        // Stops an install under way (running it again resumes it).
        ~InstallRunner();

        // False, with `error`, if the installer can't be started.
        bool Start(const InstallRequest& request, std::string& error);
        // Stops the installer and everything it started.
        void Cancel();
        // Takes what the installer reported since the last call (call once a frame).
        void Poll();

        State GetState() const { return state_; }
        const std::string& Status() const { return status_; }
        // 0..1, or negative while the step's length is unknown.
        float Progress() const { return progress_; }
        // The installer's output, the latest kMaxLogLines lines.
        const std::deque<std::string>& Log() const { return log_; }
        const std::string& Error() const { return error_; }
        // When Done: the installed game's executable.
        const std::filesystem::path& Installed() const { return installed_; }

        static constexpr size_t kMaxLogLines = 400;

    private:
        void Read();
        void Take(std::string_view line);

        State state_ = State::Idle;
        std::string status_, error_;
        float progress_ = -1.0f;
        std::deque<std::string> log_;
        std::filesystem::path installed_;

        // The installer's process and its job (everything it starts is in it), and
        // the thread reading its output into pending_.
        void* job_ = nullptr;
        void* process_ = nullptr;
        void* output_ = nullptr;
        std::thread reader_;
        std::mutex mutex_;
        std::vector<std::string> pending_;
        bool ended_ = false;
        DWORD exitCode_ = 0;
    };

    // Where the installer is: DAO_INSTALL_ENGINE (an executable, or install.py run
    // with Python, for development), else the one built into this program. Empty if
    // there's none.
    std::wstring InstallerCommand(std::string& error);
}
