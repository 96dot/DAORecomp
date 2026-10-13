#include "stdafx.h"
#include "launcher/install_runner.h"

#include <bcrypt.h>

#include <rapidjson/document.h>

#include "hash.h"
#include "program.h"
#include "text.h"

namespace fs = std::filesystem;

namespace dao::launcher
{
    namespace
    {
        void Close(void*& handle)
        {
            if (handle && handle != INVALID_HANDLE_VALUE)
                CloseHandle(handle);
            handle = nullptr;
        }

        // Where the appended installer is unpacked, in the work folder, and what to say
        // when its files went missing.
        constexpr std::wstring_view kEngineFolder = L"installer-engine";
        constexpr std::string_view kAntivirusHint =
            ". If antivirus removed it, Windows Security's Protection history can restore it (it's python.org's "
            "Python, signed by the Python Software Foundation)";

        // The footer after the appended installer (installer/pipeline.py ENGINE_MAGIC).
        constexpr std::string_view kEngineMagic = "DAORecompEngine2";
        struct EngineFooter
        {
            char magic[16];
            uint64_t offset, size;
            uint8_t sha256[32];
        };
        static_assert(sizeof(EngineFooter) == 64);

        // This program's file, opened once, at the first install: when it's the
        // install's DAORecomp.exe, the installer renames it (to DAORecomp.old.exe) and
        // puts the game's there, before a retry needs the installer again.
        HANDLE Self()
        {
            static const HANDLE self = CreateFileW(ProgramPath().c_str(), GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            return self;
        }

        bool ReadAt(HANDLE file, uint64_t offset, void* data, DWORD size, DWORD& read)
        {
            OVERLAPPED at = {};
            at.Offset = DWORD(offset);
            at.OffsetHigh = DWORD(offset >> 32);
            read = 0;
            return ReadFile(file, data, size, &read, &at) != 0;
        }

        // Opened so others may only read it (until the installer has ended), and its
        // SHA-256; nullptr (with the Windows error) if it can't be.
        HANDLE OpenChecked(const fs::path& path, std::array<uint8_t, 32>& digest)
        {
            const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
                return nullptr;
            uint64_t size = 0;
            if (HashFile(file, BCRYPT_SHA256_ALGORITHM, digest, size))
                return file;
            const DWORD error = GetLastError();
            CloseHandle(file);
            SetLastError(error);
            return nullptr;
        }

        std::string WindowsError(DWORD code)
        {
            std::string text = std::system_category().message(int(code));
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == '.'))
                text.pop_back();
            return fmt::format("{} (error {})", text, code);
        }

        // Errors from a file just written while antivirus scans it (holding it for a
        // moment), and those too as when antivirus removed it.
        bool Transient(DWORD code)
        {
            return code == ERROR_SHARING_VIOLATION || code == ERROR_ACCESS_DENIED || code == ERROR_LOCK_VIOLATION;
        }
        bool HeldOrRemoved(DWORD code)
        {
            return Transient(code) || code == ERROR_FILE_NOT_FOUND || code == ERROR_VIRUS_INFECTED ||
                   code == ERROR_VIRUS_DELETED;
        }

        // How many times, and how far apart, an unpacked file held by antivirus is
        // tried again (2 s in all).
        constexpr int kHeldRetries = 20;
        constexpr DWORD kHeldRetryMs = 100;
        // The most read or written at once.
        constexpr size_t kChunk = 1 << 24;

