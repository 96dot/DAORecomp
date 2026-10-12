#include "stdafx.h"
#include "content/install.h"
#include "content/content.h"
#include "content/stfs.h"
#include "kernel/title_update.h"
#include "text.h"

#include <charconv>
#include <unordered_set>

namespace fs = std::filesystem;

namespace dao::content
{
    namespace
    {
        // The game's (installer/deps.py GAME_TITLE_ID).
        constexpr uint32_t kTitleId = 0x454108C0;
        // The disc this is recompiled from (installer/deps.py GAME_MEDIA_ID): title
        // updates are made for one disc (other languages' discs have their own).
        constexpr uint32_t kMediaId = 0x186A1115;
        // A folder dropped is searched this deep, and this far, for packages.
        constexpr int kSearchDepth = 8;
        constexpr size_t kSearchEntries = 20000;

        using Line = mods::InstallResult::Line;

        // Package files have no extension on the console; copies may have one of these.
        bool MayBePackage(const fs::path& path)
        {
            const std::string extension = Lower(Utf8(path.extension()));
            return extension.empty() || extension == ".bin" || extension == ".pkg" || extension == ".stfs" ||
                   extension == ".con" || extension == ".live" || extension == ".pirs";
        }

        // The package's name for messages and the log (without control characters:
        // it's the package's to choose).
        std::string Name(const StfsPackage& package)
        {
            std::wstring name;
            for (const char16_t c : package.displayName)
            {
                if (c >= 0x20 && c != 0x7F)
                    name += wchar_t(c);
            }
            return WideToUtf8(name);
        }

        bool ProcessRunning(DWORD id)
        {
            const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, id);
            if (!process)
                return GetLastError() == ERROR_ACCESS_DENIED;
            DWORD code = 0;
            const bool running = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
            CloseHandle(process);
            return running;
        }

        // Removes the work folders (.new<process ID>) of installs that stopped
        // half-way: their process isn't running any more.
        void RemoveStaleWork(const fs::path& parent)
        {
            std::error_code ec;
            std::vector<fs::path> stale;
            for (const auto& entry : fs::directory_iterator(parent, ec))
            {
                const std::string name = Utf8(entry.path().filename());
                DWORD id = 0;
                const char* digits = name.c_str() + 4;
                if (!name.starts_with(".new") || name.size() < 5 || entry.is_symlink(ec) ||
                    !entry.is_directory(ec) ||
                    std::from_chars(digits, name.c_str() + name.size(), id).ptr != name.c_str() + name.size())
                    continue;
                if (id != GetCurrentProcessId() && !ProcessRunning(id))
                    stale.push_back(entry.path());
            }
            for (const fs::path& folder : stale)
            {
                LOGI("Content: removing {}, left by an install that stopped", Utf8(folder));
                fs::remove_all(folder, ec);
            }
        }

        // Makes a work folder of this process's own in `parent` (false if one is
        // there already): no longer than the folder it becomes, so what fits there
        // fits here.
        bool MakeWorkFolder(const fs::path& parent, fs::path& work)
        {
            std::error_code ec;
            fs::create_directories(parent, ec);
            RemoveStaleWork(parent);
            work = parent / FromUtf8(fmt::format(".new{}", GetCurrentProcessId()));
            return fs::create_directory(work, ec);
        }

        // Whether every file fits under `folder` within MAX_PATH: neither the game
        // nor this opens longer paths. (UTF-8 lengths: at least the UTF-16 ones.)
        bool Fits(const fs::path& folder, const StfsPackage& package)
        {
            size_t longest = 0;
            for (const StfsFile& file : package.files)
                longest = std::max(longest, file.path.size());
            return folder.native().size() + 1 + longest < MAX_PATH;
        }

        std::string TooLong(const fs::path& folder)
        {
            return fmt::format("its files' paths would be too long for Windows in {} (move the game to a folder "
                               "with a shorter path)", Utf8(folder));
        }

