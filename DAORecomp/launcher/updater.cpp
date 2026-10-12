#include "stdafx.h"
#include "launcher/updater.h"

#include <bcrypt.h>
#include <shellapi.h>
#include <winhttp.h>

#include <charconv>

#include <rapidjson/document.h>

#include "handle.h"
#include "hash.h"
#include "program.h"
#include "version.h"

namespace dao::launcher
{
    namespace fs = std::filesystem;

    namespace
    {
        // The releases of github.com/96dot/DAORecomp (the 20 newest: an update is
        // always among them). Release pages and installers anywhere else in the
        // list are ignored.
        constexpr const char* kReleasesUrl = "https://api.github.com/repos/96dot/DAORecomp/releases?per_page=20";
        constexpr std::string_view kPagePrefix = "https://github.com/96dot/DAORecomp/releases/";
        constexpr std::string_view kDownloadPrefix = "https://github.com/96dot/DAORecomp/releases/download/";
        // The exe players download (setup, scripts/build_setup_exe.py): it updates
        // the install it's given. Releases also carry it as DAORecomp-Installer.exe,
        // the name launchers up to v0.5.1 look for.
        constexpr std::string_view kInstallerName = "DAORecomp.exe";
        constexpr std::string_view kDigestPrefix = "sha256:";
        // Sanity bounds on what's downloaded: the list is ~100 KB, the installer
        // ~15 MB (v0.2.0).
        constexpr uint64_t kMaxListSize = 4u << 20;
        constexpr uint64_t kMaxInstallerSize = 256u << 20;
        // The notes shown in the launcher are cut to about this long.
        constexpr size_t kMaxNotes = 16 * 1024;
        // Where in the install folder the installer is downloaded to and run from
        // (a folder of its own: Windows looks for the programs it starts in the
        // folder of the one starting them first). Emptied at the next start.
        constexpr const wchar_t* kUpdateFolder = L"update";

        std::wstring Widen(std::string_view text)
        {
            const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
            std::wstring out(size_t(std::max(length, 0)), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), out.data(), length);
            return out;
        }

        std::string NetworkError(DWORD code)
        {
            switch (code)
            {
            case ERROR_WINHTTP_NAME_NOT_RESOLVED:
            case ERROR_WINHTTP_CANNOT_CONNECT:
                return "can't reach GitHub (are you online?)";
            case ERROR_WINHTTP_TIMEOUT:
                return "GitHub didn't answer in time";
            case ERROR_WINHTTP_SECURE_FAILURE:
                return "the secure connection to GitHub failed";
            default:
                return fmt::format("network error {}", code);
            }
        }

        struct CloseInternet
        {
            void operator()(HINTERNET handle) const { WinHttpCloseHandle(handle); }
        };
        using Internet = std::unique_ptr<void, CloseInternet>;

        Handle OpenFile(const fs::path& path, DWORD access, DWORD share, DWORD creation)
        {
            const HANDLE handle =
                CreateFileW(path.c_str(), access, share, nullptr, creation, FILE_ATTRIBUTE_NORMAL, nullptr);
            return Handle(handle == INVALID_HANDLE_VALUE ? nullptr : handle);
        }

