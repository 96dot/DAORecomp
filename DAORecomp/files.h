#pragma once
// Files: the names Windows can hold, and writing one whole.

namespace dao
{
    // A character no file name can have: a control character, or one of <>:"/\|?*
    // (separators, drive and stream colons, wildcards).
    bool IsBadNameChar(char c);

    // Whether Windows reserves the name (UTF-8) for a device (CON, NUL, COM1...),
    // also with an extension.
    bool IsDeviceName(std::string_view name);

    // A name (UTF-8) Windows holds as one file or folder, in its folder: not empty, "." or
    // "..", no IsBadNameChar, no trailing dot or space, not a device.
    bool IsPlainName(std::string_view name);

    // Writes `data` to <path>.part and renames that over `path`, so `path` is never
    // a partial file (if either fails, the old one stays). False if it fails, and
    // then the .part is removed.
    bool WriteFileAtomically(const std::filesystem::path& path, std::string_view data);
}
