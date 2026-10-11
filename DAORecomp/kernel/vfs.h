#pragma once
// Guest path namespace -> host folders.
//
// Guest paths come in three shapes:
//   game:\packages\core.erf       (drive: symbolic link)
//   \Device\Cdrom0\default.xex    (device path)
//   \??\d:\foo                    (DOS-devices prefix + drive)
// Drives resolve through symbolic links (ObCreateSymbolicLink, XamContentCreate)
// to device paths, and device paths map onto host directories by longest prefix.

#include "objects.h"

namespace dao::kernel
{
    void VfsInit(const std::filesystem::path& gameRoot, const std::filesystem::path& userRoot);

    // Maps a guest device path (e.g. "\Device\Cdrom0") to a host directory.
    void VfsMountDevice(const std::string& devicePath, const std::filesystem::path& hostRoot);
    void VfsUnmountDevice(const std::string& devicePath);

    // "game:" -> "\Device\Cdrom0". Names are case-insensitive.
    void VfsCreateSymlink(const std::string& link, const std::string& target);
    bool VfsDeleteSymlink(const std::string& link);

    // Host folders layered over a device, highest priority first (mods over the
    // disc): a path that exists in an overlay resolves there.
    void VfsSetOverlays(const std::string& devicePath, std::vector<std::filesystem::path> overlays);
    // Serves a host file the runtime generates at a guest path, above the overlays
    // (over the disc and the mods). Only that exact path resolves to it.
    void VfsAddFile(std::string_view guestPath, const std::filesystem::path& hostPath);

    // Resolves a guest path to a host path; nullopt if no device matches.
    std::optional<std::filesystem::path> VfsResolve(std::string_view guestPath);
    // Every host directory a guest directory is made of (existing overlays first,
    // then the device's own path), for merged directory listings.
    std::vector<std::filesystem::path> VfsResolveAll(std::string_view guestPath);

    // Is this host path under a read-only (disc) device?
    bool VfsIsReadOnly(const std::filesystem::path& hostPath);

    const std::filesystem::path& VfsUserRoot();

    struct File final : KernelObject
    {
        File() : KernelObject(ObjectType::File) {}
        ~File() override;

        std::string guestPath;
        std::filesystem::path hostPath;
        HANDLE host = INVALID_HANDLE_VALUE;
        bool isDirectory = false;
        bool synchronous = true;
        bool deleteOnClose = false;
        uint64_t position = 0;
        std::mutex mutex;

        // Directory enumeration state (NtQueryDirectoryFile).
        std::vector<std::filesystem::directory_entry> entries;
        size_t nextEntry = 0;
        bool enumerated = false;

        // File handles are always signaled (I/O completes synchronously).
        bool IsWaitable() const override { return true; }
        bool IsSignaled(GuestThread*) const override { return true; }
    };
}