        // A file name in the appended installer (installer/pipeline.py ENGINE_NAME): all
        // its files are in one folder, with plain names (the longest is about 20).
        constexpr size_t kMaxEngineName = 64;
        bool PlainEngineName(std::string_view name)
        {
            if (name.empty() || name.size() > kMaxEngineName || name.front() == '.' || name.back() == '.')
                return false;
            if (!std::ranges::all_of(name, [](char c) { return std::isalnum(uint8_t(c)) || c == '.' || c == '_' || c == '-'; }))
                return false;
            // Not a device's name (con, nul, com1...), which Windows opens whatever follows.
            const std::string stem = Lower(std::string(name.substr(0, name.find('.'))));
            static constexpr std::string_view kDevices[] = {"con", "prn", "aux", "nul"};
            const bool numbered = stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt")) &&
                                  std::isdigit(uint8_t(stem[3]));
            return !numbered && std::ranges::find(kDevices, stem) == std::end(kDevices);
        }

        struct EngineFile
        {
            std::string name;
            std::span<const uint8_t> data;
            std::array<uint8_t, 32> sha256;
        };

        // The appended installer's files (installer/pipeline.py append_engine); false if
        // it isn't one.
        bool ParseEngine(std::span<const uint8_t> payload, std::vector<EngineFile>& files)
        {
            for (size_t at = 0; at < payload.size();)
            {
                EngineFile file;
                uint32_t nameSize = 0;
                uint64_t size = 0;
                if (payload.size() - at < sizeof(nameSize))
                    return false;
                std::memcpy(&nameSize, payload.data() + at, sizeof(nameSize));
                at += sizeof(nameSize);
                if (payload.size() - at < uint64_t(nameSize) + sizeof(size) + file.sha256.size())
                    return false;
                file.name.assign(reinterpret_cast<const char*>(payload.data() + at), nameSize);
                at += nameSize;
                std::memcpy(&size, payload.data() + at, sizeof(size));
                at += sizeof(size);
                std::memcpy(file.sha256.data(), payload.data() + at, file.sha256.size());
                at += file.sha256.size();
                if (size > payload.size() - at || !PlainEngineName(file.name))
                    return false;
                file.data = payload.subspan(at, size_t(size));
                at += size_t(size);
                files.push_back(std::move(file));
            }
            return !files.empty();
        }

