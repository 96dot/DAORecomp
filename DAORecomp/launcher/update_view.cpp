#include "stdafx.h"
#include "launcher/launcher_internal.h"

#include "version.h"

namespace dao::launcher
{
    namespace
    {
        // Text without Markdown's emphasis and code marks, links as their text.
        std::string PlainText(std::string_view text)
        {
            std::string out;
            // The next "](" (searched forward only, so this stays linear).
            size_t close = text.find("](");
            for (size_t i = 0; i < text.size(); i++)
            {
                if (text[i] == '*' || text[i] == '`')
                    continue;
                if (text[i] == '[' && close != std::string_view::npos)
                {
                    if (close < i)
                        close = text.find("](", i);
                    const size_t end = close == std::string_view::npos ? close : text.find(')', close);
                    if (end == std::string_view::npos)
                        close = std::string_view::npos; // No more links.
                    else
                    {
                        out += text.substr(i + 1, close - i - 1);
                        i = end;
                        continue;
                    }
                }
                out += text[i];
            }
            return out;
        }

        // A release's notes (GitHub Markdown), simply: headings as sections, list
        // items as bullets, nesting by indentation (two spaces a level, up to four).
        std::vector<NoteLine> ParseNotes(std::string_view notes)
        {
            std::vector<NoteLine> lines;
            while (!notes.empty())
            {
                const size_t end = notes.find('\n');
                std::string_view line = notes.substr(0, end);
                notes.remove_prefix(end == std::string_view::npos ? notes.size() : end + 1);
                const size_t indent = line.find_first_not_of(' ');
                if (indent == std::string_view::npos)
                {
                    lines.push_back({ NoteLine::Kind::Blank });
                    continue;
                }
                line.remove_prefix(indent);
                if (line.starts_with('#'))
                {
                    line.remove_prefix(std::min(line.find_first_not_of('#'), line.size()));
                    line.remove_prefix(std::min(line.find_first_not_of(' '), line.size()));
                    lines.push_back({ NoteLine::Kind::Heading, 0, PlainText(line) });
                    continue;
                }
                const bool item = line.starts_with("- ") || line.starts_with("* ");
                if (item)
                    line.remove_prefix(2);
                const int level = int(std::min<size_t>(indent / 2, 4));
                lines.push_back({ item ? NoteLine::Kind::Item : NoteLine::Kind::Text, level, PlainText(line) });
            }
            return lines;
        }

        void ReleaseNotes(const std::vector<NoteLine>& lines)
        {
            for (const NoteLine& line : lines)
            {
                if (line.kind == NoteLine::Kind::Blank)
                {
                    ImGui::Spacing();
                    continue;
                }
                if (line.kind == NoteLine::Kind::Heading)
                {
                    Section(line.text.c_str());
                    continue;
                }
                const float shift = float(line.level) * ImGui::GetStyle().IndentSpacing;
                if (shift > 0.0f)
                    ImGui::Indent(shift);
                if (line.kind == NoteLine::Kind::Item)
                {
                    ImGui::Bullet();
                    ImGui::SameLine();
                }
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(line.text.c_str());
                ImGui::PopTextWrapPos();
                if (shift > 0.0f)
                    ImGui::Unindent(shift);
            }
        }

        // The update's page: what it is, downloading and starting its installer
        // (the launcher then closes, see Run), what's new.
        void UpdateContents(State& st)
        {
            Updater& updater = *st.updater;
            const Release* found = FoundRelease(st);
            if (!found)
                return;
            const Release& release = *found;
            const Updater::State state = updater.GetState();
            Section(release.name.c_str());
            ImGui::Text("This is %s; %s is available.", Version(), release.tag.c_str());
            if (const auto version = ParseVersion(release.tag); version && IsDevRelease(*version))
                HelpText("A dev release: a test build, published between releases. It can have bugs.");
            ImGui::Spacing();
            if (!updater.CanInstall())
            {
                if (ImGui::Button("Open the release page"))
                    updater.OpenReleasePage();
                HelpText("The launcher can't run its installer here (under Wine or Proton, or in a folder the "
                         "installer didn't make): see the README's \"Updating\".");
            }
            else if (state == Updater::State::Downloading)
            {
                const float progress = updater.GetProgress();
                ImGui::ProgressBar(progress, ImVec2(RowWidth(), 0),
                    fmt::format("Downloading {}%", int(progress * 100.0f)).c_str());
            }
            else if (state == Updater::State::Ready)
                ImGui::TextUnformatted("Starting the installer...");
            else
            {
                if (state == Updater::State::Failed)
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, kError);
                    ImGui::TextWrapped("The download failed: %s.", updater.GetError().c_str());
                    ImGui::PopStyleColor();
                }
                if (ImGui::Button(state == Updater::State::Failed ? "Try again" : "Download and install"))
                    updater.Download();
                ImGui::SameLine();
                if (ImGui::Button("Release page"))
                    updater.OpenReleasePage();
                HelpText(fmt::format("Downloads its installer ({:.0f} MB) from GitHub, checks it against its "
                                     "published checksum and runs it on this folder: it rebuilds the game in a few "
                                     "minutes and keeps your saves, settings and mods. The launcher closes while it "
                                     "runs.",
                    double(release.installerSize) / (1 << 20))
                             .c_str());
            }
            ReleaseNotes(st.releaseNotes);
        }
    }

    // The newer release the updater found, if any (copied once: it doesn't
    // change after).
    const Release* FoundRelease(State& st)
    {
        if (!st.updater->HasRelease())
        {
            // (None now: also after one was found, if the channel changed.)
            st.release.reset();
            st.releaseNotes.clear();
        }
        else if (!st.release)
        {
            st.release = st.updater->GetRelease();
            st.releaseNotes = ParseNotes(st.release->notes);
        }
        return st.release ? &*st.release : nullptr;
    }

    void UpdateView(State& st, const Layout& layout)
    {
        PanelFrame(st, layout, "Update", nullptr);
        ShowContents(st, layout, kListLeft, UpdateContents);
    }
}
