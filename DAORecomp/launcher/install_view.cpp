#include "stdafx.h"
#include "launcher/launcher_internal.h"

#include <imgui_stdlib.h>

#include "content/xiso.h"
#include "program.h"
#include "text.h"

namespace dao::launcher
{
    namespace
    {
        // The licence of the Microsoft build tools (CRT and Windows SDK) that building
        // the game downloads (installer/deps.py MSVC_LICENSE_URL).
        constexpr const char* kMsvcLicenseUrl = "https://go.microsoft.com/fwlink/?LinkId=2086102";

        std::string& FieldText(State& st, PathField field)
        {
            return field == PathField::DiscImage ? st.installIso
                : field == PathField::TitleUpdate ? st.installUpdate
                : field == PathField::NeuralWeights ? st.settings.neuralWeights
                : st.installDest;
        }

        struct DialogRequest
        {
            std::shared_ptr<DialogChoices> results;
            PathField field;
            std::string start;
        };

        // Opens Windows' file or folder dialog for a field. Its choice comes on SDL's
        // dialog thread, into st.dialogs, which the next frame takes it from.
        void Browse(State& st, PathField field)
        {
            static const SDL_DialogFileFilter kDiscImage[] = { { "Xbox 360 disc image", "iso" },
                { "All files", "*" } };
            static const SDL_DialogFileFilter kAnyFile[] = { { "All files", "*" } };
            auto* request = new DialogRequest{ st.dialogs, field, FieldText(st, field) };
            {
                std::lock_guard lock(st.dialogs->mutex);
                st.dialogs->open++;
            }
            const SDL_DialogFileCallback chosen = [](void* userdata, const char* const* files, int) {
                const std::unique_ptr<DialogRequest> request(static_cast<DialogRequest*>(userdata));
                std::lock_guard lock(request->results->mutex);
                if (files && files[0])
                    request->results->chosen.emplace_back(request->field, files[0]);
                request->results->open--;
            };
            const char* start = request->start.empty() ? nullptr : request->start.c_str();
            if (field == PathField::Folder || field == PathField::NeuralWeights)
                SDL_ShowOpenFolderDialog(chosen, request, st.window, start, false);
            else if (field == PathField::DiscImage)
                SDL_ShowOpenFileDialog(chosen, request, st.window, kDiscImage, 2, start, false);
            else
                SDL_ShowOpenFileDialog(chosen, request, st.window, kAnyFile, 1, start, false);
        }
    }

    void TakeDialogChoices(State& st)
    {
        std::lock_guard lock(st.dialogs->mutex);
        for (auto& [field, path] : st.dialogs->chosen)
        {
            // The weights' folder in lmxxf's package, or the package itself.
            if (field == PathField::NeuralWeights)
            {
                if (const auto weights = FindNeuralWeights(FromUtf8(path)))
                    path = Utf8(*weights);
            }
            FieldText(st, field) = std::move(path);
        }
        st.dialogs->chosen.clear();
    }

    // A path: typed, pasted, or chosen with Browse.
    void PathRow(State& st, const char* label, PathField field, bool enabled)
    {
        Label(label);
        ImGui::PushID(int(field));
        const ImGuiStyle& style = ImGui::GetStyle();
        const float button = ImGui::CalcTextSize("Browse...").x + style.FramePadding.x * 2.0f;
        ImGui::BeginDisabled(!enabled);
        ImGui::SetNextItemWidth(RowWidth() - button - style.ItemSpacing.x);
        ImGui::InputText("##path", &FieldText(st, field));
        ImGui::SameLine();
        bool dialogOpen = false;
        {
            std::lock_guard lock(st.dialogs->mutex);
            dialogOpen = st.dialogs->open > 0;
        }
        ImGui::BeginDisabled(dialogOpen);
        if (ImGui::Button("Browse..."))
            Browse(st, field);
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        ImGui::PopID();
    }

