#include "stdafx.h"
#include "vfs.h"

namespace dao::kernel
{
    namespace fs = std::filesystem;

    struct Device
    {
        std::string path;      // lowercase, no trailing backslash
        fs::path root;
        bool readOnly;
        std::vector<fs::path> overlays; // highest priority first
    };

    static std::mutex s_vfsMutex;
    static std::vector<Device> s_devices;
    static std::unordered_map<std::string, std::string> s_symlinks; // lowercase "game:" -> device path
    static fs::path s_userRoot;

    static std::string Lower(std::string_view s)
    {
        std::string out(s);
        for (char& c : out)
            c = char(tolower(uint8_t(c)));
        return out;
    }

    static std::string Normalize(std::string_view path)
    {
        std::string out;
        out.reserve(path.size());
        for (char c : path)
        {
            if (c == '/')
                c = '\\';
            if (c == '\\' && !out.empty() && out.back() == '\\')
                continue;
            out += c;
        }
        while (out.size() > 1 && out.back() == '\\')
            out.pop_back();
        return out;
    }

    void VfsInit(const fs::path& gameRoot, const fs::path& userRoot)
    {
        s_userRoot = userRoot;
        fs::create_directories(userRoot / "hdd");
        fs::create_directories(userRoot / "cache");

        VfsMountDevice("\\Device\\Cdrom0", gameRoot);
        VfsMountDevice("\\Device\\Harddisk0\\Partition1", userRoot / "hdd");
        VfsMountDevice("\\Device\\Harddisk0\\Cache0", userRoot / "cache");
        VfsMountDevice("\\Device\\Harddisk0\\Cache1", userRoot / "cache");

        VfsCreateSymlink("game:", "\\Device\\Cdrom0");
        VfsCreateSymlink("d:", "\\Device\\Cdrom0");
        VfsCreateSymlink("media:", "\\Device\\Cdrom0");
        VfsCreateSymlink("cache:", "\\Device\\Harddisk0\\Cache0");
        VfsCreateSymlink("hdd:", "\\Device\\Harddisk0\\Partition1");
        VfsCreateSymlink("e:", "\\Device\\Harddisk0\\Partition1");
    }

    void VfsMountDevice(const std::string& devicePath, const fs::path& hostRoot)
    {
        std::lock_guard lock(s_vfsMutex);
        const std::string key = Lower(Normalize(devicePath));
        std::erase_if(s_devices, [&](const Device& d) { return d.path == key; });
        const bool readOnly = key == "\\device\\cdrom0";
        s_devices.push_back({ key, hostRoot, readOnly });
        // Longest prefix first.
        std::sort(s_devices.begin(), s_devices.end(),
            [](const Device& a, const Device& b) { return a.path.size() > b.path.size(); });
        LOGI("VFS: {} -> {}", devicePath, hostRoot.string());
    }

    void VfsUnmountDevice(const std::string& devicePath)
    {
        std::lock_guard lock(s_vfsMutex);
        const std::string key = Lower(Normalize(devicePath));
        std::erase_if(s_devices, [&](const Device& d) { return d.path == key; });
    }

    void VfsCreateSymlink(const std::string& link, const std::string& target)
    {
        std::string key = Lower(Normalize(link));
        if (key.starts_with("\\??\\"))
            key = key.substr(4);
        std::lock_guard lock(s_vfsMutex);
        s_symlinks[key] = Normalize(target);
        LOGI("VFS: symlink {} -> {}", key, target);
    }

    bool VfsDeleteSymlink(const std::string& link)
    {
        std::string key = Lower(Normalize(link));
        if (key.starts_with("\\??\\"))
            key = key.substr(4);
        std::lock_guard lock(s_vfsMutex);
        return s_symlinks.erase(key) != 0;
    }

    void VfsSetOverlays(const std::string& devicePath, std::vector<fs::path> overlays)
    {
        std::lock_guard lock(s_vfsMutex);
        const std::string key = Lower(Normalize(devicePath));
        for (Device& device : s_devices)
        {
            if (device.path == key)
            {
                for (const auto& overlay : overlays)
                    LOGI("VFS: {} overlay {}", devicePath, overlay.string());
                device.overlays = std::move(overlays);
                return;
            }
        }
        LOGW("VFS: no device {} for overlays", devicePath);
    }

