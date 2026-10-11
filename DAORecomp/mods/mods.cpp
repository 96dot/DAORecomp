#include "stdafx.h"
#include "mods/mods.h"

#include <fstream>

#include "kernel/vfs.h"

#include "xenia/base/cvar.h" // toml++

namespace dao::mods
{
    namespace fs = std::filesystem;

    namespace
    {
        // Whether a file is in a format of the PC version: a GFF file (models,
        // meshes, morphs, ...) for the PC platform, a DDS texture (the Xbox 360
        // version's are .xds) or a PC archive (its header in UTF-16).
        bool IsPcFile(const fs::path& path)
        {
            std::string extension = path.extension().string();
            std::transform(extension.begin(), extension.end(), extension.begin(),
                [](char c) { return char(tolower(uint8_t(c))); });
            if (extension == ".dds" || extension == ".dazip")
                return true;
            char header[12] = {};
            std::ifstream file(path, std::ios::binary);
            if (!file.read(header, sizeof(header)))
                return false;
            const std::string_view head(header, sizeof(header));
            return head == std::string_view("GFF V4.0PC  ", 12) || head.starts_with(std::string_view("E\0R\0F\0", 6));
        }

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
                if (!it->is_regular_file() || it->path().filename() == "mod.toml")
                    continue;
                info.fileCount++;
                if (IsPcFile(it->path()) && info.pcFiles++ == 0)
                    info.pcExample = it->path().lexically_relative(dir).generic_string();
            }
            if (info.IsForPc())
                LOGW("Mod {}: made for the PC version ({} files such as {}); not loaded", info.folder, info.pcFiles,
                    info.pcExample);
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

    void Apply(const fs::path& modsRoot, const std::vector<ModInfo>& mods)
    {
        // Highest priority first: the last mod in the load order wins.
        std::vector<fs::path> overlays;
        for (auto it = mods.rbegin(); it != mods.rend(); ++it)
        {
            std::error_code ec;
            if (it->IsLoaded() && fs::is_directory(modsRoot / it->folder, ec))
                overlays.push_back(modsRoot / it->folder);
        }
        LOGI("Mods: {} enabled", overlays.size());
        kernel::VfsSetOverlays("\\Device\\Cdrom0", std::move(overlays));
    }
}