        // Extracts into a new work folder in `parent`, removed again on failure;
        // false and the message for `lines` if it fails.
        bool ExtractToWork(StfsPackage& package, const fs::path& parent, std::stop_token stop, fs::path& work,
            std::string& error)
        {
            if (!MakeWorkFolder(parent, work))
            {
                error = fmt::format("{} is in the way (another install running?)", Utf8(work));
                return false;
            }
            if (package.Extract(work, stop, error))
                return true;
            std::error_code ec;
            fs::remove_all(work, ec);
            return false;
        }

        void InstallDlc(const fs::path& userRoot, StfsPackage& package, std::stop_token stop,
            std::vector<Line>& lines)
        {
            const std::string name = Name(package);
            const fs::path content = userRoot / "content";
            const fs::path folder = ContentFolder(userRoot, kTitleId, kContentMarketplace);
            const fs::path target = folder / package.contentId;
            std::error_code ec;
            // Installed under the console's name (the content ID and a digit) or ours.
            const std::string id = Lower(package.contentId);
            for (const auto& entry : fs::directory_iterator(folder, ec))
            {
                if (entry.is_directory(ec) && Lower(Utf8(entry.path().filename())).starts_with(id))
                {
                    lines.push_back({ fmt::format("{} is already installed.", name) });
                    return;
                }
            }
            // Extracted outside the folder the game lists, then moved in. (The work
            // folder's name is shorter than the content ID.)
            if (!Fits(target, package))
            {
                lines.push_back({ fmt::format("Can't install {}: {}.", name, TooLong(content)), true });
                return;
            }
            fs::path work;
            std::string error;
            if (!ExtractToWork(package, content, stop, work, error))
            {
                lines.push_back({ fmt::format("Can't install {}: {}.", name, error), true });
                return;
            }
            fs::create_directories(folder, ec);
            fs::rename(work, target, ec);
            if (ec)
            {
                fs::remove_all(work, ec);
                lines.push_back({ fmt::format("Can't install {}: it can't be moved into {}.", name,
                    Utf8(folder)), true });
                return;
            }
            // The display name the game lists it by (as XamContentCreateEx keeps it).
            uint8_t display[kDisplayNameSize]{};
            for (size_t i = 0; i < package.displayName.size() && i < kDisplayNameSize / 2 - 1; i++)
            {
                display[2 * i] = uint8_t(package.displayName[i] >> 8);
                display[2 * i + 1] = uint8_t(package.displayName[i]);
            }
            WriteDisplayName(folder, package.contentId, display);
            LOGI("Content: installed {} ({}) to {}", name, package.contentId, Utf8(folder));
            lines.push_back({ fmt::format("Installed {}.", name) });
        }

