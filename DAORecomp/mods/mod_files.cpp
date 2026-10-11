#include "stdafx.h"
#include "mods/mod_files.h"

#include <fstream>

#include "text.h"

namespace dao::mods
{
    namespace fs = std::filesystem;

    std::optional<std::vector<uint8_t>> ReadAll(const fs::path& path, uintmax_t maxBytes)
    {
        std::error_code ec;
        const uintmax_t size = fs::file_size(path, ec);
        if (ec || size > maxBytes)
            return std::nullopt;
        std::ifstream file(path, std::ios::binary);
        std::vector<uint8_t> data(static_cast<size_t>(size));
        if (!file.read(reinterpret_cast<char*>(data.data()), std::streamsize(size)))
            return std::nullopt;
        return data;
    }

    std::vector<uint8_t> ReadHead(const fs::path& path, size_t bytes)
    {
        std::vector<uint8_t> head(bytes);
        std::ifstream file(path, std::ios::binary);
        file.read(reinterpret_cast<char*>(head.data()), std::streamsize(bytes));
        head.resize(size_t(file.gcount()));
        return head;
    }

    bool IsPcGff(std::span<const uint8_t> head)
    {
        return head.size() >= 12 && memcmp(head.data(), "GFF V4.0PC  ", 12) == 0;
    }

    bool IsPcArchive(std::span<const uint8_t> head)
    {
        return head.size() >= 6 && memcmp(head.data(), "E\0R\0F\0", 6) == 0;
    }

    bool IsUnder(const fs::path& relative, std::string_view folder)
    {
        return std::ranges::any_of(relative, [&](const fs::path& part) { return Lower(Utf8(part)) == folder; });
    }

    bool IsDocument(const fs::path& path)
    {
        static constexpr std::string_view kDocuments[] = { ".txt", ".md", ".pdf", ".rtf", ".doc", ".docx", ".htm",
            ".html", ".url", ".jpg", ".jpeg", ".png", ".bmp", ".gif", ".webp" };
        return std::ranges::find(kDocuments, Lower(Utf8(path.extension()))) != std::end(kDocuments);
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

    bool IsLink(const fs::path& path)
    {
        std::error_code ec;
        const fs::file_type type = fs::symlink_status(path, ec).type();
        return type == fs::file_type::symlink || type == fs::file_type::junction;
    }

    void RemoveTree(const fs::path& path)
    {
        std::error_code ec;
        if (IsLink(path))
            fs::remove(path, ec);
        else
            fs::remove_all(path, ec);
    }

    namespace
    {
        constexpr const wchar_t* kLockFile = L"lock";
    }

    LockedFolder::LockedFolder(fs::path dir) : dir_(std::move(dir))
    {
        RemoveTree(dir_);
        std::error_code ec;
        fs::create_directories(dir_, ec);
        // Shared with no one, so it can't be deleted while open.
        lock_ = CreateFileW((dir_ / kLockFile).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    }

    LockedFolder::~LockedFolder()
    {
        if (lock_ != INVALID_HANDLE_VALUE)
            CloseHandle(lock_);
        RemoveTree(dir_);
        std::error_code ec;
        fs::remove(dir_.parent_path(), ec); // Only if empty.
    }

    void RemoveUnlockedFolders(const fs::path& parent)
    {
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(parent, ec))
        {
            // A running one's lock can't be deleted.
            if (!IsLink(entry.path()))
                fs::remove(entry.path() / kLockFile, ec);
            if (!fs::exists(entry.path() / kLockFile, ec))
                RemoveTree(entry.path());
        }
        fs::remove(parent, ec); // Only if empty.
    }

    std::string_view XmlAttribute(std::string_view element, std::string_view name)
    {
        for (size_t at = element.find(name); at != std::string_view::npos; at = element.find(name, at + 1))
        {
            const size_t value = at + name.size();
            if (at == 0 || !isspace(uint8_t(element[at - 1])) || element.substr(value, 2) != "=\"")
                continue;
            const size_t end = element.find('"', value + 2);
            return end == std::string_view::npos ? std::string_view() : element.substr(value + 2, end - value - 2);
        }
        return {};
    }
}
