#include "stdafx.h"
#include "launcher/launcher_internal.h"

#include "content/install.h"
#include "text.h"

namespace dao::launcher
{
    namespace
    {
        // Mod folders' names compare as Windows' do, ignoring case.
        bool SameFolder(const std::string& a, const std::string& b)
        {
            return _stricmp(a.c_str(), b.c_str()) == 0;
        }
    }

    bool AddingMods(const State& st)
    {
        return st.modInstall.valid();
    }

    bool ConvertingMod(const State& st)
    {
        return st.modConvert.valid();
    }

    // Mods being added or converted: the game waits for them, as do other changes.
    bool ModsBusy(const State& st)
    {
        return AddingMods(st) || ConvertingMod(st);
    }

    bool ReadingMods(const State& st)
    {
        return st.modScan.valid();
    }

    // Reads the mod list (in the saved order) in the background: every file of
    // every mod is looked at, and the game's file names read the first time.
    void ReadMods(State& st, std::vector<ModState> saved)
    {
        st.modScan = std::async(std::launch::async,
            [root = st.modsRoot, gameRoot = st.gameRoot, game = st.gameFiles, saved = std::move(saved),
                stop = st.modStop.get_token()]() mutable {
                if (!game)
                    game = std::make_shared<const mods::GameFiles>(mods::GameFiles::Read(gameRoot, stop));
                return ModScan{ game, mods::Scan(root, saved, game.get()) };
            });
    }

