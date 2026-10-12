#include "stdafx.h"
#include "launcher/steam.h"

#include <tlhelp32.h>

#include <charconv>
#include <fstream>

// Its own copy (static), as the game's (gpu/xenia_host.cpp) isn't in the
// launcher-only build.
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "third_party/stb/stb_image_write.h"

#include "files.h"
#include "program.h"
#include "text.h"

namespace fs = std::filesystem;

namespace dao::launcher::steam
{
    namespace
    {
        // Steam's binary KeyValues, as shortcuts.vdf holds them: a type byte, a
        // NUL-terminated name, then the value; a map's entries end with kEnd.
        enum Type : uint8_t
        {
            kMap = 0,
            kString = 1,
            kInt32 = 2,
            kFloat = 3,
            kPointer = 4,
            kColor = 6,
            kUint64 = 7,
            kEnd = 8,
            kInt64 = 10,
        };

        struct Node
        {
            uint8_t type = kMap;
            std::string name;
            std::string text;      // kString
            uint64_t number = 0;   // The 4- and 8-byte types, as stored.
            std::vector<Node> children;
        };

        // Nesting far beyond what shortcuts.vdf has (3 levels).
        constexpr int kMaxDepth = 16;
        // A shortcuts.vdf with thousands of games is a few hundred KB.
        constexpr uintmax_t kMaxFileSize = 64u << 20;

        bool ReadString(std::span<const uint8_t> data, size_t& pos, std::string& out)
        {
            const auto end = std::find(data.begin() + pos, data.end(), uint8_t(0));
            if (end == data.end())
                return false;
            out.assign(data.begin() + pos, end);
            pos = size_t(end - data.begin()) + 1;
            return true;
        }

        bool ReadMap(std::span<const uint8_t> data, size_t& pos, std::vector<Node>& out, int depth)
        {
            if (depth > kMaxDepth)
                return false;
            while (pos < data.size())
            {
                Node node;
                node.type = data[pos++];
                if (node.type == kEnd)
                    return true;
                if (!ReadString(data, pos, node.name))
                    return false;
                size_t width = 0;
                switch (node.type)
                {
                case kMap:
                    if (!ReadMap(data, pos, node.children, depth + 1))
                        return false;
                    break;
                case kString:
                    if (!ReadString(data, pos, node.text))
                        return false;
                    break;
                case kInt32:
                case kFloat:
                case kPointer:
                case kColor:
                    width = 4;
                    break;
                case kUint64:
                case kInt64:
                    width = 8;
                    break;
                default:
                    return false;
                }
                if (width)
                {
                    if (pos + width > data.size())
                        return false;
                    memcpy(&node.number, data.data() + pos, width);
                    pos += width;
                }
                out.push_back(std::move(node));
            }
            return false;
        }

        void WriteMap(const std::vector<Node>& nodes, std::string& out)
        {
            for (const Node& node : nodes)
            {
                out += char(node.type);
                out += node.name;
                out += '\0';
                switch (node.type)
                {
                case kMap:
                    WriteMap(node.children, out);
                    break;
                case kString:
                    out += node.text;
                    out += '\0';
                    break;
                case kUint64:
                case kInt64:
                    out.append(reinterpret_cast<const char*>(&node.number), 8);
                    break;
                default:
                    out.append(reinterpret_cast<const char*>(&node.number), 4);
                    break;
                }
            }
            out += char(kEnd);
        }

        Node* Child(std::vector<Node>& nodes, std::string_view name)
        {
            const std::string wanted = Lower(name);
            const auto it = std::ranges::find_if(nodes, [&](const Node& node) { return Lower(node.name) == wanted; });
            return it == nodes.end() ? nullptr : &*it;
        }

        Node& Field(Node& entry, std::string_view name, uint8_t type)
        {
            Node* field = Child(entry.children, name);
            if (!field)
            {
                entry.children.push_back({ .type = type, .name = std::string(name) });
                field = &entry.children.back();
            }
            field->type = type;
            return *field;
        }

        void SetString(Node& entry, std::string_view name, std::string value)
        {
            Field(entry, name, kString).text = std::move(value);
        }

        void SetInt(Node& entry, std::string_view name, uint32_t value)
        {
            Field(entry, name, kInt32).number = value;
        }

        uint32_t Crc32(std::string_view text)
        {
            uint32_t crc = 0xFFFFFFFF;
            for (const char c : text)
            {
                crc ^= uint8_t(c);
                for (int bit = 0; bit < 8; bit++)
                    crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
            }
            return ~crc;
        }

        bool WritePng(const fs::path& path, const Artwork& art)
        {
            std::string png;
            const auto append = [](void* context, void* data, int size) {
                static_cast<std::string*>(context)->append(static_cast<const char*>(data), size_t(size));
            };
            if (art.width <= 0 || art.height <= 0 || art.rgba.size() != size_t(art.width) * art.height * 4 ||
                !stbi_write_png_to_func(append, &png, art.width, art.height, 4, art.rgba.data(), art.width * 4))
                return false;
            return WriteFileAtomically(path, png);
        }

