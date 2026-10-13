#include "stdafx.h"
#include "content/installed.h"
#include "content/content.h"
#include "files.h"
#include "kernel/title_update.h"
#include "text.h"

namespace fs = std::filesystem;

namespace dao::content
{
    namespace
    {
        // The add-ins in a package's folder: its folders with a manifest.xml.
        std::vector<std::string> AddIns(const fs::path& package)
        {
            std::vector<std::string> addIns;
            std::error_code ec;
            for (fs::directory_iterator it(package, ec), end; !ec && it != end; it.increment(ec))
            {
                const fs::path& inner = it->path();
                std::error_code fileError;
                if (IsPlainFolder(inner) && fs::is_regular_file(inner / "manifest.xml", fileError))
                    addIns.push_back(Utf8(inner.filename()));
            }
            return addIns;
        }
    }

    fs::path DlcFolder(const fs::path& userRoot)
    {
        return ContentFolder(userRoot, kTitleId, kContentMarketplace);
    }

    std::vector<InstalledPackage> ListDlc(const fs::path& userRoot)
    {
        std::vector<InstalledPackage> packages;
        const fs::path folder = DlcFolder(userRoot);
        std::error_code ec;
        for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
        {
            const std::string name = Utf8(it->path().filename());
            if (!IsContentName(name) || !IsPlainFolder(it->path()))
                continue;
            InstalledPackage package{ name, ReadDisplayName(folder, name), AddIns(it->path()) };
            if (package.name.empty())
                package.name = name;
            packages.push_back(std::move(package));
        }
        std::ranges::sort(packages, {}, [](const InstalledPackage& p) { return Lower(p.name); });
        return packages;
    }

    TitleUpdateState FindTitleUpdate(const fs::path& gameRoot)
    {
        // (Title Update 7's is about 3 MB: nothing bigger is hashed, or a link followed.)
        constexpr uintmax_t kMaxXexpSize = 64 << 20;
        const fs::path xexp = kernel::UpdateRoot(gameRoot) / "default.xexp";
        std::error_code ec;
        TitleUpdateState state{ fs::is_regular_file(xexp, ec) };
        const DWORD attributes = GetFileAttributesW(xexp.c_str());
        if (state.present && attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
            fs::file_size(xexp, ec) <= kMaxXexpSize && !ec)
            state.number = kernel::TitleUpdateNumber(xexp);
        return state;
    }
}
