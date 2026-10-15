#include "stdafx.h"
#include "launcher/launcher_internal.h"

namespace dao::launcher
{
    namespace
    {
        // The game's downloadable content for the Xbox 360, by the add-in its package
        // holds (the folder with its manifest.xml).
        struct KnownDlc
        {
            const char* addIn;
            const char* name;
            // A campaign of its own, made for Title Update 6 and later.
            bool needsUpdate;
        };
        constexpr KnownDlc kKnownDlc[] = {
            { "DAO_PRC_CP_2", "The Stone Prisoner", false },
            { "DAO_PRC_NRX_1", "Warden's Keep", false },
            { "DAO_PRC_CP_1", "Return to Ostagar", false },
            { "DAO_PRC_PROMO_ME", "Blood Dragon Armor", false },
            { "DAO_PRC_CP_3", "Feastday Gifts", false },
            { "DAO_PRC_CP_7", "Feastday Pranks", false },
            { "DAO_PRC_EP_1", "Awakening", true },
            { "DAO_PRC_LEL", "Leliana's Song", true },
            { "DAO_PRC_DRK", "The Darkspawn Chronicles", true },
            { "DAO_PRC_GIB", "The Golems of Amgarrak", true },
            { "DAO_PRC_STR", "Witch Hunt", true },
        };

        bool HasAddIn(const content::InstalledPackage& package, const char* addIn)
        {
            return std::ranges::any_of(package.addIns,
                [&](const std::string& name) { return _stricmp(name.c_str(), addIn) == 0; });
        }

        bool IsKnown(const content::InstalledPackage& package)
        {
            return std::ranges::any_of(kKnownDlc, [&](const KnownDlc& known) { return HasAddIn(package, known.addIn); });
        }

        // The table's rows: the known DLC, then packages it doesn't know (by their own
        // names).
        std::vector<DlcRow> MakeRows(const std::vector<content::InstalledPackage>& packages)
        {
            std::vector<DlcRow> rows;
            for (const KnownDlc& known : kKnownDlc)
            {
                const bool installed = std::ranges::any_of(packages,
                    [&](const content::InstalledPackage& package) { return HasAddIn(package, known.addIn); });
                rows.push_back({ known.name, installed, known.needsUpdate });
            }
            for (const content::InstalledPackage& package : packages)
            {
                if (!IsKnown(package))
                    rows.push_back({ package.name, true, false });
            }
            return rows;
        }

        // "Awakening, Leliana's Song, ... and Witch Hunt": the DLC that need the title update.
        const std::string& NeedingUpdate()
        {
            static const std::string names = [] {
                std::vector<const char*> needing;
                for (const KnownDlc& known : kKnownDlc)
                {
                    if (known.needsUpdate)
                        needing.push_back(known.name);
                }
                std::string text;
                for (size_t i = 0; i < needing.size(); i++)
                    text += fmt::format("{}{}", i == 0 ? "" : i + 1 == needing.size() ? " and " : ", ", needing[i]);
                return text;
            }();
            return names;
        }

        // DAO_BUILD=disc: the game runs the disc's version even with the title update.
        bool DiscForced()
        {
            const char* forced = getenv("DAO_BUILD");
            return forced && _stricmp(forced, "disc") == 0;
        }

        // Whether the game runs with the title update: one is installed, and this
        // build has its code.
        bool UpdateUsable(const content::TitleUpdateState& update)
        {
#if DAO_HAS_TU6
            return update.number != 0 && !DiscForced();
#else
            return false;
#endif
        }

        void TitleUpdateSection(const State& st)
        {
            Section("Title update");
            if (!st.dlcRead)
            {
                ImGui::TextUnformatted("Checking...");
                return;
            }
            const content::TitleUpdateState& update = st.titleUpdate;
            if (UpdateUsable(update))
                ImGui::TextWrapped("Title Update %d is installed, so the DLC that need it can be played.",
                    update.number);
            else if (update.number && DiscForced())
                ColoredText(kWarning, fmt::format("Title Update {} is installed, but DAO_BUILD=disc runs the game "
                                                  "without it.", update.number).c_str());
            else if (update.number)
            {
                const std::string text = fmt::format("Title Update {} is installed, but the game was built "
                                                     "without it: run the installer again on this folder to use it.",
                    update.number);
                ColoredText(kWarning, text.c_str());
            }
            else if (update.present)
                ColoredText(kError, "title_update/default.xexp isn't Title Update 6 or 7 (or can't be read), so the "
                                    "game runs without it.");
            else
                ImGui::TextWrapped("Not installed. %s need Title Update 6 or 7; the other DLC don't.",
                    NeedingUpdate().c_str());
        }

