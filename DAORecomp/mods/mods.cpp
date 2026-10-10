#include "stdafx.h"
#include "mods/mods.h"
#include "kernel/vfs.h"

#include "xenia/base/cvar.h" // toml++

namespace dao::mods
{
    namespace fs = std::filesystem;

    namespace
    {
        ModInfo Describe(const fs::path& dir)
        {
            ModInfo info;
            info.folder = dir.filename().string();
            info.name = info.folder;
            const auto manifest = dir / "mod.toml";
            if (fs::exists(manifest))
            {
                try
                {
                    const toml::table table = toml::parse_file(manifest.string());
                    info.name = table["name"].value_or(info.name);
                    info.version = table["version"].value_or(std::string());
                    info.author = table["author"].value_or(std::string());
                    info.description = table["description"].value_or(std::string());
                }
                catch (const toml::parse_error& error)
                {
                    LOGW("Mod {}: mod.toml: {}", info.folder, error.description());
                }
            }
            std::error_code ec;
            for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator();
                 it.increment(ec))
            {
                if (it->is_regular_file() && it->path().filename() != "mod.toml")
                    info.fileCount++;
            }
            return info;
        }
    }

    fs::path ModsRoot(const fs::path& gameRoot)
    {
        return gameRoot.parent_path() / "mods";
    }

    std::vector<ModInfo> Scan(const fs::path& modsRoot, const std::vector<ModState>& saved)
    {
        std::error_code ec;
        fs::create_directories(modsRoot, ec);

        std::vector<fs::path> folders;
        for (const auto& entry : fs::directory_iterator(modsRoot, ec))
        {
            if (entry.is_directory())
                folders.push_back(entry.path());
        }
        std::sort(folders.begin(), folders.end());

        std::vector<ModInfo> out;
        for (const ModState& state : saved)
        {
            const auto it = std::find_if(folders.begin(), folders.end(),
                [&](const fs::path& p) { return _stricmp(p.filename().string().c_str(), state.folder.c_str()) == 0; });
            if (it == folders.end())
                continue;
            ModInfo info = Describe(*it);
            info.enabled = state.enabled;
            out.push_back(std::move(info));
            folders.erase(it);
        }
        for (const auto& folder : folders)
            out.push_back(Describe(folder));
        return out;
    }

    std::vector<ModState> ToState(const std::vector<ModInfo>& mods)
    {
        std::vector<ModState> out;
        for (const ModInfo& mod : mods)
            out.push_back({ mod.folder, mod.enabled });
        return out;
    }

    void Apply(const fs::path& modsRoot, const std::vector<ModState>& mods)
    {
        // Highest priority first: the last mod in the load order wins.
        std::vector<fs::path> overlays;
        for (auto it = mods.rbegin(); it != mods.rend(); ++it)
        {
            std::error_code ec;
            if (it->enabled && fs::is_directory(modsRoot / it->folder, ec))
                overlays.push_back(modsRoot / it->folder);
        }
        LOGI("Mods: {} enabled", overlays.size());
        kernel::VfsSetOverlays("\\Device\\Cdrom0", std::move(overlays));
    }
}
