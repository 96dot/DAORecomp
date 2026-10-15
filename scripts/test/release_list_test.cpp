// Tests the launcher's choice of update (DAORecomp/launcher/release_list.cpp):
// version tags, the update channels, and what makes a release unusable. Build and
// run it from the repo root (scripts/test/README.md has the command).
#include "stdafx.h"
#include "launcher/release_list.h"

#include <cstdio>

using namespace dao;
using namespace dao::launcher;

namespace
{
    int failures = 0;

    void Expect(bool ok, const char* what)
    {
        if (!ok)
        {
            std::printf("FAIL: %s\n", what);
            failures++;
        }
    }

    const std::string kDigest = "sha256:" + std::string(64, 'a');

    // A release as GitHub's list has it, with a usable installer unless changed.
    std::string Item(std::string_view tag, bool prerelease = false, bool draft = false,
        std::string_view installer = "DAORecomp.exe", std::string_view digest = kDigest)
    {
        return fmt::format(R"({{"tag_name":"{0}","name":"{0}","draft":{1},"prerelease":{2},"body":"notes",)"
                           R"("html_url":"https://github.com/96dot/DAORecomp/releases/tag/{0}","assets":[)"
                           R"({{"name":"{3}","size":1000,"digest":"{4}",)"
                           R"("browser_download_url":"https://github.com/96dot/DAORecomp/releases/download/{0}/{3}"}}]}})",
            tag, draft, prerelease, installer, digest);
    }

    // `item` with `from` replaced by `to` (once).
    std::string With(std::string item, std::string_view from, std::string_view to)
    {
        const size_t at = item.find(from);
        if (at != std::string::npos)
            item.replace(at, from.size(), to);
        return item;
    }

    std::string List(std::initializer_list<std::string> items)
    {
        std::string out = "[";
        for (const std::string& item : items)
            out += (out.size() > 1 ? "," : "") + item;
        return out + "]";
    }

    // The tag FindUpdate offers ("" for none, "error" if the list can't be read).
    std::string Offered(const std::string& json, std::string_view current, UpdateChannel channel)
    {
        std::optional<Release> update;
        if (!FindUpdate(json, current, channel, update))
            return "error";
        return update ? update->tag : "";
    }
}

