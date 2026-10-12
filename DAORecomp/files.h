#pragma once
// Files: the names Windows can hold.

namespace dao
{
    // A character no file name can have: a control character, or one of <>:"/\|?*
    // (separators, drive and stream colons, wildcards).
    bool IsBadNameChar(char c);

    // Whether Windows reserves the name for a device (CON, NUL, COM1...), also
    // with an extension.
    bool IsDeviceName(std::string_view name);

    // A name Windows holds as one file or folder, in its folder: not empty, "." or
    // "..", no IsBadNameChar, no trailing dot or space, not a device.
    bool IsPlainName(std::string_view name);
}
