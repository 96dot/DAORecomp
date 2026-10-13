#include "stdafx.h"
#include "content/save_backups.h"
#include "content/content.h"
#include "files.h"
#include "text.h"

#include <fstream>

namespace fs = std::filesystem;

namespace dao::content
{
    namespace
    {
        using namespace std::chrono;

        // The restore's name for the save it sets aside (see save_backups.h).
        constexpr std::wstring_view kAside = L"~current";

        // A display name file's bytes (kDisplayNameSize at most); empty if there's none.
        std::string ReadNameFile(const fs::path& path)
        {
            std::string data(kDisplayNameSize, '\0');
            std::ifstream in(path, std::ios::binary);
            in.read(data.data(), std::streamsize(data.size()));
            data.resize(size_t(in.gcount()));
            return data;
        }

        // "YYYYMMDDhhmmss" (UTC), as the game ends a save's display name with.
        std::optional<sys_seconds> ParseSaveTime(std::string_view text)
        {
            if (text.size() != 14 || !std::ranges::all_of(text, [](char c) { return c >= '0' && c <= '9'; }))
                return std::nullopt;
            const auto number = [&](size_t at, size_t length) {
                int value = 0;
                for (size_t i = at; i < at + length; i++)
                    value = value * 10 + (text[i] - '0');
                return value;
            };
            const year_month_day date{ year(number(0, 4)), month(unsigned(number(4, 2))), day(unsigned(number(6, 2))) };
            const int hour = number(8, 2), minute = number(10, 2), second = number(12, 2);
            if (!date.ok() || hour > 23 || minute > 59 || second > 59)
                return std::nullopt;
            return sys_days(date) + hours(hour) + minutes(minute) + seconds(second);
        }

        // When the newest file in a save's folder (or its Saves folder) was written;
        // 0 if it has none.
        sys_seconds FolderTime(const fs::path& folder)
        {
            std::optional<fs::file_time_type> newest;
            std::error_code ec;
            for (fs::recursive_directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
            {
                std::error_code timeError;
                if (it->is_directory(timeError) && (it.depth() >= 1 || !IsPlainFolder(it->path())))
                    it.disable_recursion_pending();
                else if (const fs::file_time_type time = it->last_write_time(timeError);
                         !timeError && it->is_regular_file(timeError))
                    newest = std::max(newest.value_or(time), time);
            }
            return newest ? floor<seconds>(clock_cast<system_clock>(*newest)) : sys_seconds{};
        }

        SaveVersion DescribeSave(const fs::path& folder, const std::string& name)
        {
            SaveVersion save;
            const fs::path path = folder / FromUtf8(name);
            if (!IsPlainFolder(path))
                return save;
            save.exists = true;
            const std::string display = ReadDisplayName(folder, name);
            const size_t first = display.find('_'), last = display.rfind('_');
            const std::optional<sys_seconds> time =
                first != last ? ParseSaveTime(std::string_view(display).substr(last + 1)) : std::nullopt;
            if (time && first > 0)
            {
                save.character = display.substr(0, first);
                save.area = display.substr(first + 1, last - first - 1);
                save.saved = *time;
            }
            else
            {
                save.character = display.empty() ? name : display;
                save.saved = FolderTime(path);
            }
            return save;
        }

        // Finishes a restore that stopped half-way (see RestoreSaveBackup), from the
        // save it set aside, <name>~current: put back in place if nothing took its
        // place, or made the backup if the backup did. False if neither fits (all
        // three are there), and it's left alone.
        bool FinishStoppedRestore(const fs::path& backups, const fs::path& saves, const std::string& name)
        {
            const fs::path aside = backups / (FromUtf8(name).native() + std::wstring(kAside));
            std::error_code ec;
            if (!fs::exists(fs::symlink_status(aside, ec)))
                return true;
            const fs::path current = saves / FromUtf8(name), backup = backups / FromUtf8(name);
            const fs::path to = !fs::exists(fs::symlink_status(current, ec)) ? current
                : !fs::exists(fs::symlink_status(backup, ec))              ? backup
                                                                           : fs::path();
            if (to.empty())
                return false;
            fs::create_directories(to.parent_path(), ec);
            fs::rename(aside, to, ec);
            if (ec)
                return false;
            LOGI("Saves: finished a restore of '{}' that stopped: the save set aside is {}", name,
                to == current ? "back in place" : "the backup");
            return true;
        }
    }

    fs::path SaveBackupsFolder(const fs::path& userRoot)
    {
        return userRoot / "save_backups";
    }

    fs::path SaveWritingMarker(const fs::path& userRoot, const fs::path& name)
    {
        return SaveBackupsFolder(userRoot) / (name.native() + L".writing");
    }

