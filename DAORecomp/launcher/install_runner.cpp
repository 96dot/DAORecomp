#include "stdafx.h"
#include "launcher/install_runner.h"

#include <rapidjson/document.h>

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
        error = "this build has no installer in it (DAO_INSTALL_ENGINE names one)";
        return {};
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
