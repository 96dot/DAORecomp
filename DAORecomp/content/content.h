#pragma once
// Where the game's content lives on the host, shared by XAM (which the game asks
// for it) and the launcher (which installs DLC): userdata/content/<title ID>/<content
// type>/<name>/, with the content's display name in <name>.name.

#include "text.h"

namespace dao::content
{
    // Content types (XCONTENTTYPE_*). Saves are type 1.
    constexpr uint32_t kContentSave = 0x00000001;
    constexpr uint32_t kContentMarketplace = 0x00000002; // downloadable content
    constexpr uint32_t kContentInstaller = 0x000B0000;   // title update

    // XCONTENT_DATA's displayName: 128 UTF-16 characters, big-endian.
    constexpr size_t kDisplayNameSize = 256;

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
}
