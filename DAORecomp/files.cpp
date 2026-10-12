#include "stdafx.h"
#include "files.h"

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
}