        void DlcSection(const State& st)
        {
            Section("Downloadable content");
            if (!st.dlcRead)
            {
                ImGui::TextUnformatted("Reading...");
                return;
            }
            const bool updateUsable = UpdateUsable(st.titleUpdate);
            constexpr ImGuiTableFlags flags =
                ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp;
            if (!ImGui::BeginTable("dlc", 3, flags))
                return;
            ImGui::TableSetupColumn("Content", ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableSetupColumn("Installed", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Needs", ImGuiTableColumnFlags_WidthStretch, 1.5f);
            ImGui::TableHeadersRow();
            for (const DlcRow& row : st.dlcRows)
            {
                const bool missing = row.installed && row.needsUpdate && !updateUsable;
                const char* needs = !row.needsUpdate ? "" : missing ? "Title update (missing)" : "Title update";
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Text, row.installed ? kText : kTextDim);
                ImGui::TextUnformatted(row.name.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(row.installed ? "Installed" : "Not installed");
                ImGui::PopStyleColor();
                ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Text, missing ? kWarning : kTextDim);
                ImGui::TextUnformatted(needs);
                ImGui::PopStyleColor();
            }
            ImGui::EndTable();
        }

        void DlcList(State& st)
        {
            TitleUpdateSection(st);
            DlcSection(st);

            Section("Adding more");
            HelpText("DLC and title updates come from an Xbox 360: copy their packages from the console's drive "
                     "(or your backup of them) and drop the packages, or the folder holding them, on this window. "
                     "On the console, DLC are in Content/0000000000000000/454108C0/00000002/ and the title update "
                     "in .../000B0000/. They're checked, then installed where the game finds them; the packages "
                     "are left as they are. (Mods dropped here are added too, to the Mods page.)");
            HelpText("Only packages as Xbox Live delivers them (LIVE or PIRS) are read. The title update has to be "
                     "the one for the English, Italian and Spanish disc (media ID 186A1115); Title Update 7 replaces 6. The installer "
                     "can take it too (Title Update 6 or 7).");
            ImGui::Spacing();
            if (AddingMods(st))
                ImGui::TextUnformatted("Adding...");
            else if (ConvertingMod(st))
                ImGui::TextUnformatted("Converting a mod: drops wait for it.");
            Messages(st.dlcMessages);
            ImGui::Spacing();
            if (ImGui::Button("Open DLC folder"))
                OpenFolder(content::DlcFolder(st.userRoot));
            ImGui::SameLine();
            ImGui::BeginDisabled(ReadingDlc(st) || AddingMods(st));
            if (ImGui::Button("Refresh"))
                ReadDlc(st);
            ImGui::EndDisabled();
        }
    }

    bool ReadingDlc(const State& st)
    {
        return st.dlcScan.valid();
    }

    // Reads what's installed in the background (the title update is hashed); once
    // more after the reading under way, if there's one. Not while packages are
    // being installed (the title update's folder is moved then): finishing reads.
    void ReadDlc(State& st)
    {
        if (AddingMods(st))
            return;
        if (ReadingDlc(st))
        {
            st.readDlcAgain = true;
            return;
        }
        st.readDlcAgain = false;
        st.dlcScan = std::async(std::launch::async, [userRoot = st.userRoot, gameRoot = st.gameRoot] {
            return DlcScan{ content::ListDlc(userRoot), content::FindTitleUpdate(gameRoot) };
        });
    }

    void FinishReadingDlc(State& st)
    {
        if (!ReadingDlc(st) || st.dlcScan.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return;
        try
        {
            DlcScan scan = st.dlcScan.get();
            st.dlcRows = MakeRows(scan.packages);
            st.titleUpdate = scan.titleUpdate;
            st.dlcRead = true;
        }
        catch (const std::exception& error)
        {
            LOGE("Launcher: reading the DLC failed: {}", error.what());
            st.dlcMessages = { { "Can't read what's installed: something went wrong (see dao.log).", true } };
        }
        if (st.readDlcAgain)
            ReadDlc(st);
    }

    void DlcView(State& st, const Layout& layout)
    {
        // Read as the view opens (the last list stays shown meanwhile).
        if (st.viewChanged)
            ReadDlc(st);
        PanelFrame(st, layout, "Downloadable Content", nullptr);
        ShowContents(st, layout, kListLeft, DlcList);
    }
}