int main()
{
    // Tags.
    Expect(ParseVersion("v0.9.0") == VersionNumber{ 0, 9, 0, kNotDevRelease }, "v0.9.0");
    Expect(ParseVersion("v0.10.0-dev.3") == VersionNumber{ 0, 10, 0, 3 }, "v0.10.0-dev.3");
    for (const char* bad : { "dev", "0.9.0", "v0.9", "v0.9.0-dirty", "v0.9.0-dev", "v0.9.0-dev.", "v0.9.0-dev.1-dirty",
             "v0.9.0-dev.x", "v0.09.0", "v0.9.0-dev.01", "v-1.0.0", "v0.9.0 ", "v99999999999.0.0",
             "v0.9.0-dev.2147483647" })
        Expect(!ParseVersion(bad), bad);
    Expect(*ParseVersion("v0.10.0-dev.1") < *ParseVersion("v0.10.0-dev.2"), "dev.1 < dev.2");
    Expect(*ParseVersion("v0.10.0-dev.9") < *ParseVersion("v0.10.0"), "a dev release comes before its release");
    Expect(*ParseVersion("v0.9.0") < *ParseVersion("v0.10.0-dev.1"), "and after the one before");

    const std::string list = List({ Item("v0.10.0-dev.2", true), Item("v0.10.0-dev.1", true),
        Item("v0.9.0", true), Item("v0.8.0", true) });
    // The release channel: 0.x releases are all marked pre-release, and still offered.
    Expect(Offered(list, "v0.8.0", UpdateChannel::Release) == "v0.9.0", "release channel: v0.9.0");
    Expect(Offered(list, "v0.9.0", UpdateChannel::Release) == "", "release channel: no dev releases");
    // On a dev release, the release channel waits for the release.
    Expect(Offered(list, "v0.10.0-dev.1", UpdateChannel::Release) == "", "release channel on a dev release");
    Expect(Offered(List({ Item("v0.10.0", true), Item("v0.10.0-dev.2", true) }), "v0.10.0-dev.1",
               UpdateChannel::Release) == "v0.10.0",
        "release channel: the release after a dev release");
    // The dev channel: the newest of either.
    Expect(Offered(list, "v0.9.0", UpdateChannel::Dev) == "v0.10.0-dev.2", "dev channel: newest dev release");
    Expect(Offered(list, "v0.10.0-dev.1", UpdateChannel::Dev) == "v0.10.0-dev.2", "dev channel: next dev release");
    Expect(Offered(List({ Item("v0.10.0", true), Item("v0.10.0-dev.2", true) }), "v0.10.0-dev.2",
               UpdateChannel::Dev) == "v0.10.0",
        "dev channel: the release after its dev releases");
    Expect(Offered(list, "v0.10.0-dev.2", UpdateChannel::Dev) == "", "dev channel: up to date");
    // From 1.0 on, a release marked pre-release is for the dev channel only.
    const std::string after = List({ Item("v1.1.0", true), Item("v1.0.1") });
    Expect(Offered(after, "v1.0.0", UpdateChannel::Release) == "v1.0.1", "1.x: pre-release skipped on release");
    Expect(Offered(after, "v1.0.0", UpdateChannel::Dev) == "v1.1.0", "1.x: pre-release offered on dev");

    // Unusable releases are skipped (an older usable one is offered instead).
    const std::string next = Item("v0.11.0");
    for (const std::string& bad : { Item("v0.11.0", false, true), Item("v0.11.0", false, false, "Other.exe"),
             Item("v0.11.0", false, false, "DAORecomp.exe", "sha256:abc"),
             Item("v0.11.0", false, false, "DAORecomp.exe", ""), Item("v0.11.0-dirty"), Item("vX"),
             With(next, "\"size\":1000", "\"size\":0"), With(next, "\"size\":1000", "\"size\":268435457"),
             With(next, "\"size\":1000", "\"size\":\"1000\""),
             With(next, "https://github.com/96dot/DAORecomp/releases/tag", "https://github.com/other/repo/releases/tag"),
             With(next, "https://github.com/96dot/DAORecomp/releases/download/", "https://example.com/"),
             With(next, "releases/download/v0.11.0", "releases/download/../../../other/repo/releases/download/v0.11.0"),
             With(next, "releases/download/v0.11.0", "releases/download/%2e%2e/v0.11.0") })
        Expect(Offered(List({ bad, Item("v0.10.0") }), "v0.9.0", UpdateChannel::Dev) == "v0.10.0",
            ("skipped: " + bad.substr(0, 40)).c_str());
    // The size limit itself is fine; a missing name is the tag; long notes are cut at a
    // line end.
    Expect(Offered(List({ With(next, "\"size\":1000", "\"size\":268435456") }), "v0.9.0", UpdateChannel::Dev) ==
            "v0.11.0",
        "installer at the size limit");
    {
        std::optional<Release> update;
        const std::string longNotes = std::string(20000, 'x') + "\\n" + std::string(20000, 'y');
        FindUpdate(List({ With(With(next, "\"name\":\"v0.11.0\"", "\"name\":\"\""), "\"body\":\"notes\"",
                       "\"body\":\"line one\\r\\n" + longNotes + "\"") }),
            "v0.9.0", UpdateChannel::Dev, update);
        Expect(update && update->name == "v0.11.0", "name falls back to the tag");
        Expect(update && update->notes == "line one\n...\n", "notes cut at a line end, without carriage returns");
    }

    // A list that can't be read, or a build with no version to compare.
    Expect(Offered("{", "v0.9.0", UpdateChannel::Dev) == "error", "bad JSON");
    Expect(Offered(list, "dev", UpdateChannel::Dev) == "error", "a build from source");

    if (failures)
        std::printf("%d failed\n", failures);
    else
        std::printf("all passed\n");
    return failures ? 1 : 0;
}