    // Files dropped on the Install view: a disc image (.iso), or else the title
    // update's package.
    void TakeDroppedInstallFiles(State& st)
    {
        for (const fs::path& path : st.dropped)
        {
            std::error_code ec;
            if (!fs::is_regular_file(path, ec))
                continue;
            FieldText(st, Lower(Utf8(path.extension())) == ".iso" ? PathField::DiscImage
                                                                   : PathField::TitleUpdate) = Utf8(path);
        }
        st.dropped.clear();
    }

    // Whether the install folder already has the game's files (an install to
    // update): then no disc image is needed. Checked when the folder changes.
    bool HasGameFiles(State& st)
    {
        if (st.installDest != st.checkedDest)
        {
            st.checkedDest = st.installDest;
            st.destHasGame = !st.installDest.empty() && IsInstalledGame(FromUtf8(st.installDest));
        }
        return st.destHasGame;
    }

    // Where setup keeps the art archive from the disc image: a folder laid out as
    // the game's, in the installer's work folder.
    fs::path SetupArtRoot()
    {
        return InstallerWorkFolder() / "setup-art";
    }

    void StartInstall(State& st)
    {
        InstallRequest request;
        if (!HasGameFiles(st))
            request.iso = FromUtf8(st.installIso);
        request.titleUpdate = FromUtf8(st.installUpdate);
        request.dest = FromUtf8(st.installDest);
        request.acceptLicense = st.installAccept;
        request.shortcuts = st.installShortcuts;
        st.installStartError.clear();
        st.installer.Start(request, st.installStartError);
    }

    namespace
    {
        // The art archive's size limit: far larger than the game's (18 MB).
        constexpr uint64_t kMaxArtArchiveSize = 256ull << 20;

        // Setup before the game is installed: once a disc image is chosen, its menu art
        // is copied out (in the background) and the screens get it.
        void UpdateIsoArt(State& st)
        {
            if (st.isoArt.valid())
            {
                if (st.isoArt.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                    return;
                if (st.isoArt.get() && st.art.empty())
                {
                    st.gameRoot = SetupArtRoot();
                    LoadArt(st, st.renderer);
                }
                return;
            }
            if (!st.art.empty() || st.installIso.empty() || st.installIso == st.artIso)
                return;
            st.artIso = st.installIso;
            const fs::path iso = FromUtf8(st.installIso);
            std::error_code ec;
            if (!fs::is_regular_file(iso, ec))
                return;
            st.isoArt = std::async(std::launch::async, [iso] {
                const fs::path out = SetupArtRoot() / kArtArchive;
                std::error_code ec;
                fs::create_directories(out.parent_path(), ec);
                std::string error;
                if (!content::ExtractIsoFile(iso, kArtArchive, out, kMaxArtArchiveSize, error))
                {
                    LOGW("Setup: no menu art from {}: {}", Utf8(iso), error);
                    return false;
                }
                return true;
            });
        }

        // The installer's output, kept scrolled to the end unless scrolled up.
        void InstallLog(const InstallRunner& installer, float height)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(kTextDim));
            ImGui::BeginChild("log", ImVec2(RowWidth(), std::max(height, ImGui::GetTextLineHeight() * 4.0f)),
                ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
            // Only the lines in view (up to kMaxLogLines in all).
            const std::deque<std::string>& log = installer.Log();
            ImGuiListClipper clipper;
            clipper.Begin(int(log.size()));
            while (clipper.Step())
            {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++)
                    ImGui::TextUnformatted(log[size_t(i)].c_str());
            }
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
                ImGui::SetScrollHereY(1.0f);
            ImGui::EndChild();
            ImGui::PopStyleColor();
        }