    // When the mod list has been read: shown, and a drop that waited for it added.
    void FinishReadingMods(State& st)
    {
        if (!ReadingMods(st) || st.modScan.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return;
        try
        {
            ModScan scan = st.modScan.get();
            st.gameFiles = std::move(scan.gameFiles);
            st.mods = std::move(scan.mods);
            st.modsRead = true;
        }
        catch (const std::exception& error)
        {
            LOGE("Mods: reading the mods failed: {}", error.what());
            st.modMessages = { { "Can't read the mods: something went wrong (see dao.log).", true } };
        }
        st.selectedMod = -1;
        if (!st.dropped.empty() && !st.play && !st.quit)
            AddDroppedMods(st);
    }

    // Adds the mods just dropped on the window, in the background (copying and
    // unpacking can take a while), and shows the Mods page.
    void AddDroppedMods(State& st)
    {
        // (After the list has been read, which they're added to.)
        if (ReadingMods(st))
        {
            if (!st.dropped.empty())
                SetView(st, View::Mods);
            return;
        }
        std::vector<fs::path> dropped = std::move(st.dropped);
        st.dropped.clear();
        if (dropped.empty())
            return;
        SetView(st, View::Mods);
        if (ModsBusy(st))
        {
            st.modMessages = { { "Still working on the last mods: drop these again when it's done.", true } };
            return;
        }
        st.modMessages.clear();
        // Xbox 360 packages (DLC, the title update) are installed; the rest are mods.
        st.modInstall = std::async(std::launch::async,
            [root = st.modsRoot, gameRoot = st.gameRoot, userRoot = st.userRoot, dropped = std::move(dropped),
                stop = st.modStop.get_token(), game = st.gameFiles]() mutable {
                const std::vector<content::DroppedPackage> packages = content::TakePackages(dropped);
                std::vector<mods::InstallResult::Line> installed;
                content::InstallPackages(gameRoot, userRoot, packages, stop, installed);
                mods::InstallResult result;
                if (!dropped.empty())
                    result = mods::Install(root, dropped, stop, game.get());
                result.lines.insert(result.lines.begin(), installed.begin(), installed.end());
                return result;
            });
    }

    // When the background install is done: the new mods at the end of the list
    // (the last one selected) and the outcome.
    void FinishAddingMods(State& st)
    {
        if (!AddingMods(st) || st.modInstall.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return;
        mods::InstallResult result;
        try
        {
            result = st.modInstall.get();
        }
        catch (const std::exception& error)
        {
            LOGE("Mods: adding failed: {}", error.what());
            result.lines.push_back({ "Can't add the mods: something went wrong (see dao.log).", true });
        }
        for (mods::ModInfo& mod : result.mods)
        {
            // (A folder deleted while the launcher was open may have the same name.)
            std::erase_if(st.mods, [&](const mods::ModInfo& old) { return SameFolder(old.folder, mod.folder); });
            st.mods.push_back(std::move(mod));
        }
        if (!result.mods.empty())
        {
            st.selectedMod = int(st.mods.size()) - 1;
            st.scrollToMod = true;
        }
        mods::FindOverlaps(st.mods);
        st.modMessages = std::move(result.lines);
    }

    // When the background conversion is done: the mod as it is now, and the outcome.
    void FinishConverting(State& st)
    {
        if (!ConvertingMod(st) || st.modConvert.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return;
        Converted converted;
        bool described = false;
        try
        {
            converted = st.modConvert.get();
            described = true;
        }
        catch (const std::exception& error)
        {
            LOGE("Mods: converting failed: {}", error.what());
            converted.result.problems.push_back("Something went wrong (see dao.log).");
        }
        const mods::ConvertResult& result = converted.result;
        const std::string folder = std::exchange(st.convertingMod, {});
        const auto mod = std::ranges::find_if(st.mods,
            [&](const mods::ModInfo& m) { return SameFolder(m.folder, folder); });
        const std::string name = mod != st.mods.end() ? mod->name : folder;
        if (mod != st.mods.end() && described)
        {
            const bool enabled = mod->enabled;
            *mod = std::move(converted.mod);
            mod->enabled = enabled;
            mods::FindOverlaps(st.mods);
        }

        // The files left out: the first few (all are in dao.log).
        constexpr size_t kLeftOutShown = 8;
        auto& lines = st.modMessages;
        lines.clear();
        if (result.converted)
            lines.push_back({ fmt::format("Converted \"{}\": {} file{} written. Its PC files are kept in "
                                          "mods/.originals/{}/.",
                name, result.files, result.files == 1 ? "" : "s", folder) });
        for (const std::string& problem : result.problems)
            lines.push_back({ fmt::format("Can't convert \"{}\": {}", name, problem), true });
        if (!result.leftOut.empty())
            lines.push_back({ fmt::format("{} of its files couldn't be converted and {} left out:",
                                  result.leftOut.size(), result.leftOut.size() == 1 ? "was" : "were"),
                true });
        for (size_t i = 0; i < result.leftOut.size(); ++i)
        {
            LOGW("Mod {}: left out {}", folder, result.leftOut[i]);
            if (i < kLeftOutShown)
                lines.push_back({ "    " + result.leftOut[i], true });
        }
        if (result.leftOut.size() > kLeftOutShown)
            lines.push_back({ fmt::format("    and {} more (see dao.log).", result.leftOut.size() - kLeftOutShown),
                true });
        for (const std::string& note : result.notes)
            lines.push_back({ note });
        if (mod != st.mods.end() && mod->IsForPc())
            lines.push_back({ fmt::format("It still has files in the PC version's formats (such as {}), so it "
                                          "isn't loaded.",
                                  mod->pcExample),
                true });
    }

    namespace
    {
        // Converts the selected mod, made for the PC version, in the background (and
        // describes it again there: that reads every file).
        void ConvertSelectedMod(State& st)
        {
            const mods::ModInfo& mod = st.mods[st.selectedMod];
            st.convertingMod = mod.folder;
            st.modMessages.clear();
            st.modConvert = std::async(std::launch::async,
                [folder = st.modsRoot / FromUtf8(mod.folder), root = st.gameRoot, stop = st.modStop.get_token(),
                    game = st.gameFiles] {
                    Converted out{ mods::ConvertPcMod(folder, root, stop, game.get()), {} };
                    out.mod = mods::Describe(folder, game.get());
                    return out;
                });
        }

        // Asks before deleting the selected mod (to the Recycle Bin).
        void DeleteModPopup(State& st)
        {
            const float width = 560.0f * st.scale;
            ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0), ImVec2(width, FLT_MAX));
            if (!ImGui::BeginPopupModal("Delete mod", nullptr,
                    ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings))
                return;
            bool close = st.selectedMod < 0 || st.selectedMod >= int(st.mods.size());
            if (!close)
            {
                const mods::ModInfo& mod = st.mods[st.selectedMod];
                ImGui::PushTextWrapPos(0.0f);
                ImGui::Text("Delete \"%s\"?", mod.name.c_str());
                ImGui::PopTextWrapPos();
                HelpText(fmt::format("Its folder, mods/{}, goes to the Recycle Bin, with the PC files kept from "
                                     "converting it, if it was.",
                    mod.folder)
                             .c_str());
                ImGui::Spacing();
                if (ImGui::Button("Delete"))
                {
                    const std::string name = mod.name;
                    std::string error;
                    if (mods::RecycleMod(st.modsRoot, mod.folder, error))
                    {
                        st.mods.erase(st.mods.begin() + st.selectedMod);
                        st.selectedMod = -1;
                        mods::FindOverlaps(st.mods);
                        st.modMessages = { { fmt::format("Deleted \"{}\" (it's in the Recycle Bin).", name) } };
                    }
                    else
                        st.modMessages = { { fmt::format("Can't delete \"{}\": {}.", name, error), true } };
                    close = true;
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
                    ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
                    close = true;
                // Cancel is the default.
                if (ImGui::IsWindowAppearing())
                    ImGui::SetKeyboardFocusHere(-1);
            }
            if (close)
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        void ModsList(State& st)
        {
            HelpText("Each mod is a folder in mods/ that mirrors the game folder, e.g. "
                     "mods/MyMod/packages/core/override/. Where two change the same file, the one lower in the list "
                     "usually wins.");
            ImGui::Spacing();

            HelpText(mods::CanOpenArchives()
                    ? "To add a mod, drop its archive (.zip, .7z, .rar), folder or files on this window: its files "
                      "are sorted into place."
                    : "To add a mod, drop its folder or files on this window: its files are sorted into place.");
            ImGui::Spacing();
            HelpText("Downloadable content and Title Update 6 or 7 copied from an Xbox 360 are installed the same way: "
                     "drop the packages, or the folder holding them.");
            ImGui::Spacing();
            if (ReadingMods(st))
            {
                ImGui::TextUnformatted("Reading the mods...");
                return;
            }
            if (AddingMods(st))
                ImGui::TextUnformatted("Adding...");
            if (ConvertingMod(st))
                ImGui::Text("Converting %s...", st.convertingMod.c_str());
            ImGui::PushTextWrapPos(0.0f);
            for (const auto& line : st.modMessages)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, line.problem ? kError : kText);
                ImGui::TextUnformatted(line.text.c_str());
                ImGui::PopStyleColor();
            }
            ImGui::PopTextWrapPos();
            ImGui::Spacing();

            if (ImGui::Button("Open mods folder"))
                OpenFolder(st.modsRoot);
            ImGui::SameLine();
            // The list changes under a conversion: not while one runs.
            ImGui::BeginDisabled(ConvertingMod(st));
            // (Nor while mods are added, which the new list would leave out.)
            ImGui::BeginDisabled(AddingMods(st));
            if (ImGui::Button("Refresh"))
                ReadMods(st, mods::ToState(st.mods));
            ImGui::EndDisabled();
            ImGui::SameLine();
            const bool canUp = st.selectedMod > 0;
            const bool canDown = st.selectedMod >= 0 && st.selectedMod + 1 < int(st.mods.size());
            ImGui::BeginDisabled(!canUp);
            if (ImGui::Button("Move up"))
            {
                std::swap(st.mods[st.selectedMod], st.mods[st.selectedMod - 1]);
                st.selectedMod--;
                mods::FindOverlaps(st.mods);
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(!canDown);
            if (ImGui::Button("Move down"))
            {
                std::swap(st.mods[st.selectedMod], st.mods[st.selectedMod + 1]);
                st.selectedMod++;
                mods::FindOverlaps(st.mods);
            }
            ImGui::EndDisabled();
            ImGui::EndDisabled();
            // A PC mod: Convert (one at a time, not while mods are being added).
            const bool selected = st.selectedMod >= 0 && st.selectedMod < int(st.mods.size());
            if (selected && st.mods[st.selectedMod].IsForPc())
            {
                ImGui::SameLine();
                ImGui::BeginDisabled(ModsBusy(st));
                if (ImGui::Button("Convert"))
                    ConvertSelectedMod(st);
                ImGui::EndDisabled();
            }
            if (selected)
            {
                ImGui::SameLine();
                ImGui::BeginDisabled(ModsBusy(st));
                if (ImGui::Button("Delete"))
                    ImGui::OpenPopup("Delete mod");
                ImGui::EndDisabled();
            }
            DeleteModPopup(st);
            ImGui::Spacing();

            if (st.mods.empty())
            {
                HelpText("No mods installed. Put mod folders in the mods folder and press Refresh.");
                return;
            }

            const float detailsHeight = ImGui::GetTextLineHeightWithSpacing() * 6;
            constexpr ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
            if (ImGui::BeginTable("mods", 5, flags, ImVec2(0, -detailsHeight)))
            {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 3.0f);
                ImGui::TableSetupColumn("Version", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn("Author", ImGuiTableColumnFlags_WidthStretch, 1.5f);
                ImGui::TableSetupColumn("Files", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableHeadersRow();
                for (int i = 0; i < int(st.mods.size()); i++)
                {
                    auto& mod = st.mods[i];
                    ImGui::PushID(i);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    // A PC mod can't be turned on (its choice is kept for a converted one),
                    // nor the one being converted changed.
                    ImGui::BeginDisabled(mod.IsForPc() || mod.folder == st.convertingMod);
                    bool on = mod.IsLoaded();
                    if (ImGui::Checkbox("##on", &on))
                    {
                        mod.enabled = on;
                        mods::FindOverlaps(st.mods);
                    }
                    ImGui::EndDisabled();
                    ImGui::TableNextColumn();
                    if (ImGui::Selectable(mod.name.c_str(), st.selectedMod == i, ImGuiSelectableFlags_SpanAllColumns))
                        st.selectedMod = i;
                    if (st.scrollToMod && st.selectedMod == i)
                    {
                        ImGui::SetScrollHereY();
                        st.scrollToMod = false;
                    }
                    if (mod.IsForPc())
                    {
                        ImGui::SameLine();
                        ImGui::PushStyleColor(ImGuiCol_Text, kError);
                        ImGui::TextUnformatted("(PC version)");
                        ImGui::PopStyleColor();
                    }
                    if (const size_t warnings = mod.warnings.size() + !mod.overlap.empty())
                    {
                        ImGui::SameLine();
                        ImGui::PushStyleColor(ImGuiCol_Text, kWarning);
                        ImGui::Text("(%d warning%s)", int(warnings), warnings == 1 ? "" : "s");
                        ImGui::PopStyleColor();
                    }
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(mod.version.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(mod.author.c_str());
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", int(mod.fileCount));
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            if (st.selectedMod >= 0 && st.selectedMod < int(st.mods.size()))
            {
                const auto& mod = st.mods[st.selectedMod];
                ImGui::BeginChild("details", ImVec2(0, 0), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoBackground);
                ImGui::Text("mods/%s", mod.folder.c_str());
                if (mod.IsForPc())
                    HelpText(fmt::format("Made for the PC version of the game: {} of its files ({}{}) are in PC "
                                         "formats, which this Xbox 360 version can't read, so it isn't loaded. "
                                         "Convert makes them this version's (most models, textures and tables "
                                         "convert; the PC files are kept).",
                        mod.pcFiles, mod.pcExample, mod.pcFiles > 1 ? ", ..." : "")
                                 .c_str());
                else
                    HelpText(mod.description.empty() ? "No description." : mod.description.c_str());
                ImGui::PushStyleColor(ImGuiCol_Text, kWarning);
                ImGui::PushTextWrapPos(0.0f);
                for (const std::string& warning : mod.warnings)
                    WrappedBullet(warning.c_str());
                if (!mod.overlap.empty())
                    WrappedBullet(mod.overlap.c_str());
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
                ImGui::EndChild();
            }
        }

        // Shown before the mods page is first opened: what to expect of mods.
        void ModsWarning(State& st)
        {
            ImGui::PushFont(st.headingFont);
            ImGui::TextUnformatted("Before you add mods");
            ImGui::PopFont();
            ImGui::Spacing();
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted("Mods for Dragon Age: Origins are made for the PC version of the game. This is the "
                                   "Xbox 360 version, so not every mod works:");
            ImGui::Spacing();
            WrappedBullet("Mods that change models, textures, items or tables usually work. Those in the PC "
                              "version's file formats need converting first: select one and press Convert.");
            WrappedBullet("New areas and campaigns (add-ins), and menus or sounds made for the PC version, "
                              "can't work here.");
            WrappedBullet("Any mod can make the game crash, look wrong or break a save. The launcher warns of "
                              "what it finds in a mod, but it can't catch everything.");
            WrappedBullet("Back up your saves before trying a mod: they're in userdata/content/.");
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
            ImGui::Spacing();
            if (ImGui::Button("I understand"))
            {
                st.settings.modsWarningSeen = true;
                // Focus and scroll start over on the list.
                SetView(st, View::Mods);
            }
            if (st.viewChanged)
                ImGui::SetKeyboardFocusHere(-1);
            ImGui::SameLine();
            if (ImGui::Button("Open saves folder"))
                OpenFolder(st.userRoot / "content");
        }
    }

    void ModsView(State& st, const Layout& layout)
    {
        PanelFrame(st, layout, "Mods", nullptr);
        // (Screenshots show the list.)
        const bool warn = !st.settings.modsWarningSeen && st.screenshotDir.empty();
        ShowContents(st, layout, kModsLeft, warn ? ModsWarning : ModsList);
    }
}
