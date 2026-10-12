#include "stdafx.h"
#include "launcher/install_runner.h"

#include <bcrypt.h>

#include <fstream>

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

        // The footer after the appended installer (installer/pipeline.py ENGINE_MAGIC).
        constexpr std::string_view kEngineMagic = "DAORecompEngine1";
        struct EngineFooter
        {
            char magic[16];
            uint64_t offset, size;
            uint8_t sha256[32];
        };
        static_assert(sizeof(EngineFooter) == 64);

        bool Sha256(const fs::path& path, std::array<uint8_t, 32>& digest)
        {
            const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
                return false;
            uint64_t size = 0;
            const bool hashed = HashFile(file, BCRYPT_SHA256_ALGORITHM, digest, size);
            CloseHandle(file);
            return hashed;
        }

        // The installer appended to this program, extracted (once) next to the build
        // tools; checked by its SHA-256 every time, as the folder is the player's.
        fs::path EmbeddedInstaller(std::string& error)
        {
            const fs::path self = ProgramPath();
            std::ifstream in(self, std::ios::binary);
            EngineFooter footer = {};
            std::error_code ec;
            const uintmax_t selfSize = fs::file_size(self, ec);
            if (!in || ec || selfSize < sizeof(footer) ||
                !in.seekg(std::streamoff(selfSize - sizeof(footer))) ||
                !in.read(reinterpret_cast<char*>(&footer), sizeof(footer)) ||
                std::string_view(footer.magic, sizeof(footer.magic)) != kEngineMagic ||
                footer.offset > selfSize || footer.size > selfSize - sizeof(footer) - footer.offset)
            {
                error = "this build has no installer in it (DAO_INSTALL_ENGINE names one)";
                return {};
            }
            std::array<uint8_t, 32> wanted;
            std::copy(std::begin(footer.sha256), std::end(footer.sha256), wanted.begin());
            const fs::path folder = InstallerWorkFolder() / "installer";
            const fs::path engine = folder / fmt::format("DAORecomp-Installer-{:02x}{:02x}{:02x}{:02x}.exe",
                wanted[0], wanted[1], wanted[2], wanted[3]);
            std::array<uint8_t, 32> digest;
            if (Sha256(engine, digest) && digest == wanted)
                return engine;

            fs::create_directories(folder, ec);
            const fs::path part = fs::path(engine).concat(L".part");
            {
                std::ofstream out(part, std::ios::binary | std::ios::trunc);
                in.clear();
                in.seekg(std::streamoff(footer.offset));
                std::vector<char> chunk(1 << 20);
                for (uint64_t left = footer.size; left && out && in;)
                {
                    const size_t n = size_t(std::min<uint64_t>(left, chunk.size()));
                    in.read(chunk.data(), std::streamsize(n));
                    out.write(chunk.data(), std::streamsize(in.gcount()));
                    left -= uint64_t(in.gcount());
                    if (in.gcount() == 0)
                        break;
                }
            }
            if (!Sha256(part, digest) || digest != wanted || (fs::rename(part, engine, ec), ec))
            {
                fs::remove(part, ec);
                error = fmt::format("can't unpack the installer into {}", Utf8(folder));
                return {};
            }
            LOGI("Install: unpacked the installer to {}", Utf8(engine));
            return engine;
        }
    }

    fs::path InstallerWorkFolder()
    {
        const wchar_t* local = _wgetenv(L"LOCALAPPDATA");
        return local && *local ? fs::path(local) / "DAORecomp-build" : fs::temp_directory_path() / "DAORecomp-build";
    }

    std::wstring InstallerCommand(std::string& error)
    {
        if (const wchar_t* engine = _wgetenv(L"DAO_INSTALL_ENGINE"); engine && *engine)
        {
            const fs::path path(engine);
            // A script: the Python on PATH (development only).
            if (Lower(Utf8(path.extension())) == ".py")
                return L"python " + QuoteArgument(path);
            return QuoteArgument(path);
        }
        const fs::path engine = EmbeddedInstaller(error);
        return engine.empty() ? std::wstring() : QuoteArgument(engine);
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
        status_.clear();
        error_.clear();
        progress_ = -1.0f;
        log_.clear();
        installed_.clear();
        pending_.clear();
        ended_ = false;

        std::wstring command = InstallerCommand(error);
        if (command.empty())
            return false;
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
        const bool started = job_ &&
            SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) &&
            CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                nullptr, nullptr, &startup, &info);
        const DWORD startError = GetLastError();
        CloseHandle(writeEnd);
        if (!started)
        {
            error = fmt::format("can't start the installer (error {})", startError);
            return false;
        }
        process_ = info.hProcess;
        if (!AssignProcessToJobObject(job_, process_))
        {
            error = fmt::format("can't start the installer (error {})", GetLastError());
            TerminateProcess(process_, 1);
            CloseHandle(info.hThread);
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
        state_ = State::Failed;
        error_ = "Stopped. Installing again resumes where it stopped.";
        LOGI("Install: stopped");
    }
}