        // GETs an https URL (following redirects to https only), giving the body to
        // `sink` in pieces. False, with `error`, if it fails, the answer isn't 200
        // OK, it's longer than `maxSize`, `sink` returns false or `cancel` is set.
        bool HttpGet(std::string_view url, const wchar_t* headers, uint64_t maxSize, const std::atomic<bool>& cancel,
            const std::function<bool(std::span<const uint8_t>)>& sink, std::string& error)
        {
            const std::wstring wide = Widen(url);
            URL_COMPONENTS parts = {};
            parts.dwStructSize = sizeof(parts);
            parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = DWORD(-1);
            if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS)
            {
                error = "not an https address";
                return false;
            }
            const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
            const std::wstring path = std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength) +
                std::wstring(parts.lpszExtraInfo, parts.dwExtraInfoLength);

            const std::wstring agent = L"DAORecomp-launcher/" + Widen(Version());
            Internet session(WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                WINHTTP_NO_PROXY_BYPASS, 0));
            if (!session) // Before Windows 8.1, and some Wine versions.
                session.reset(WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                    WINHTTP_NO_PROXY_BYPASS, 0));
            if (!session)
            {
                error = NetworkError(GetLastError());
                return false;
            }
            // Name lookup, connecting and sending: 15 s each; waiting for each piece of
            // the answer: 30 s.
            WinHttpSetTimeouts(session.get(), 15000, 15000, 15000, 30000);
            Internet connection(WinHttpConnect(session.get(), host.c_str(), parts.nPort, 0));
            Internet request(connection ? WinHttpOpenRequest(connection.get(), L"GET", path.c_str(), nullptr,
                                              WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                        : nullptr);
            if (!request ||
                !WinHttpSendRequest(request.get(), headers ? headers : WINHTTP_NO_ADDITIONAL_HEADERS,
                    headers ? DWORD(-1L) : 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
                !WinHttpReceiveResponse(request.get(), nullptr))
            {
                error = NetworkError(GetLastError());
                return false;
            }
            DWORD status = 0, statusSize = sizeof(status);
            WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
            if (status != 200)
            {
                error = fmt::format("GitHub answered {}", status);
                return false;
            }

            std::vector<uint8_t> buffer(64 * 1024);
            uint64_t total = 0;
            for (;;)
            {
                if (cancel)
                {
                    error = "cancelled";
                    return false;
                }
                DWORD read = 0;
                if (!WinHttpReadData(request.get(), buffer.data(), DWORD(buffer.size()), &read))
                {
                    error = NetworkError(GetLastError());
                    return false;
                }
                if (read == 0)
                    return true;
                total += read;
                if (total > maxSize)
                {
                    error = "the download is larger than expected";
                    return false;
                }
                if (!sink(std::span<const uint8_t>(buffer.data(), read)))
                {
                    error = "can't save the download";
                    return false;
                }
            }
        }

        // The SHA-256 of an open file, read from its start, and its size.
        std::optional<std::array<uint8_t, 32>> FileSha256(HANDLE file, uint64_t& size)
        {
            std::array<uint8_t, 32> digest;
            if (!HashFile(file, BCRYPT_SHA256_ALGORITHM, digest, size))
                return std::nullopt;
            return digest;
        }

        std::optional<std::array<uint8_t, 32>> ParseSha256(std::string_view digest)
        {
            if (!digest.starts_with(kDigestPrefix))
                return std::nullopt;
            digest.remove_prefix(kDigestPrefix.size());
            std::array<uint8_t, 32> out;
            if (digest.size() != out.size() * 2)
                return std::nullopt;
            for (size_t i = 0; i < out.size(); i++)
            {
                const auto [end, ec] = std::from_chars(digest.data() + i * 2, digest.data() + i * 2 + 2, out[i], 16);
                if (ec != std::errc() || end != digest.data() + i * 2 + 2)
                    return std::nullopt;
            }
            return out;
        }

        std::string_view String(const rapidjson::Value& object, const char* key)
        {
            const auto it = object.FindMember(key);
            if (it == object.MemberEnd() || !it->value.IsString())
                return {};
            return std::string_view(it->value.GetString(), it->value.GetStringLength());
        }

        // Whether `key` is present and anything but false.
        bool Flag(const rapidjson::Value& object, const char* key)
        {
            const auto it = object.FindMember(key);
            return it != object.MemberEnd() && !(it->value.IsBool() && !it->value.GetBool());
        }

        // A release's notes for the launcher: without carriage returns, cut at a
        // line end (or a whole character) if very long.
        std::string Notes(std::string_view body)
        {
            std::string notes;
            for (const char c : body.substr(0, kMaxNotes))
            {
                if (c != '\r')
                    notes += c;
            }
            if (body.size() > kMaxNotes)
            {
                if (const size_t line = notes.rfind('\n'); line != std::string::npos)
                    notes.resize(line + 1);
                else
                {
                    while (!notes.empty() && (uint8_t(notes.back()) & 0xC0) == 0x80)
                        notes.pop_back();
                    if (!notes.empty() && uint8_t(notes.back()) >= 0xC0)
                        notes.pop_back();
                    notes += '\n';
                }
                notes += "...\n";
            }
            return notes;
        }
    }

    std::optional<std::array<int, 3>> ParseVersion(std::string_view tag)
    {
        if (!tag.starts_with('v'))
            return std::nullopt;
        std::array<int, 3> version;
        const char* at = tag.data() + 1;
        const char* end = tag.data() + tag.size();
        for (size_t i = 0; i < version.size(); i++)
        {
            if (i > 0 && (at == end || *at++ != '.'))
                return std::nullopt;
            // Digits only (from_chars would take a sign).
            if (at == end || *at < '0' || *at > '9')
                return std::nullopt;
            const auto [next, ec] = std::from_chars(at, end, version[i]);
            if (ec != std::errc())
                return std::nullopt;
            at = next;
        }
        if (at != end)
            return std::nullopt;
        return version;
    }

    bool FindUpdate(std::string_view json, std::string_view current, std::optional<Release>& update)
    {
        update.reset();
        const auto currentVersion = ParseVersion(current);
        rapidjson::Document document;
        // Iterative: deeply nested input can't exhaust the stack.
        document.Parse<rapidjson::kParseIterativeFlag>(json.data(), json.size());
        if (!currentVersion || document.HasParseError() || !document.IsArray())
            return false;

        // Pre-releases are offered while this is one too (0.x).
        const bool takePrereleases = (*currentVersion)[0] == 0;
        std::array<int, 3> newest = *currentVersion;
        for (const rapidjson::Value& item : document.GetArray())
        {
            if (!item.IsObject() || Flag(item, "draft") || (!takePrereleases && Flag(item, "prerelease")))
                continue;
            const auto version = ParseVersion(String(item, "tag_name"));
            const std::string_view page = String(item, "html_url");
            const auto assets = item.FindMember("assets");
            if (!version || *version <= newest || !page.starts_with(kPagePrefix) || assets == item.MemberEnd() ||
                !assets->value.IsArray())
                continue;
            for (const rapidjson::Value& asset : assets->value.GetArray())
            {
                if (!asset.IsObject() || String(asset, "name") != kInstallerName)
                    continue;
                const std::string_view url = String(asset, "browser_download_url");
                const auto size = asset.FindMember("size");
                const auto sha256 = ParseSha256(String(asset, "digest"));
                if (!url.starts_with(kDownloadPrefix) || size == asset.MemberEnd() || !size->value.IsUint64() ||
                    size->value.GetUint64() == 0 || size->value.GetUint64() > kMaxInstallerSize || !sha256)
                    continue;
                Release release;
                release.tag = String(item, "tag_name");
                release.name = String(item, "name");
                if (release.name.empty())
                    release.name = release.tag;
                release.notes = Notes(String(item, "body"));
                release.pageUrl = page;
                release.installerUrl = url;
                release.installerSize = size->value.GetUint64();
                release.installerSha256 = *sha256;
                update = std::move(release);
                newest = *version;
                break;
            }
        }
        return true;
    }

    // What the background threads share with the launcher (they keep it alive if
    // the launcher closes first).
    struct Updater::Shared
    {
        std::atomic<State> state = State::Idle;
        std::atomic<bool> cancel = false;
        std::atomic<bool> found = false;
        std::atomic<float> progress = 0.0f;
        std::mutex mutex;
        Release release;
        std::string error;
        fs::path installer;
        // The verified installer, open so it can't be changed or replaced before
        // it runs.
        Handle installerLock;
    };

    Updater::Updater() : shared_(std::make_shared<Shared>()), installRoot_(ProgramPath().parent_path())
    {
        // Updates go into an install made by the installer (its game files are
        // what it rebuilds from), and only on Windows (under Wine/Proton the
        // installer can't run).
        installable_ = !IsUnderWine() && IsInstalledGame(installRoot_);
        // The last update's installer (it may still be closing, then it goes next time).
        std::error_code ec;
        if (installable_)
            fs::remove_all(installRoot_ / kUpdateFolder, ec);
    }

    Updater::~Updater()
    {
        shared_->cancel = true;
    }

    bool Updater::IsRelease()
    {
        return ParseVersion(Version()).has_value();
    }

    void Updater::Check()
    {
        const State state = shared_->state;
        // Once a release is found it stays (a later check couldn't add anything).
        if (!IsRelease() || state == State::Checking || shared_->found)
            return;
        shared_->state = State::Checking;
        std::thread([shared = shared_] {
            std::string list, error;
            bool ok = HttpGet(kReleasesUrl,
                L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n", kMaxListSize,
                shared->cancel,
                [&](std::span<const uint8_t> data) {
                    list.append(reinterpret_cast<const char*>(data.data()), data.size());
                    return true;
                },
                error);
            std::optional<Release> update;
            if (ok && !FindUpdate(list, Version(), update))
            {
                ok = false;
                error = "can't read GitHub's list of releases";
            }
            if (shared->cancel) // The launcher is closing (and maybe the log with it).
                return;
            std::lock_guard lock(shared->mutex);
            if (!ok)
            {
                LOGW("Launcher: update check failed: {}", error);
                shared->error = std::move(error);
                shared->state = State::Failed;
            }
            else if (update)
            {
                LOGI("Launcher: {} is available (this is {})", update->tag, Version());
                shared->release = std::move(*update);
                shared->found = true;
                shared->state = State::Available;
            }
            else
                shared->state = State::UpToDate;
        }).detach();
    }

    void Updater::Download()
    {
        const State state = shared_->state;
        if (!installable_ || !shared_->found || (state != State::Available && state != State::Failed))
            return;
        shared_->state = State::Downloading;
        shared_->progress = 0.0f;
        std::thread([shared = shared_, folder = installRoot_ / kUpdateFolder] {
            const Release release = [&] {
                std::lock_guard lock(shared->mutex);
                return shared->release;
            }();
            const fs::path path = folder / kInstallerName;
            fs::path part = path;
            part += L".part";
            std::string error;
            std::error_code ec;
            fs::create_directories(folder, ec);
            // Removed first, so a link left in its place isn't followed.
            fs::remove(part, ec);
            bool ok;
            {
                // Nobody else may write to it meanwhile.
                Handle file = OpenFile(part, GENERIC_WRITE, FILE_SHARE_READ, CREATE_NEW);
                uint64_t done = 0;
                const auto save = [&](std::span<const uint8_t> data) {
                    done += data.size();
                    shared->progress = float(double(done) / double(release.installerSize));
                    DWORD written = 0;
                    return WriteFile(file.get(), data.data(), DWORD(data.size()), &written, nullptr) &&
                        written == data.size();
                };
                ok = file && HttpGet(release.installerUrl, nullptr, release.installerSize, shared->cancel, save, error);
                if (!file)
                    error = "can't write the installer to the install folder";
            }
            if (ok && !MoveFileExW(part.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
            {
                ok = false;
                error = fmt::format("can't save the installer in the install folder (error {})", GetLastError());
            }
            // Checked as it is on disk, and kept open (others may only read it)
            // until it runs.
            Handle installer;
            if (ok)
            {
                installer = OpenFile(path, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING);
                uint64_t size = 0;
                const auto digest = installer ? FileSha256(installer.get(), size) : std::nullopt;
                if (!digest)
                    error = "can't check the downloaded installer";
                else if (size != release.installerSize)
                    error = "the download is incomplete";
                else if (*digest != release.installerSha256)
                    error = "the download doesn't match its published checksum";
                ok = digest && size == release.installerSize && *digest == release.installerSha256;
                if (!ok)
                {
                    installer.reset();
                    fs::remove(path, ec);
                }
            }
            if (!ok)
                fs::remove(part, ec);
            if (shared->cancel)
                return;

            std::lock_guard lock(shared->mutex);
            if (ok)
            {
                LOGI("Launcher: downloaded the {} installer", release.tag);
                shared->installer = path;
                shared->installerLock = std::move(installer);
                shared->state = State::Ready;
            }
            else
            {
                LOGW("Launcher: the {} installer couldn't be downloaded: {}", release.tag, error);
                shared->error = std::move(error);
                shared->state = State::Failed;
            }
        }).detach();
    }

    bool Updater::Launch()
    {
        std::lock_guard lock(shared_->mutex);
        if (shared_->state != State::Ready)
            return false;
        const fs::path installer = shared_->installer;
        if (!StartProgram(installer, L"--update --dest " + QuoteArgument(installRoot_), 0, installer.parent_path()))
        {
            const DWORD code = GetLastError();
            LOGW("Launcher: can't start the installer (error {})", code);
            shared_->error = fmt::format("can't start the installer (error {})", code);
            shared_->state = State::Failed;
            return false;
        }
        shared_->installerLock.reset();
        LOGI("Launcher: started the installer to update");
        return true;
    }

    void Updater::OpenReleasePage() const
    {
        const std::string url = GetRelease().pageUrl;
        if (url.starts_with(kPagePrefix))
            ShellExecuteW(nullptr, L"open", Widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    Updater::State Updater::GetState() const
    {
        return shared_->state;
    }

    bool Updater::HasRelease() const
    {
        return shared_->found;
    }

    Release Updater::GetRelease() const
    {
        std::lock_guard lock(shared_->mutex);
        return shared_->release;
    }

    std::string Updater::GetError() const
    {
        std::lock_guard lock(shared_->mutex);
        return shared_->error;
    }

    float Updater::GetProgress() const
    {
        return shared_->progress;
    }
}