        // Writes `file` to `path` (as <path>.part-<process>, renamed once complete) and
        // opens it checked into `lock`. Empty, or what went wrong (`antivirus`: as when
        // antivirus takes the file away).
        std::string Unpack(const EngineFile& file, const fs::path& path, HANDLE& lock, bool& antivirus)
        {
            std::error_code ec;
            fs::create_directories(path.parent_path(), ec);
            const fs::path part = fs::path(path).concat(L".part-" + std::to_wstring(GetCurrentProcessId()));
            const HANDLE out = CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL, nullptr);
            if (out == INVALID_HANDLE_VALUE)
                return fmt::format("can't create {}: {}", Utf8(part), WindowsError(GetLastError()));
            bool written = true;
            DWORD error = 0;
            for (size_t at = 0, chunk = 0; written && at < file.data.size(); at += chunk)
            {
                DWORD wrote = 0;
                chunk = std::min(file.data.size() - at, kChunk);
                written = WriteFile(out, file.data.data() + at, DWORD(chunk), &wrote, nullptr) && wrote == chunk;
                error = written ? 0 : GetLastError();
            }
            CloseHandle(out);
            if (!written)
            {
                DeleteFileW(part.c_str());
                return fmt::format("can't write {}: {}", Utf8(part), WindowsError(error));
            }
            // Antivirus scanning the new file may hold it for a moment: tried for a while.
            // (A try after the rename went through finds no .part, and opens the file.)
            std::array<uint8_t, 32> digest = {};
            for (int attempt = 0;; attempt++)
            {
                error = 0;
                if (GetFileAttributesW(part.c_str()) != INVALID_FILE_ATTRIBUTES &&
                    !MoveFileExW(part.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
                    error = GetLastError();
                else if (!(lock = OpenChecked(path, digest)))
                    error = GetLastError() ? GetLastError() : ERROR_GEN_FAILURE;
                if (!error || !Transient(error) || attempt == kHeldRetries)
                    break;
                Sleep(kHeldRetryMs);
            }
            if (error)
            {
                DeleteFileW(part.c_str());
                antivirus = HeldOrRemoved(error);
                return fmt::format("{} can't be opened once unpacked: {}", file.name, WindowsError(error));
            }
            if (digest != file.sha256)
                return fmt::format("{} changed once unpacked", file.name);
            return {};
        }

        // Earlier versions' unpacked installers, not to pile up: the other folders in
        // `engines` (one in use stays: its files are open), and the single programs
        // releases before v0.7.1 unpacked into the work folder's installer/ (which also
        // holds the source copy's installer package: only those programs go).
        void RemoveOldInstallers(const fs::path& engines, const fs::path& current)
        {
            std::error_code ec;
            for (fs::directory_iterator it(engines, ec), end; !ec && it != end; it.increment(ec))
            {
                std::error_code removeError;
                if (it->path() != current && it->symlink_status(removeError).type() == fs::file_type::directory)
                    fs::remove_all(it->path(), removeError);
            }
            for (fs::directory_iterator it(InstallerWorkFolder() / "installer", ec), end; !ec && it != end;
                 it.increment(ec))
            {
                std::error_code removeError;
                if (it->path().filename().wstring().starts_with(L"DAORecomp-Installer-") &&
                    it->symlink_status(removeError).type() == fs::file_type::regular)
                    fs::remove(it->path(), removeError);
            }
        }

        // Whether `path` is a folder itself, not a link to one.
        bool PlainFolder(const fs::path& path)
        {
            std::error_code ec;
            return fs::symlink_status(path, ec).type() == fs::file_type::directory;
        }

        // What's in the installer's folder besides its files (put there since), removed:
        // the ._pth file has Python import from it. False if something can't be.
        bool RemoveStrays(const fs::path& folder, const std::vector<EngineFile>& files)
        {
            std::error_code ec;
            bool removed = true;
            for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
            {
                const std::string name = Utf8(it->path().filename());
                const bool known = std::ranges::any_of(files, [&](const EngineFile& file) {
                    return Lower(file.name) == Lower(name);
                });
                std::error_code removeError;
                const fs::file_type type = it->symlink_status(removeError).type();
                if (known && type == fs::file_type::regular)
                    continue;
                // (A folder's links are removed, not followed.)
                if (type == fs::file_type::directory)
                    fs::remove_all(it->path(), removeError);
                else
                    fs::remove(it->path(), removeError);
                if (removeError)
                {
                    LOGE("Install: can't remove {} from the installer's folder: {}", Utf8(it->path()),
                        removeError.message());
                    removed = false;
                }
            }
            return removed && !ec;
        }

        // The installer appended to this program (installer/pipeline.py append_engine):
        // python.org's embeddable Python and installer.pyz, unpacked (once) into a
        // folder next to the build tools named after them, and checked by their SHA-256
        // every time, as the folder is the player's. Each stays open (`locks`: others
        // may only read it) until the installer has ended. The folder, or empty with
        // `error`.
        fs::path EmbeddedInstaller(std::string& error, std::vector<HANDLE>& locks)
        {
            const HANDLE self = Self();
            LARGE_INTEGER selfSize = {};
            EngineFooter footer = {};
            DWORD read = 0;
            constexpr uint64_t kFooterSize = sizeof(footer);
            if (self == INVALID_HANDLE_VALUE || !GetFileSizeEx(self, &selfSize) ||
                uint64_t(selfSize.QuadPart) < kFooterSize ||
                !ReadAt(self, uint64_t(selfSize.QuadPart) - kFooterSize, &footer, DWORD(kFooterSize), read) ||
                read != kFooterSize || std::string_view(footer.magic, sizeof(footer.magic)) != kEngineMagic ||
                footer.offset > uint64_t(selfSize.QuadPart) - kFooterSize ||
                footer.size > uint64_t(selfSize.QuadPart) - kFooterSize - footer.offset)
            {
                LOGE("Install: no installer appended to {} (DAO_INSTALL_ENGINE names one in development)",
                    Utf8(ProgramPath()));
                error = "this copy of DAORecomp.exe is incomplete: download it again";
                return {};
            }
            // Read and checked whole (a damaged download).
            std::vector<uint8_t> payload(size_t(footer.size));
            for (uint64_t at = 0; at < footer.size; at += read)
            {
                const DWORD chunk = DWORD(std::min<uint64_t>(footer.size - at, kChunk));
                if (!ReadAt(self, footer.offset + at, payload.data() + at, chunk, read) || !read)
                {
                    payload.clear();
                    break;
                }
            }
            std::array<uint8_t, 32> digest;
            std::vector<EngineFile> files;
            if (payload.size() != footer.size || !Sha256(payload, digest) ||
                !std::equal(digest.begin(), digest.end(), std::begin(footer.sha256)) || !ParseEngine(payload, files))
            {
                LOGE("Install: the installer appended to {} is damaged", Utf8(ProgramPath()));
                error = "this copy of DAORecomp.exe is damaged: download it again";
                return {};
            }

            const fs::path engines = InstallerWorkFolder() / kEngineFolder;
            const fs::path folder = engines / fmt::format("{:02x}{:02x}{:02x}{:02x}", footer.sha256[0],
                footer.sha256[1], footer.sha256[2], footer.sha256[3]);
            // One launcher at a time unpacks and cleans up (another's cleanup would take
            // the folder being unpacked).
            const HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\DAORecompInstallerEngine");
            const DWORD waited = mutex ? WaitForSingleObject(mutex, 60000) : WAIT_FAILED;
            const auto release = [&] {
                if (waited == WAIT_OBJECT_0 || waited == WAIT_ABANDONED)
                    ReleaseMutex(mutex);
                if (mutex)
                    CloseHandle(mutex);
            };
            RemoveOldInstallers(engines, folder);
            std::error_code ec;
            fs::create_directories(folder, ec);
            // Its folders are real ones, not links (the files checked are the ones run).
            if (!PlainFolder(engines) || !PlainFolder(folder) || !RemoveStrays(folder, files))
            {
                release();
                LOGE("Install: {} isn't a plain folder of the installer's files", Utf8(folder));
                error = fmt::format("can't unpack the installer into {}: something else is there (remove the "
                                    "folder and try again)", Utf8(folder));
                return {};
            }
            size_t unpacked = 0;
            for (const EngineFile& file : files)
            {
                const fs::path path = folder / FromUtf8(file.name);
                HANDLE lock = OpenChecked(path, digest);
                if (lock && digest == file.sha256)
                {
                    locks.push_back(lock);
                    continue;
                }
                Close(lock);
                bool antivirus = false;
                const std::string problem = Unpack(file, path, lock, antivirus);
                if (lock)
                    locks.push_back(lock);
                if (!problem.empty())
                {
                    release();
                    LOGE("Install: can't unpack the installer into {}: {}", Utf8(folder), problem);
                    error = fmt::format("can't unpack the installer into {}: {}", Utf8(folder), problem);
                    if (antivirus)
                        error += kAntivirusHint;
                    return {};
                }
                unpacked++;
            }
            release();
            if (unpacked)
                LOGI("Install: unpacked the installer to {} ({} of {} files)", Utf8(folder), unpacked, files.size());
            return folder;
        }

        // How to run the installer: DAO_INSTALL_ENGINE (an executable, or install.py
        // run with Python, for development), else the one appended to this program
        // (scripts/build_setup_exe.py), unpacked to the work folder and `locks`ed.
        // Empty, with `error`, if there's none.
        std::wstring InstallerCommand(std::string& error, std::vector<HANDLE>& locks)
        {
            if (const wchar_t* engine = _wgetenv(L"DAO_INSTALL_ENGINE"); engine && *engine)
            {
                const fs::path path(engine);
                // A script: the Python on PATH.
                if (Lower(Utf8(path.extension())) == ".py")
                    return L"python " + QuoteArgument(path);
                return QuoteArgument(path);
            }
            const fs::path engine = EmbeddedInstaller(error, locks);
            return engine.empty() ? std::wstring()
                                  : QuoteArgument(engine / "python.exe") + L" " + QuoteArgument(engine / "installer.pyz");
        }
    }

