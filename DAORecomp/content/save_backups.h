#pragma once
// The saves' backups. Overwriting a save keeps the one it replaces in
// userdata/save_backups/<name>/, with its display name in <name>.name (XAM's
// SetSaveAside, kernel/xam.cpp); the launcher lists them and puts one back. A restore
// sets the save it replaces aside there as <name>~current while it works.

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace dao::content
{
    // userdata/save_backups.
    std::filesystem::path SaveBackupsFolder(const std::filesystem::path& userRoot);

    // The mark of a save being written, in save_backups: there from the game creating
    // the save until it closes it (left by a crash while saving). `name` is the save's
    // folder name.
    std::filesystem::path SaveWritingMarker(const std::filesystem::path& userRoot, const std::filesystem::path& name);

    // A save as its display name describes it ("<character>_<area>_<UTC time>"); one
    // whose name doesn't say so has that name (or, without one, its folder's) as the
    // character.
    struct SaveVersion
    {
        bool exists = false;
        std::string character, area;
        // When it was saved; if the name doesn't say, when its newest file was written
        // (0 if it has none).
        std::chrono::sys_seconds saved{};
    };

    struct SaveBackup
    {
        // The save's folder name (e.g. 0xa16f59e3_2_u!0).
        std::string name;
        SaveVersion backup;
        // The save in its place now (exists false if it was deleted).
        SaveVersion current;
        // The game stopped while writing the save there now.
        bool currentUnfinished = false;
    };

    // The backups there are, by character, the latest first. With finishStopped (only
    // while the game isn't running: it could be saving), a restore that stopped
    // half-way is finished first.
    std::vector<SaveBackup> ListSaveBackups(const std::filesystem::path& userRoot, bool finishStopped);

    struct RestoreResult
    {
        bool restored = false;
        // Why it wasn't; or, restored, what didn't go as it should.
        std::string problem;
    };

    // Puts the backup of save `name` back, and the save it replaces becomes the backup
    // (so restoring again switches back); a deleted save just comes back. Not restored,
    // the saves are as they were (unless `problem` says one is in <name>~current: the
    // next ListSaveBackups with the game closed puts it back).
    RestoreResult RestoreSaveBackup(const std::filesystem::path& userRoot, const std::string& name);
}