        // The Steam accounts that have used this PC: userdata/<account id>.
        std::vector<fs::path> Accounts(const fs::path& steam)
        {
            std::vector<fs::path> accounts;
            std::error_code ec;
            for (fs::directory_iterator it(steam / "userdata", ec), end; !ec && it != end; it.increment(ec))
            {
                const std::string name = Utf8(it->path().filename());
                // (0 isn't an account: Steam keeps shared settings there.)
                if (it->is_directory(ec) && !name.empty() && name != "0" &&
                    std::ranges::all_of(name, [](char c) { return c >= '0' && c <= '9'; }))
                    accounts.push_back(it->path());
            }
            return accounts;
        }

        // Adds or updates the shortcut in one account's shortcuts.vdf; returns its app
        // id (0 with `error` if the file can't be read or written).
        uint32_t AddToAccount(const fs::path& account, const Shortcut& shortcut, std::string& error)
        {
            const fs::path config = account / "config";
            const fs::path file = config / "shortcuts.vdf";
            std::error_code ec;
            fs::create_directories(config, ec);

            std::vector<Node> root;
            const bool exists = fs::exists(file, ec);
            if (ec)
            {
                error = fmt::format("can't read {}", Utf8(file));
                return 0;
            }
            // (An empty one, as Steam may leave, is as none.)
            const uintmax_t size = exists ? fs::file_size(file, ec) : 0;
            if (exists && (ec || size > 0))
            {
                std::vector<uint8_t> data(ec || size > kMaxFileSize ? 0 : size_t(size));
                std::ifstream in(file, std::ios::binary);
                size_t pos = 0;
                if (data.empty() || !in.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size())) ||
                    !ReadMap(data, pos, root, 0))
                {
                    error = fmt::format("can't read {}", Utf8(file));
                    return 0;
                }
                // A copy of how it was before the first change, in case Steam doesn't
                // like it.
                fs::copy_file(file, fs::path(file).concat(L".daorecomp-backup"), fs::copy_options::skip_existing, ec);
            }
            Node* shortcuts = Child(root, "shortcuts");
            if (!shortcuts)
            {
                root.push_back({ .type = kMap, .name = "shortcuts" });
                shortcuts = &root.back();
            }
            else if (shortcuts->type != kMap)
            {
                error = fmt::format("{} isn't laid out as expected", Utf8(file));
                return 0;
            }

            // The exe and its folder in quotes, as Steam writes them.
            const std::string exe = "\"" + Utf8(shortcut.exe) + "\"";
            const std::string folder = "\"" + Utf8(shortcut.exe.parent_path()) + "\\\"";
            Node* entry = nullptr;
            for (Node& item : shortcuts->children)
            {
                const Node* itemExe = item.type == kMap ? Child(item.children, "Exe") : nullptr;
                if (itemExe && Lower(itemExe->text) == Lower(exe))
                    entry = &item;
            }
            if (!entry)
            {
                // After the highest index there is.
                uint32_t next = 0;
                for (const Node& item : shortcuts->children)
                {
                    uint32_t value = 0;
                    if (std::from_chars(item.name.data(), item.name.data() + item.name.size(), value).ec == std::errc())
                        next = std::max(next, value + 1);
                }
                shortcuts->children.push_back({ .type = kMap, .name = std::to_string(next) });
                entry = &shortcuts->children.back();
                // What Steam gives a new shortcut.
                SetString(*entry, "ShortcutPath", "");
                SetInt(*entry, "IsHidden", 0);
                SetInt(*entry, "AllowDesktopConfig", 1);
                SetInt(*entry, "AllowOverlay", 1);
                SetInt(*entry, "OpenVR", 0);
                SetInt(*entry, "Devkit", 0);
                SetString(*entry, "DevkitGameID", "");
                SetInt(*entry, "DevkitOverrideAppID", 0);
                SetInt(*entry, "LastPlayTime", 0);
                SetString(*entry, "FlatpakAppID", "");
                Field(*entry, "tags", kMap);
            }
            // The id Steam derives for a non-Steam game (kept if it has one: its
            // artwork and playtime are filed under it).
            const Node* id = Child(entry->children, "appid");
            const uint32_t appId = id && id->number ? uint32_t(id->number) : (Crc32(exe + shortcut.name) | 0x80000000u);
            SetInt(*entry, "appid", appId);
            SetString(*entry, "AppName", shortcut.name);
            SetString(*entry, "Exe", exe);
            SetString(*entry, "StartDir", folder);
            SetString(*entry, "icon", Utf8(shortcut.exe));
            SetString(*entry, "LaunchOptions", shortcut.launchOptions);

