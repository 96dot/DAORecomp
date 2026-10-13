#include "stdafx.h"
#include "launcher/launcher_internal.h"

#include "program.h"

namespace dao::launcher
{
    namespace
    {
        constexpr const char* kRestorePopup = "Restore save";

        // A time as the player's clock has it: "12 Oct 2026, 13:55".
        std::string LocalTime(std::chrono::sys_seconds time)
        {
            if (time == std::chrono::sys_seconds{})
                return "unknown time";
            // FILETIME counts 100 ns ticks from 1601.
            constexpr int64_t kSecondsFrom1601 = 11644473600, kTicksPerSecond = 10'000'000;
            const uint64_t ticks = uint64_t(time.time_since_epoch().count() + kSecondsFrom1601) * kTicksPerSecond;
            const FILETIME utc = { DWORD(ticks), DWORD(ticks >> 32) };
            SYSTEMTIME system{}, local{};
            if (!FileTimeToSystemTime(&utc, &system))
                return "unknown time";
            if (!SystemTimeToTzSpecificLocalTime(nullptr, &system, &local))
                local = system;
            constexpr const char* kMonths[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct",
                "Nov", "Dec" };
            return fmt::format("{} {} {}, {:02}:{:02}", local.wDay, kMonths[std::clamp<int>(local.wMonth, 1, 12) - 1],
                local.wYear, local.wHour, local.wMinute);
        }

        // Where and when: "The Fade, 12 Oct 2026, 13:55".
        std::string WhereAndWhen(const content::SaveVersion& save)
        {
            const std::string time = LocalTime(save.saved);
            return save.area.empty() ? time : fmt::format("{}, {}", save.area, time);
        }

        const SaveBackupRow* SelectedBackup(const State& st)
        {
            return st.selectedBackup >= 0 && st.selectedBackup < int(st.saveBackups.size())
                ? &st.saveBackups[st.selectedBackup]
                : nullptr;
        }

        // The list (a few small files), with its text made once.
        void ReadSaveBackups(State& st)
        {
            const SaveBackupRow* selected = SelectedBackup(st);
            const std::string selectedName = selected ? selected->backup.name : std::string();
            st.saveBackups.clear();
            for (content::SaveBackup& backup : content::ListSaveBackups(st.userRoot, !OtherInstanceRunning()))
            {
                SaveBackupRow row{ std::move(backup) };
                row.backupText = WhereAndWhen(row.backup.backup);
                row.currentText = row.backup.current.exists ? WhereAndWhen(row.backup.current) : "Deleted";
                st.saveBackups.push_back(std::move(row));
            }
            const auto it = std::ranges::find(st.saveBackups, selectedName,
                [](const SaveBackupRow& row) { return row.backup.name; });
            st.selectedBackup = selectedName.empty() || it == st.saveBackups.end() ? -1
                                                                                   : int(it - st.saveBackups.begin());
        }

        void Restore(State& st, const SaveBackupRow& row)
        {
            const std::string what = fmt::format("{}'s save from {}", row.backup.backup.character,
                LocalTime(row.backup.backup.saved));
            // (The game in another window could be writing the save.)
            if (OtherInstanceRunning())
            {
                st.saveMessages = { { "Can't restore while the game (or another launcher) is running: close it "
                                      "first.", true } };
                return;
            }
            const content::RestoreResult result = content::RestoreSaveBackup(st.userRoot, row.backup.name);
            if (!result.restored)
                st.saveMessages = { { fmt::format("Can't restore {}: {}.", what, result.problem), true } };
            else
            {
                st.saveMessages = { { fmt::format("Restored {}.", what) } };
                if (!result.problem.empty())
                    st.saveMessages.push_back({ fmt::format("But {}.", result.problem), true });
            }
            ReadSaveBackups(st);
        }