    std::vector<SaveBackup> ListSaveBackups(const fs::path& userRoot)
    {
        const fs::path folder = SaveBackupsFolder(userRoot);
        const fs::path saves = ContentFolder(userRoot, kTitleId, kContentSave);
        std::vector<std::string> names, stopped;
        std::error_code ec;
        for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
        {
            const std::wstring& file = it->path().filename().native();
            if (file.ends_with(kAside))
                stopped.push_back(WideToUtf8(file.substr(0, file.size() - kAside.size())));
            // Backups: not one being made (<name>.new) or a restore's work (<name>~current).
            else if (const std::string name = WideToUtf8(file); IsContentName(name) && IsPlainFolder(it->path()))
                names.push_back(name);
        }
        for (const std::string& name : stopped)
        {
            if (IsContentName(name) && FinishStoppedRestore(folder, saves, name) &&
                std::ranges::find(names, name) == names.end() && IsPlainFolder(folder / FromUtf8(name)))
                names.push_back(name);
        }

        std::vector<SaveBackup> backups;
        for (const std::string& name : names)
        {
            SaveBackup backup{ name, DescribeSave(folder, name), DescribeSave(saves, name) };
            backup.currentUnfinished = backup.current.exists && fs::exists(SaveWritingMarker(userRoot, FromUtf8(name)), ec);
            backups.push_back(std::move(backup));
        }
        std::ranges::sort(backups, [](const SaveBackup& a, const SaveBackup& b) {
            const std::string ca = Lower(a.backup.character), cb = Lower(b.backup.character);
            if (ca != cb)
                return ca < cb;
            if (a.backup.saved != b.backup.saved)
                return a.backup.saved > b.backup.saved;
            return a.name < b.name;
        });
        return backups;
    }

    // The backup and the save in its place swap folders by renames (one drive, no
    // copying): the save set aside as <name>~current, the backup moved into its place,
    // the save set aside made the backup; then their display names swap. A restore that
    // stops after the first rename is finished by the next (FinishStoppedRestore).
    RestoreResult RestoreSaveBackup(const fs::path& userRoot, const std::string& name)
    {
        if (!IsContentName(name))
            return { false, "that isn't a save's name" };
        const fs::path backups = SaveBackupsFolder(userRoot);
        const fs::path saves = ContentFolder(userRoot, kTitleId, kContentSave);
        const fs::path file = FromUtf8(name);
        const fs::path backup = backups / file, current = saves / file;
        const fs::path aside = backups / (file.native() + std::wstring(kAside));
        const fs::path nameFile = file.native() + L".name";
        if (!FinishStoppedRestore(backups, saves, name))
            return { false, fmt::format("save_backups/{}~current, left by a restore that stopped, holds a save: "
                                        "move it somewhere else first", name) };
        if (!IsPlainFolder(backup))
            return { false, "the backup isn't there any more" };
        std::error_code ec;
        const bool hadSave = fs::exists(fs::symlink_status(current, ec));
        const std::string backupName = ReadNameFile(backups / nameFile);
        const std::string currentName = hadSave ? ReadNameFile(saves / nameFile) : std::string();
        const fs::path marker = SaveWritingMarker(userRoot, file);
        const bool unfinished = fs::exists(marker, ec);

        // Setting the save aside fails while the game has one of its files open.
        if (hadSave)
        {
            fs::rename(current, aside, ec);
            if (ec)
                return { false, fmt::format("the save there now can't be moved ({}); is the game running?",
                                    ec.message()) };
        }
        fs::create_directories(saves, ec);
        fs::rename(backup, current, ec);
        if (ec)
        {
            const std::string why = ec.message();
            if (hadSave)
            {
                fs::rename(aside, current, ec);
                if (ec)
                    return { false, fmt::format("the backup can't be put in place ({}), and the save there now is "
                                                "in save_backups/{}~current until the list is read again", why, name) };
            }
            return { false, fmt::format("the backup can't be put in place ({})", why) };
        }

        RestoreResult result{ true };
        if (hadSave)
        {
            fs::rename(aside, backup, ec);
            if (ec)
                result.problem = fmt::format("the save it replaced is in save_backups/{}~current until the list is "
                                             "read again ({})", name, ec.message());
        }
        // The display names follow. (A backup kept without one keeps the replaced save's
        // name, the same character's: the game can't list a save with none.)
        if (!backupName.empty() && !WriteFileAtomically(saves / nameFile, backupName))
            result.problem = "its name in the game's list is still the old save's";
        if (!hadSave || currentName.empty())
            fs::remove(backups / nameFile, ec);
        else if (!WriteFileAtomically(backups / nameFile, currentName))
            fs::remove(backups / nameFile, ec);
        // The write the game didn't finish was the save's that's now the backup.
        if (unfinished)
            fs::remove(marker, ec);
        LOGI("Saves: restored the backup of '{}'{}", name, result.problem.empty() ? "" : ": " + result.problem);
        return result;
    }
}