        void InstallTitleUpdate(const fs::path& gameRoot, StfsPackage& package, std::stop_token stop,
            std::vector<Line>& lines)
        {
            const fs::path update = kernel::UpdateRoot(gameRoot);
            std::error_code ec;
            if (kernel::IsTu6Patch(update / "default.xexp"))
            {
                lines.push_back({ "Title Update 6 is already installed." });
                return;
            }
            if (fs::exists(update, ec))
            {
                lines.push_back({ fmt::format("Can't install Title Update 6: {} holds something else. Move it "
                                              "away and drop the update again.", Utf8(update)), true });
                return;
            }
            // (The work folder's name may be a few characters longer.)
            const fs::path parent = update.parent_path();
            if (!Fits(parent / "title_update.new00000", package))
            {
                lines.push_back({ fmt::format("Can't install Title Update 6: {}.", TooLong(parent)), true });
                return;
            }
            fs::path work;
            std::string error;
            if (!ExtractToWork(package, parent, stop, work, error))
            {
                lines.push_back({ fmt::format("Can't install Title Update 6: {}.", error), true });
                return;
            }
            if (!kernel::IsTu6Patch(work / "default.xexp"))
            {
                fs::remove_all(work, ec);
                lines.push_back({ "This title update isn't Title Update 6, the one the game is made for.", true });
                return;
            }
            fs::rename(work, update, ec);
            if (ec)
            {
                fs::remove_all(work, ec);
                lines.push_back({ fmt::format("Can't install Title Update 6: it can't be moved to {}.", Utf8(update)),
                    true });
                return;
            }
            LOGI("Content: installed Title Update 6 to {}", Utf8(update));
#if DAO_HAS_TU6
            lines.push_back({ "Installed Title Update 6: Awakening and the DLC campaigns can be played." });
#else
            lines.push_back({ "Installed Title Update 6. The game needs building again to use it: run the "
                              "installer and choose this install folder.", true });
#endif
        }
    }

    std::vector<DroppedPackage> TakePackages(std::vector<fs::path>& dropped)
    {
        std::vector<DroppedPackage> packages;
        std::error_code ec;
        std::erase_if(dropped, [&](const fs::path& path) {
            if (fs::is_regular_file(path, ec))
            {
                if (!StfsPackage::IsPackage(path))
                    return false;
                packages.push_back({ path });
                return true;
            }
            if (!fs::is_directory(path, ec))
                return false;
            std::vector<DroppedPackage> found;
            size_t seen = 0;
            for (auto it = fs::recursive_directory_iterator(path, fs::directory_options::skip_permission_denied, ec);
                 it != fs::recursive_directory_iterator() && seen < kSearchEntries; it.increment(ec), seen++)
            {
                if (ec)
                    break;
                if (it.depth() >= kSearchDepth)
                    it.disable_recursion_pending();
                if (it->is_regular_file(ec) && MayBePackage(it->path()) && StfsPackage::IsPackage(it->path()))
                    found.push_back({ it->path(), true });
            }
            if (found.empty())
                return false;
            packages.insert(packages.end(), found.begin(), found.end());
            return true;
        });
        return packages;
    }

    void InstallPackages(const fs::path& gameRoot, const fs::path& userRoot,
        const std::vector<DroppedPackage>& packages, std::stop_token stop, std::vector<Line>& lines)
    {
        std::unordered_set<std::string> done;
        // A title update for another disc is only mentioned if this disc's isn't there too.
        std::vector<std::string> otherDiscs;
        bool updateFound = false;
        const size_t linesBefore = lines.size();
        for (const auto& [path, inFolder] : packages)
        {
            if (stop.stop_requested())
                return;
            // Packages found in a folder that aren't this game's DLC are passed over
            // (a console's folders also hold saves, the dashboard's art, ...).
            std::string error;
            auto package = StfsPackage::Open(path, error);
            const std::string file = Utf8(path.filename());
            if (!package)
            {
                if (!inFolder)
                    lines.push_back({ fmt::format("{}: {}.", file, error), true });
                continue;
            }
            if (package->titleId != kTitleId)
            {
                if (inFolder)
                    continue;
                lines.push_back({ fmt::format("{} ({}) isn't for Dragon Age: Origins.", file, Name(*package)), true });
                continue;
            }
            if (!done.insert(package->contentId).second)
                continue;
            if (package->contentType == kContentMarketplace)
            {
                InstallDlc(userRoot, *package, stop, lines);
            }
            else if (package->contentType == kContentInstaller)
            {
                if (package->mediaId != kMediaId)
                {
                    otherDiscs.push_back(fmt::format("{:08X}", package->mediaId));
                    continue;
                }
                updateFound = true;
                InstallTitleUpdate(gameRoot, *package, stop, lines);
            }
            else if (!inFolder)
            {
                lines.push_back({ fmt::format("{} ({}) isn't downloadable content or a title update.", file,
                    Name(*package)), true });
            }
        }
        if (!updateFound && !otherDiscs.empty())
        {
            lines.push_back({ fmt::format("The title update dropped is for another edition of the game (disc {}); "
                                          "yours needs the one for disc {:08X}.", otherDiscs.front(), kMediaId), true });
        }
        if (lines.size() == linesBefore && !packages.empty())
        {
            lines.push_back({ "There's no downloadable content or title update for Dragon Age: Origins in the "
                              "folders dropped.", true });
        }
    }
}
