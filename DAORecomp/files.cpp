#include "stdafx.h"
#include "files.h"

#include <fstream>

#include "text.h"

namespace dao
{
    bool IsBadNameChar(char c)
    {
        return uint8_t(c) < 0x20 || std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos;
    }

    bool IsDeviceName(std::string_view name)
    {
        std::string base = Lower(name.substr(0, name.find('.')));
        while (!base.empty() && base.back() == ' ')
            base.pop_back();
        if (base == "con" || base == "prn" || base == "aux" || base == "nul")
            return true;
        if (!base.starts_with("com") && !base.starts_with("lpt"))
            return false;
        const std::string_view number = std::string_view(base).substr(3);
        // A digit, or a superscript 1, 2 or 3.
        return (number.size() == 1 && isdigit(uint8_t(number[0]))) || number == "\xC2\xB9" ||
            number == "\xC2\xB2" || number == "\xC2\xB3";
    }

    bool IsPlainName(std::string_view name)
    {
        // ("." and ".." end in a dot.)
        if (name.empty() || name.back() == '.' || name.back() == ' ' || IsDeviceName(name))
            return false;
        return std::ranges::none_of(name, IsBadNameChar);
    }

    bool IsPlainFolder(const std::filesystem::path& path)
    {
        const DWORD attributes = GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) &&
            !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
    }

    bool WriteFileAtomically(const std::filesystem::path& path, std::string_view data)
    {
        const std::filesystem::path part = std::filesystem::path(path).concat(L".part");
        bool written = false;
        {
            std::ofstream out(part, std::ios::binary | std::ios::trunc);
            written = out && out.write(data.data(), std::streamsize(data.size())) && out.flush();
        }
        std::error_code ec;
        if (written)
            std::filesystem::rename(part, path, ec);
        if (!written || ec)
        {
            std::filesystem::remove(part, ec);
            return false;
        }
        return true;
    }
}
