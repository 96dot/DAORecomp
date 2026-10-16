#pragma once
// Where the game's content lives on the host, shared by XAM (which the game asks
// for it) and the launcher (which installs DLC): userdata/content/<title ID>/<content
// type>/<name>/, with the content's display name in <name>.name.

#include "files.h"
#include "text.h"

namespace dao::content
{
    // The game's title ID (installer/deps.py GAME_TITLE_ID).
    constexpr uint32_t kTitleId = 0x454108C0;

    // Content types (XCONTENTTYPE_*). Saves are type 1.
    constexpr uint32_t kContentSave = 0x00000001;
    constexpr uint32_t kContentMarketplace = 0x00000002; // downloadable content
    constexpr uint32_t kContentInstaller = 0x000B0000;   // title update

    // XCONTENT_DATA's displayName: 128 UTF-16 characters, big-endian.
    constexpr size_t kDisplayNameSize = 256;

    // A name content can have as its folder: a plain one (IsPlainName) without "~",
    // so it's neither another's short (8.3) name nor a restore's <name>~current (see
    // save_backups.h), and not named as the files kept beside content (<name>.name) or
    // a save's backup (SetSaveAside's <name>.new and <name>.writing).
    inline bool IsContentName(std::string_view name)
    {
        if (!IsPlainName(name) || name.find('~') != std::string_view::npos)
            return false;
        const std::string lower = Lower(name);
        for (std::string_view suffix : { ".name", ".new", ".writing" })
        {
            if (lower.ends_with(suffix))
                return false;
        }
        return true;
    }

    inline std::filesystem::path ContentFolder(const std::filesystem::path& userRoot, uint32_t titleId,
        uint32_t contentType)
    {
        return userRoot / "content" / fmt::format("{:08X}", titleId) / fmt::format("{:08X}", contentType);
    }

    // Writes <folder>/<name>.name: the display name (kDisplayNameSize bytes) as the
    // game passes it.
    inline void WriteDisplayName(const std::filesystem::path& folder, std::string_view name, const void* displayName)
    {
        if (FILE* f = _wfopen((folder / FromUtf8(std::string(name) + ".name")).c_str(), L"wb"))
        {
            fwrite(displayName, 1, kDisplayNameSize, f);
            fclose(f);
        }
    }

    // Reads <folder>/<name>.name back, as UTF-8 up to its first null (control
    // characters left out: the name is the save's or package's to choose); empty if
    // there's none.
    inline std::string ReadDisplayName(const std::filesystem::path& folder, std::string_view name)
    {
        uint8_t data[kDisplayNameSize] = {};
        size_t size = 0;
        if (FILE* f = _wfopen((folder / FromUtf8(std::string(name) + ".name")).c_str(), L"rb"))
        {
            size = fread(data, 1, sizeof(data), f);
            fclose(f);
        }
        std::wstring text;
        for (size_t i = 0; i + 1 < size; i += 2)
        {
            const wchar_t c = wchar_t(data[i] << 8 | data[i + 1]);
            if (!c)
                break;
            if (c >= 0x20 && c != 0x7F)
                text += c;
        }
        return WideToUtf8(text);
    }
}