    fs::path InstallerWorkFolder()
    {
        if (const wchar_t* work = _wgetenv(L"DAO_WORK_DIR"); work && *work)
            return fs::absolute(fs::path(work));
        const wchar_t* local = _wgetenv(L"LOCALAPPDATA");
        return local && *local ? fs::path(local) / "DAORecomp-build" : fs::temp_directory_path() / "DAORecomp-build";
    }

    InstallRunner::~InstallRunner()
    {
        if (state_ == State::Running)
            Cancel();
        if (reader_.joinable())
            reader_.join();
        Close(output_);
        Close(process_);
        Close(job_);
        ReleaseEngine();
    }

    void InstallRunner::ReleaseEngine()
    {
        for (void*& lock : engineLocks_)
            Close(lock);
        engineLocks_.clear();
    }

    bool InstallRunner::Start(const InstallRequest& request, std::string& error)
    {
        if (state_ == State::Running)
            return false;
        if (reader_.joinable())
            reader_.join();
        Close(output_);
        Close(process_);
        Close(job_);
        ReleaseEngine();
        status_.clear();
        error_.clear();
        progress_ = -1.0f;
        log_.clear();
        installed_.clear();
        pending_.clear();
        ended_ = false;

        // A quote in a path would end its argument early (no Windows path has one).
        for (const fs::path* path : { &request.dest, &request.iso, &request.titleUpdate })
        {
            if (path->native().find(L'"') != std::wstring::npos)
            {
                error = fmt::format("{} isn't a valid path", Utf8(*path));
                return false;
            }
        }
        std::wstring command = InstallerCommand(error, engineLocks_);
        if (command.empty())
        {
            ReleaseEngine();
            return false;
        }
        command += L" --progress-json --dest " + QuoteArgument(request.dest);
        if (!request.iso.empty())
            command += L" --iso " + QuoteArgument(request.iso);
        if (!request.titleUpdate.empty())
            command += L" --title-update " + QuoteArgument(request.titleUpdate);
        if (request.acceptLicense)
            command += L" --accept-msvc-license";
        if (request.shortcuts)
            command += L" --shortcuts";

        // Its output (stdout and stderr) through a pipe; in a job, so it and the
        // build tools it starts end with the launcher or Cancel.
        SECURITY_ATTRIBUTES inherit = { .nLength = sizeof(inherit), .bInheritHandle = TRUE };
        HANDLE readEnd = nullptr, writeEnd = nullptr;
        if (!CreatePipe(&readEnd, &writeEnd, &inherit, 0))
        {
            error = fmt::format("can't start the installer (error {})", GetLastError());
            ReleaseEngine();
            return false;
        }
        SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
        output_ = readEnd;
        job_ = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        STARTUPINFOW startup = { .cb = sizeof(startup) };
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = INVALID_HANDLE_VALUE;
        startup.hStdOutput = writeEnd;
        startup.hStdError = writeEnd;
        PROCESS_INFORMATION info = {};
        // In the work folder: not where setup was started from (Downloads), whose
        // files (a git.exe) the installer's tools would find first.
        const fs::path work = InstallerWorkFolder();
        std::error_code ec;
        fs::create_directories(work, ec);
        const bool started = job_ &&
            SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) &&
            CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                nullptr, work.c_str(), &startup, &info);
        const DWORD startError = GetLastError();
        CloseHandle(writeEnd);
        if (!started)
        {
            error = fmt::format("can't start the installer: {}", WindowsError(startError));
            // (The unpacked installer's python.exe taken away since it was checked.)
            if (!engineLocks_.empty() && HeldOrRemoved(startError))
                error += kAntivirusHint;
            ReleaseEngine();
            return false;
        }
        process_ = info.hProcess;
        if (!AssignProcessToJobObject(job_, process_))
        {
            error = fmt::format("can't start the installer (error {})", GetLastError());
            TerminateProcess(process_, 1);
            CloseHandle(info.hThread);
            ReleaseEngine();
            return false;
        }
        ResumeThread(info.hThread);
        CloseHandle(info.hThread);
        LOGI("Install: started {}", Utf8(command));

        state_ = State::Running;
        reader_ = std::thread([this] { Read(); });
        return true;
    }

    void InstallRunner::Read()
    {
        std::string buffer;
        char chunk[4096];
        DWORD read = 0;
        while (ReadFile(output_, chunk, sizeof(chunk), &read, nullptr) && read)
        {
            buffer.append(chunk, read);
            std::vector<std::string> lines;
            for (size_t end; (end = buffer.find('\n')) != buffer.npos; buffer.erase(0, end + 1))
            {
                std::string line = buffer.substr(0, end);
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                lines.push_back(std::move(line));
            }
            std::lock_guard lock(mutex_);
            for (std::string& line : lines)
                pending_.push_back(std::move(line));
        }
        // The pipe closes when the installer and everything it started have ended.
        WaitForSingleObject(process_, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(process_, &code);
        std::lock_guard lock(mutex_);
        if (!buffer.empty())
            pending_.push_back(std::move(buffer));
        exitCode_ = code;
        ended_ = true;
    }

    void InstallRunner::Take(std::string_view line)
    {
        rapidjson::Document item;
        item.Parse(line.data(), line.size());
        // Anything else (a crash's traceback) is shown as it is.
        if (item.HasParseError() || !item.IsObject() || item.MemberCount() != 1)
        {
            if (!line.empty())
                log_.emplace_back(line);
        }
        else
        {
            const auto& [name, value] = *item.MemberBegin();
            const std::string_view key(name.GetString(), name.GetStringLength());
            const std::string text = value.IsString() ? std::string(value.GetString(), value.GetStringLength()) : "";
            if (key == "progress")
                progress_ = value.IsNumber() ? std::clamp(float(value.GetDouble()), 0.0f, 1.0f) : -1.0f;
            else if (key == "status")
            {
                status_ = text;
                log_.push_back("== " + text);
            }
            else if (key == "log")
                log_.push_back(text);
            else if (key == "error")
                error_ = text;
            else if (key == "done")
                installed_ = FromUtf8(text);
        }
        while (log_.size() > kMaxLogLines)
            log_.pop_front();
    }

    void InstallRunner::Poll()
    {
        if (state_ != State::Running)
            return;
        std::vector<std::string> lines;
        bool ended = false;
        DWORD code = 0;
        {
            std::lock_guard lock(mutex_);
            lines.swap(pending_);
            ended = ended_;
            code = exitCode_;
        }
        for (const std::string& line : lines)
            Take(line);
        if (!ended)
            return;
        ReleaseEngine();
        if (code == 0 && !installed_.empty())
        {
            state_ = State::Done;
            LOGI("Install: done, {}", Utf8(installed_));
            return;
        }
        state_ = State::Failed;
        if (error_.empty())
            error_ = fmt::format("The installer stopped (exit code {}).", code);
        LOGW("Install: failed: {}", error_);
    }

    void InstallRunner::Cancel()
    {
        if (state_ != State::Running)
            return;
        TerminateJobObject(job_, 1);
        if (reader_.joinable())
            reader_.join();
        ReleaseEngine();
        state_ = State::Failed;
        error_ = "Stopped. Installing again resumes where it stopped.";
        LOGI("Install: stopped");
    }
}