            std::string out;
            WriteMap(root, out);
            if (!WriteFileAtomically(file, out))
            {
                error = fmt::format("can't write {}", Utf8(file));
                return 0;
            }
            return appId;
        }
    }

    fs::path FindSteam()
    {
        // For tests: a Steam folder to use in place of the installed one.
        if (const wchar_t* test = _wgetenv(L"DAO_STEAM_ROOT"); test && *test)
            return test;
        // Under Proton or Wine, the Steam there is a stand-in (Steam itself runs
        // outside, on Linux).
        if (IsUnderWine())
            return {};
        wchar_t path[MAX_PATH * 2];
        DWORD size = sizeof(path);
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, path,
                &size) != ERROR_SUCCESS)
            return {};
        const fs::path steam = fs::path(path).make_preferred();
        std::error_code ec;
        return fs::is_regular_file(steam / "steam.exe", ec) ? steam : fs::path();
    }

    namespace
    {
        // The steam.exe processes of the Steam in that folder that are still running,
        // or with `exited` those that have exited but Windows still holds (a thread
        // stuck in the kernel keeps the process and its handles, and Steam won't start
        // again while they're held). One that's gone, though something still has it
        // open, is neither. Opened with `access` (and PROCESS_QUERY_LIMITED_INFORMATION,
        // and SYNCHRONIZE where allowed); the caller closes them.
        std::vector<HANDLE> SteamProcesses(const fs::path& steam, DWORD access, bool exited = false)
        {
            access |= PROCESS_QUERY_LIMITED_INFORMATION;
            std::vector<HANDLE> processes;
            const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (snapshot == INVALID_HANDLE_VALUE)
                return processes;
            const std::string wanted = Lower(Utf8((steam / "steam.exe").lexically_normal()));
            PROCESSENTRY32W entry = { .dwSize = sizeof(entry) };
            for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry))
            {
                if (_wcsicmp(entry.szExeFile, L"steam.exe") != 0)
                    continue;
                // (A Steam run as administrator may not allow waiting on it: then it's
                // taken as not gone.)
                HANDLE process = OpenProcess(access | SYNCHRONIZE, FALSE, entry.th32ProcessID);
                const bool waitable = process;
                if (!process)
                    process = OpenProcess(access, FALSE, entry.th32ProcessID);
                if (!process)
                    continue;
                wchar_t image[MAX_PATH * 2];
                DWORD size = DWORD(std::size(image));
                DWORD code = 0;
                if (QueryFullProcessImageNameW(process, 0, image, &size) &&
                    Lower(Utf8(fs::path(image).lexically_normal())) == wanted &&
                    (!waitable || WaitForSingleObject(process, 0) == WAIT_TIMEOUT) &&
                    GetExitCodeProcess(process, &code) && (code != STILL_ACTIVE) == exited)
                    processes.push_back(process);
                else
                    CloseHandle(process);
            }
            CloseHandle(snapshot);
            return processes;
        }

        bool AnySteamProcess(const fs::path& steam, bool exited)
        {
            const std::vector<HANDLE> processes = SteamProcesses(steam, 0, exited);
            for (const HANDLE process : processes)
                CloseHandle(process);
            return !processes.empty();
        }
    }

    bool IsRunning(const fs::path& steam)
    {
        return AnySteamProcess(steam, false);
    }

    bool IsStuck(const fs::path& steam)
    {
        return AnySteamProcess(steam, true);
    }

    bool RequestShutdown(const fs::path& steam)
    {
        return StartProgram(steam / "steam.exe", L"-shutdown", 0, steam);
    }

    bool End(const fs::path& steam)
    {
        // (None found: it's run as administrator, or another user, or it has closed.)
        const std::vector<HANDLE> processes = SteamProcesses(steam, PROCESS_TERMINATE);
        bool ended = !processes.empty();
        for (const HANDLE process : processes)
        {
            LOGW("Steam: ending steam.exe (process {}), stuck while closing", GetProcessId(process));
            // (Exit code 1, as Task Manager's End task gives.)
            if (!TerminateProcess(process, 1))
            {
                LOGW("Steam: can't end process {} (error {})", GetProcessId(process), GetLastError());
                ended = false;
            }
            CloseHandle(process);
        }
        if (processes.empty())
            LOGW("Steam: can't open steam.exe to end it (run as administrator?)");
        return ended;
    }

    bool Start(const fs::path& steam)
    {
        return StartProgram(steam / "steam.exe", L"", 0, steam);
    }

    int AddShortcut(const fs::path& steam, const Shortcut& shortcut, std::span<const Artwork> artwork,
        std::string& error)
    {
        const std::vector<fs::path> accounts = Accounts(steam);
        if (accounts.empty())
        {
            error = "no Steam account has signed in on this PC yet";
            return 0;
        }
        int added = 0;
        for (const fs::path& account : accounts)
        {
            std::string accountError;
            const uint32_t appId = AddToAccount(account, shortcut, accountError);
            if (!appId)
            {
                LOGW("Steam: {}", accountError);
                error = accountError;
                continue;
            }
            added++;
            const fs::path grid = account / "config" / "grid";
            std::error_code ec;
            fs::create_directories(grid, ec);
            for (const Artwork& art : artwork)
            {
                const fs::path file = grid / fmt::format("{}{}.png", appId, art.suffix);
                if (!WritePng(file, art))
                    LOGW("Steam: can't write {}", Utf8(file));
            }
            LOGI("Steam: added {} to account {} (app id {})", shortcut.name, Utf8(account.filename()), appId);
        }
        // (With `error` still if an account didn't get it.)
        return added;
    }
}