    // The device a guest path lives on and the path relative to the device root.
    // Caller holds s_vfsMutex.
    static std::optional<std::pair<const Device*, std::string>> Locate(std::string_view guestPath)
    {
        std::string path = Normalize(guestPath);
        for (int depth = 0; depth < 8; depth++)
        {
            std::string lower = Lower(path);
            if (lower.starts_with("\\??\\"))
            {
                path = path.substr(4);
                continue;
            }

            // drive: prefix
            const size_t colon = path.find(':');
            const size_t slash = path.find('\\');
            if (colon != std::string::npos && (slash == std::string::npos || colon < slash))
            {
                const std::string drive = Lower(path.substr(0, colon + 1));
                auto it = s_symlinks.find(drive);
                if (it == s_symlinks.end())
                    return std::nullopt;
                std::string rest = path.substr(colon + 1);
                if (!rest.empty() && rest[0] != '\\')
                    rest = "\\" + rest;
                path = it->second + rest;
                continue;
            }

            for (const Device& device : s_devices)
            {
                if (lower == device.path || (lower.starts_with(device.path) && lower[device.path.size()] == '\\'))
                {
                    std::string rest = path.substr(device.path.size());
                    while (!rest.empty() && rest[0] == '\\')
                        rest.erase(0, 1);
                    return std::make_pair(&device, rest);
                }
            }

            // Device-relative symlink (e.g. "\Device\Foo" registered via ObCreateSymbolicLink)?
            auto it = s_symlinks.find(lower);
            if (it != s_symlinks.end())
            {
                path = it->second;
                continue;
            }
            return std::nullopt;
        }
        return std::nullopt;
    }

    static fs::path Join(const fs::path& root, const std::string& rest)
    {
        fs::path host = root;
        if (!rest.empty())
            host /= fs::path(rest);
        return host.lexically_normal();
    }

    std::optional<fs::path> VfsResolve(std::string_view guestPath)
    {
        std::lock_guard lock(s_vfsMutex);
        const auto located = Locate(guestPath);
        if (!located)
            return std::nullopt;
        const auto& [device, rest] = *located;
        if (!rest.empty())
        {
            for (const auto& overlay : device->overlays)
            {
                fs::path candidate = Join(overlay, rest);
                std::error_code ec;
                if (fs::exists(candidate, ec))
                    return candidate;
            }
        }
        return Join(device->root, rest);
    }

    std::vector<fs::path> VfsResolveAll(std::string_view guestPath)
    {
        std::lock_guard lock(s_vfsMutex);
        std::vector<fs::path> out;
        const auto located = Locate(guestPath);
        if (!located)
            return out;
        const auto& [device, rest] = *located;
        for (const auto& overlay : device->overlays)
        {
            fs::path candidate = Join(overlay, rest);
            std::error_code ec;
            if (fs::is_directory(candidate, ec))
                out.push_back(std::move(candidate));
        }
        out.push_back(Join(device->root, rest));
        return out;
    }

    bool VfsIsReadOnly(const fs::path& hostPath)
    {
        std::lock_guard lock(s_vfsMutex);
        const std::string host = Lower(hostPath.lexically_normal().string());
        for (const Device& device : s_devices)
        {
            if (!device.readOnly)
                continue;
            if (host.starts_with(Lower(device.root.lexically_normal().string())))
                return true;
            for (const auto& overlay : device.overlays)
            {
                if (host.starts_with(Lower(overlay.lexically_normal().string())))
                    return true;
            }
        }
        return false;
    }

    const fs::path& VfsUserRoot()
    {
        return s_userRoot;
    }

    File::~File()
    {
        if (host != INVALID_HANDLE_VALUE)
            ::CloseHandle(host);
        if (deleteOnClose)
        {
            std::error_code ec;
            fs::remove(hostPath, ec);
        }
    }
}
