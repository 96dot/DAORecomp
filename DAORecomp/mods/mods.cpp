#include "stdafx.h"
#include "mods/mods.h"

#include <fstream>

#include "kernel/vfs.h"
#include "text.h"

#include "xenia/base/cvar.h" // toml++

namespace dao::mods
{
    namespace fs = std::filesystem;

    namespace
    {
        // A bigger mod.toml isn't one (it's read on every start).
        constexpr uintmax_t kMaxModTomlBytes = 1 << 20;

        // Whether a file is in a format of the PC version: a GFF file (models,
        // meshes, morphs, ...) for the PC platform, a DDS texture (the Xbox 360
        // version's are .xds) or a PC archive (its header in UTF-16).
        bool IsPcFile(const fs::path& path)
        {
            const std::string extension = Lower(Utf8(path.extension()));
            if (extension == ".dds" || extension == ".dazip")
                return true;
            char header[12] = {};
            std::ifstream file(path, std::ios::binary);
            if (!file.read(header, sizeof(header)))
                return false;
            const std::string_view head(header, sizeof(header));
            return head == std::string_view("GFF V4.0PC  ", 12) || head.starts_with(std::string_view("E\0R\0F\0", 6));
        }
    }

    ModInfo Describe(const fs::path& dir)
    {
        ModInfo info;
        info.folder = Utf8(dir.filename());
        info.name = info.folder;
        const auto manifest = dir / "mod.toml";
        std::error_code ec;
        if (fs::is_regular_file(manifest, ec) && fs::file_size(manifest, ec) > kMaxModTomlBytes)
            LOGW("Mod {}: mod.toml is too big to be one", info.folder);
        else if (fs::is_regular_file(manifest, ec))
        {
            try
            {
                std::ifstream file(manifest, std::ios::binary);
                const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                const toml::table table = toml::parse(text, Utf8(manifest));
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
        for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator();
             it.increment(ec))
        {
            if (!it->is_regular_file(ec) || it->path().filename() == "mod.toml")
                continue;
            info.fileCount++;
            if (IsPcFile(it->path()) && info.pcFiles++ == 0)
                info.pcExample = WideToUtf8(it->path().lexically_relative(dir).generic_wstring());
        }
        if (info.IsForPc())
            LOGW("Mod {}: made for the PC version ({} files such as {}); not loaded", info.folder, info.pcFiles,
                info.pcExample);
        return info;
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
            // Not the hidden work folder of mods being added.
            if (entry.is_directory(ec) && !entry.path().filename().native().starts_with(kHiddenPrefix))
                folders.push_back(entry.path());
        }
        std::sort(folders.begin(), folders.end());

        std::vector<ModInfo> out;
        for (const ModState& state : saved)
        {
            const auto it = std::find_if(folders.begin(), folders.end(),
                [&](const fs::path& p) { return _stricmp(Utf8(p.filename()).c_str(), state.folder.c_str()) == 0; });
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
            const fs::path folder = modsRoot / FromUtf8(it->folder);
            if (it->IsLoaded() && fs::is_directory(folder, ec))
                overlays.push_back(folder);
        }
        LOGI("Mods: {} enabled", overlays.size());
        kernel::VfsSetOverlays("\\Device\\Cdrom0", std::move(overlays));
    }
}
