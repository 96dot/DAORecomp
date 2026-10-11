#pragma once
// Reading mods' files, shared by describing, adding and converting mods: what a
// file is from its first bytes, and the names and folders that mean something.

namespace dao::mods
{
    // A whole file, if it's at most `maxBytes`.
    std::optional<std::vector<uint8_t>> ReadAll(const std::filesystem::path& path, uintmax_t maxBytes);

    // A file's first `bytes` (fewer if it's shorter).
    std::vector<uint8_t> ReadHead(const std::filesystem::path& path, size_t bytes);

    // Enough of a file for IsPcGff and IsPcArchive.
    constexpr size_t kFormatHeadBytes = 12;

    // From a file's first bytes: a GFF file for the PC platform (the Xbox 360
    // version's say "X360"), or a PC archive (ERF, its header in UTF-16; the 360
    // version's are ERF V2.1, in ASCII).
    bool IsPcGff(std::span<const uint8_t> head);
    bool IsPcArchive(std::span<const uint8_t> head);

    // Whether one of the folders on a relative path is `folder` (lower case).
    bool IsUnder(const std::filesystem::path& relative, std::string_view folder);

    // Read-mes, pictures and links: files that aren't for the game.
    bool IsDocument(const std::filesystem::path& path);

    // Whether Windows reserves the name for a device (CON, NUL, COM1...), also
    // with an extension.
    bool IsDeviceName(std::string_view name);

    // Whether a path is a link (symbolic, or a junction) rather than what it leads to.
    bool IsLink(const std::filesystem::path& path);

    // Removes a folder and what's in it; a link, only the link (not what it leads to).
    void RemoveTree(const std::filesystem::path& path);

    // A work folder of this launcher's (made empty), locked while it's there: a
    // second launcher's RemoveUnlockedFolders leaves it alone. Removed at the end.
    class LockedFolder
    {
    public:
        explicit LockedFolder(std::filesystem::path dir);
        ~LockedFolder();
        LockedFolder(const LockedFolder&) = delete;
        LockedFolder& operator=(const LockedFolder&) = delete;

        const std::filesystem::path& Dir() const { return dir_; }

    private:
        std::filesystem::path dir_;
        void* lock_;   // a HANDLE
    };

    // Removes the folders in `parent` that aren't locked (left by work that was
    // interrupted), then `parent` if it's empty.
    void RemoveUnlockedFolders(const std::filesystem::path& parent);

    // An attribute's value (as written, entities and all) in an XML element's text
    // ("<Name a="1" b="2""), or nothing.
    std::string_view XmlAttribute(std::string_view element, std::string_view name);
}
