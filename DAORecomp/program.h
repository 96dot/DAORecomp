#pragma once
// DAORecomp.exe itself, and starting programs from it.

namespace dao
{
    // The path of DAORecomp.exe; empty if Windows can't say.
    std::filesystem::path ProgramPath();

    // A path as one command-line argument: in quotes, with a '.' after a final
    // backslash (a drive's root), which would escape the closing quote.
    std::wstring QuoteArgument(const std::filesystem::path& path);

    // Starts `program` with `arguments` (after its own quoted path) and leaves it
    // running on its own, in `folder` (this one's if empty). False if it can't be
    // started, with GetLastError saying why.
    bool StartProgram(const std::filesystem::path& program, std::wstring_view arguments, DWORD flags = 0,
        const std::filesystem::path& folder = {});

    // Whether this runs under Wine or Proton.
    bool IsUnderWine();
}