        // Asks before restoring the selected backup.
        void RestorePopup(State& st)
        {
            if (!BeginDialog(st, kRestorePopup))
                return;
            bool close = !SelectedBackup(st);
            if (!close)
            {
                // A copy: restoring reads the list again.
                const SaveBackupRow row = *SelectedBackup(st);
                ImGui::PushTextWrapPos(0.0f);
                ImGui::Text("Restore %s's save from %s?", row.backup.backup.character.c_str(),
                    row.backupText.c_str());
                ImGui::PopTextWrapPos();
                HelpText(row.backup.current.exists
                        ? fmt::format("The save there now ({}) becomes the backup, so restoring again switches back.",
                              row.currentText)
                              .c_str()
                        : "The save was deleted, so the backup is put back as it is.");
                if (row.backup.currentUnfinished)
                    ColoredText(kWarning, "The game stopped while writing the save there now, so it may be damaged.");
                ImGui::Spacing();
                if (ImGui::Button("Restore"))
                {
                    Restore(st, row);
                    close = true;
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel") || CancelPressed())
                    close = true;
                // Cancel is the default.
                if (ImGui::IsWindowAppearing())
                    ImGui::SetKeyboardFocusHere(-1);
            }
            if (close)
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        void SaveBackupsList(State& st)
        {
            HelpText("Each time the game saves over a save, the one it replaces is kept here: one backup for each "
                     "save. Restoring a backup puts it back, and the save there until then becomes the backup, so "
                     "restoring again switches back.");
            ImGui::Spacing();
            Messages(st.saveMessages);
            ImGui::Spacing();

            ImGui::BeginDisabled(!SelectedBackup(st));
            if (ImGui::Button("Restore"))
                ImGui::OpenPopup(kRestorePopup);
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Refresh"))
                ReadSaveBackups(st);
            ImGui::SameLine();
            if (ImGui::Button("Open backups folder"))
                OpenFolder(content::SaveBackupsFolder(st.userRoot));
            RestorePopup(st);
            ImGui::Spacing();

            if (st.saveBackups.empty())
            {
                HelpText("No backups yet: the first is made when the game saves over a save.");
                return;
            }
            constexpr ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
            if (!BeginNavTable("backups", 3, flags, ImVec2(0, ImGui::GetContentRegionAvail().y)))
                return;
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Character", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Backup", ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableSetupColumn("The save there now", ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableHeadersRow();
            ImGuiListClipper clipper;
            clipper.Begin(int(st.saveBackups.size()));
            while (clipper.Step())
            {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++)
                {
                    const SaveBackupRow& row = st.saveBackups[i];
                    ImGui::PushID(i);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    // (The name drawn apart from the row's label: it's the save's to choose.)
                    if (ImGui::Selectable("##row", st.selectedBackup == i, ImGuiSelectableFlags_SpanAllColumns))
                        st.selectedBackup = i;
                    ImGui::SameLine();
                    ImGui::TextUnformatted(row.backup.backup.character.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(row.backupText.c_str());
                    ImGui::TableNextColumn();
                    ImGui::PushStyleColor(ImGuiCol_Text,
                        ImGui::ColorConvertU32ToFloat4(row.backup.current.exists ? kText : kTextDim));
                    ImGui::TextUnformatted(row.currentText.c_str());
                    ImGui::PopStyleColor();
                    if (row.backup.currentUnfinished)
                    {
                        ImGui::SameLine();
                        ImGui::PushStyleColor(ImGuiCol_Text, kWarning);
                        ImGui::TextUnformatted("(not finished)");
                        ImGui::PopStyleColor();
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }
    }

    void SavesView(State& st, const Layout& layout)
    {
        // Read as the view opens, and on Refresh.
        if (st.viewChanged)
        {
            st.saveMessages.clear();
            ReadSaveBackups(st);
        }
        PanelFrame(st, layout, "Save Backups", nullptr);
        ShowContents(st, layout, kListLeft, SaveBackupsList);
    }
}
