#include "stdafx.h"
#include "launcher/release_list.h"

#include <charconv>

#include <rapidjson/document.h>

#include "version.h"

namespace dao
{
    UpdateChannel DefaultUpdateChannel()
    {
        const auto version = launcher::ParseVersion(Version());
        return version && launcher::IsDevRelease(*version) ? UpdateChannel::Dev : UpdateChannel::Release;
    }
}

namespace dao::launcher
{
    namespace
    {
        constexpr std::string_view kReleaseDownloadPrefix = "https://github.com/96dot/DAORecomp/releases/download/";
        // Sanity bound on the installer's size (~15 MB at v0.2.0).
        constexpr uint64_t kMaxInstallerSize = 256u << 20;
        constexpr std::string_view kDigestPrefix = "sha256:";
        constexpr std::string_view kDevSuffix = "-dev.";
        // The notes shown in the launcher are cut to about this long.
        constexpr size_t kMaxNotes = 16 * 1024;

        std::optional<std::array<uint8_t, 32>> ParseSha256(std::string_view digest)
        {
            if (!digest.starts_with(kDigestPrefix))
                return std::nullopt;
            digest.remove_prefix(kDigestPrefix.size());
            std::array<uint8_t, 32> out;
            if (digest.size() != out.size() * 2)
                return std::nullopt;
            for (size_t i = 0; i < out.size(); i++)
            {
                const auto [end, ec] = std::from_chars(digest.data() + i * 2, digest.data() + i * 2 + 2, out[i], 16);
                if (ec != std::errc() || end != digest.data() + i * 2 + 2)
                    return std::nullopt;
            }
            return out;
        }

        std::string_view String(const rapidjson::Value& object, const char* key)
        {
            const auto it = object.FindMember(key);
            if (it == object.MemberEnd() || !it->value.IsString())
                return {};
            return std::string_view(it->value.GetString(), it->value.GetStringLength());
        }

        // Whether `key` is present and anything but false.
        bool Flag(const rapidjson::Value& object, const char* key)
        {
            const auto it = object.FindMember(key);
            return it != object.MemberEnd() && !(it->value.IsBool() && !it->value.GetBool());
        }

        // A release's notes for the launcher: without carriage returns, cut at a
        // line end (or a whole character) if very long.
        std::string Notes(std::string_view body)
        {
            std::string notes;
            for (const char c : body.substr(0, kMaxNotes))
            {
                if (c != '\r')
                    notes += c;
            }
            if (body.size() > kMaxNotes)
            {
                if (const size_t line = notes.rfind('\n'); line != std::string::npos)
                    notes.resize(line + 1);
                else
                {
                    while (!notes.empty() && (uint8_t(notes.back()) & 0xC0) == 0x80)
                        notes.pop_back();
                    if (!notes.empty() && uint8_t(notes.back()) >= 0xC0)
                        notes.pop_back();
                    notes += '\n';
                }
                notes += "...\n";
            }
            return notes;
        }

        // Whether a release's page or download address is under `prefix`, with no
        // "." or ".." path parts or escapes past it that could lead elsewhere.
        bool UnderPrefix(std::string_view url, std::string_view prefix)
        {
            if (!url.starts_with(prefix))
                return false;
            url.remove_prefix(prefix.size());
            return url.find("..") == std::string_view::npos && url.find("/./") == std::string_view::npos &&
                !url.starts_with("./") && url.find('%') == std::string_view::npos &&
                url.find('\\') == std::string_view::npos;
        }

        // A number of digits at `at` (no sign, no leading zero but "0"), into `out`.
        bool ParseNumber(const char*& at, const char* end, int& out)
        {
            if (at == end || *at < '0' || *at > '9' || (*at == '0' && at + 1 != end && at[1] >= '0' && at[1] <= '9'))
                return false;
            const auto [next, ec] = std::from_chars(at, end, out);
            if (ec != std::errc())
                return false;
            at = next;
            return true;
        }
    }

    std::optional<VersionNumber> ParseVersion(std::string_view tag)
    {
        if (!tag.starts_with('v'))
            return std::nullopt;
        VersionNumber version;
        const char* at = tag.data() + 1;
        const char* end = tag.data() + tag.size();
        for (size_t i = 0; i < 3; i++)
        {
            if (i > 0 && (at == end || *at++ != '.'))
                return std::nullopt;
            if (!ParseNumber(at, end, version[i]))
                return std::nullopt;
        }
        version[3] = kNotDevRelease;
        if (std::string_view(at, end).starts_with(kDevSuffix))
        {
            at += kDevSuffix.size();
            // (kNotDevRelease itself would read as a release.)
            if (!ParseNumber(at, end, version[3]) || version[3] == kNotDevRelease)
                return std::nullopt;
        }
        if (at != end)
            return std::nullopt;
        return version;
    }

    bool FindUpdate(std::string_view json, std::string_view current, UpdateChannel channel,
        std::optional<Release>& update)
    {
        update.reset();
        const auto currentVersion = ParseVersion(current);
        rapidjson::Document document;
        // Iterative: deeply nested input can't exhaust the stack.
        document.Parse<rapidjson::kParseIterativeFlag>(json.data(), json.size());
        if (!currentVersion || document.HasParseError() || !document.IsArray())
            return false;

        VersionNumber newest = *currentVersion;
        for (const rapidjson::Value& item : document.GetArray())
        {
            if (!item.IsObject() || Flag(item, "draft"))
                continue;
            const auto version = ParseVersion(String(item, "tag_name"));
            if (!version || *version <= newest)
                continue;
            // The release channel skips dev releases, and from 1.0 on also releases
            // marked pre-release on GitHub (every 0.x release is marked so).
            if (channel == UpdateChannel::Release &&
                (IsDevRelease(*version) || ((*version)[0] > 0 && Flag(item, "prerelease"))))
                continue;
            const std::string_view page = String(item, "html_url");
            const auto assets = item.FindMember("assets");
            if (!UnderPrefix(page, kReleasePagePrefix) || assets == item.MemberEnd() || !assets->value.IsArray())
                continue;
            for (const rapidjson::Value& asset : assets->value.GetArray())
            {
                if (!asset.IsObject() || String(asset, "name") != kInstallerName)
                    continue;
                const std::string_view url = String(asset, "browser_download_url");
                const auto size = asset.FindMember("size");
                const auto sha256 = ParseSha256(String(asset, "digest"));
                if (!UnderPrefix(url, kReleaseDownloadPrefix) || size == asset.MemberEnd() ||
                    !size->value.IsUint64() || size->value.GetUint64() == 0 ||
                    size->value.GetUint64() > kMaxInstallerSize || !sha256)
                    continue;
                Release release;
                release.tag = String(item, "tag_name");
                release.name = String(item, "name");
                if (release.name.empty())
                    release.name = release.tag;
                release.notes = Notes(String(item, "body"));
                release.pageUrl = page;
                release.installerUrl = url;
                release.installerSize = size->value.GetUint64();
                release.installerSha256 = *sha256;
                update = std::move(release);
                newest = *version;
                break;
            }
        }
        return true;
    }
}