        void InstallProgress(State& st)
        {
            InstallRunner& installer = st.installer;
            if (installer.GetState() == InstallRunner::State::Done)
            {
                Section("Installed");
                ImGui::TextUnformatted("Opening the launcher...");
                return;
            }
            Section(installer.Status().empty() ? "Starting" : installer.Status().c_str());
            // Negative and changing: ImGui's moving bar, for a step of unknown length.
            const float progress = installer.Progress();
            ImGui::ProgressBar(progress < 0.0f ? -float(ImGui::GetTime()) : progress, ImVec2(RowWidth(), 0),
                progress < 0.0f ? "" : nullptr);
            HelpText("The first install takes a while: it downloads the build tools, then recompiles and builds "
                     "the game. Stopping is safe: installing again resumes where it stopped.");
            ImGui::Spacing();
            const float button = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
            InstallLog(installer, ImGui::GetContentRegionAvail().y - button);
            if (ImGui::Button("Stop"))
                installer.Cancel();
        }

        void InstallContents(State& st)
        {
            TakeDialogChoices(st);
            UpdateIsoArt(st);
            const InstallRunner::State state = st.installer.GetState();
            if (state == InstallRunner::State::Running || state == InstallRunner::State::Done)
            {
                InstallProgress(st);
                return;
            }
            const bool update = HasGameFiles(st);
            Section("Your copy of the game");
            PathRow(st, "Disc image", PathField::DiscImage, !update);
            HelpText(update ? "Not needed: this folder already has the game's files. Installing again updates it, "
                              "keeping your saves, settings and mods."
                            : "Your Xbox 360 disc image (.iso) of Dragon Age: Origins. Its game files are copied out "
                              "and its code is recompiled for this PC: nothing of the game is downloaded. You can "
                              "also drop it on this window.");
            ImGui::Spacing();
            PathRow(st, "Title Update 6 or 7 (optional)", PathField::TitleUpdate);
            HelpText("Needed by Awakening and the DLC campaigns: its package (tu00000001_00000000). It can also be "
                     "added later, by dropping it on the launcher.");

            Section("Install");
            PathRow(st, "Folder", PathField::Folder);
            HelpText("Gets the game's files, your saves, settings and mods. The build tools and the build go to "
                     "%LOCALAPPDATA%\\DAORecomp-build (about 12 GB).");
            ImGui::Checkbox("Start menu and desktop shortcuts", &st.installShortcuts);
            if (st.setupOnly && !st.steamPath.empty())
            {
                ImGui::Checkbox("Add to Steam", &st.installSteam);
                HelpText("With artwork; started from Steam (Big Picture too), it goes straight into the game. Steam "
                         "has to close for it: the launcher asks first, once the game is installed.");
            }
            ImGui::Spacing();
            ImGui::Checkbox("I accept the license of the Microsoft C++ build tools and Windows SDK",
                &st.installAccept);
            ImGui::SameLine();
            if (ImGui::Button("Read it"))
                SDL_OpenURL(kMsvcLicenseUrl);
            HelpText("Building the game needs them; they're downloaded from Microsoft. (Once they're set up, an "
                     "update doesn't need this.)");
            ImGui::Spacing();

            const std::string& error = st.installStartError.empty() && state == InstallRunner::State::Failed
                ? st.installer.Error()
                : st.installStartError;
            if (!error.empty())
            {
                ImGui::PushStyleColor(ImGuiCol_Text, kError);
                ImGui::TextWrapped("%s", error.c_str());
                ImGui::PopStyleColor();
            }
            const bool ready = (update || !st.installIso.empty()) && !st.installDest.empty() &&
                (st.installAccept || update);
            ImGui::BeginDisabled(!ready);
            if (ImGui::Button(state == InstallRunner::State::Failed ? "Try again" : update ? "Update" : "Install"))
                StartInstall(st);
            ImGui::EndDisabled();
            if (state == InstallRunner::State::Failed && !st.installer.Log().empty())
            {
                ImGui::Spacing();
                InstallLog(st.installer, ImGui::GetTextLineHeightWithSpacing() * 12.0f);
            }
        }
    }

    void InstallView(State& st, const Layout& layout)
    {
        PanelFrame(st, layout, "Install", nullptr);
        ShowContents(st, layout, kListLeft, InstallContents);
    }
}
