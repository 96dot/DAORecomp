#include "stdafx.h"
#include "launcher/launcher_internal.h"

#include <backends/imgui_impl_sdlrenderer3.h>

#include "program.h"

namespace dao::launcher
{
    namespace
    {
        // A draw list drawn into an image of `art`'s size, read back into it as RGBA.
        bool RenderToPixels(SDL_Renderer* renderer, ImDrawList& list, steam::Artwork& art)
        {
            SDL_Texture* target =
                SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, art.width, art.height);
            if (!target)
                return false;
            SDL_Surface* pixels = nullptr;
            if (SDL_SetRenderTarget(renderer, target))
            {
                SDL_SetRenderScale(renderer, 1.0f, 1.0f);
                SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
                SDL_RenderClear(renderer);
                ImDrawData data;
                data.Valid = true;
                data.DisplaySize = ImVec2(float(art.width), float(art.height));
                data.FramebufferScale = ImVec2(1.0f, 1.0f);
                data.AddDrawList(&list);
                ImGui_ImplSDLRenderer3_RenderDrawData(&data, renderer);
                pixels = SDL_RenderReadPixels(renderer, nullptr);
                SDL_SetRenderTarget(renderer, nullptr);
            }
            SDL_DestroyTexture(target);
            SDL_Surface* rgba = pixels ? SDL_ConvertSurface(pixels, SDL_PIXELFORMAT_RGBA32) : nullptr;
            SDL_DestroySurface(pixels);
            const bool ok = rgba && rgba->w == art.width && rgba->h == art.height;
            if (ok)
            {
                const size_t row = size_t(art.width) * 4;
                art.rgba.resize(row * art.height);
                for (int y = 0; y < art.height; y++)
                    memcpy(art.rgba.data() + row * y, static_cast<const uint8_t*>(rgba->pixels) + size_t(rgba->pitch) * y,
                        row);
            }
            SDL_DestroySurface(rgba);
            return ok;
        }

        // The title screen this far in, for Steam's artwork: the fire up.
        constexpr double kSteamArtTime = 6.0;

        // Steam's library images, from the title screen: the scene with the logo for
        // the cover and the wide capsule, without it for the hero image (Steam puts
        // the logo over that), and the logo alone. None without the game's art.
        std::vector<steam::Artwork> RenderSteamArt(const State& st)
        {
            const std::optional<LogoArt> logo = FindLogoArt(st);
            if (!logo)
                return {};
            constexpr int kLogoImageWidth = 1280;
            // (Bounded, as the art comes from the player's disc: the game's is 0.25.)
            const float logoAspect =
                std::clamp((logo->max.y - logo->min.y) / (logo->max.x - logo->min.x), 0.05f, 1.0f);
            struct Image
            {
                const char* suffix;
                int width, height;
                bool scene;
                // The logo's centre (as a share of the height) and width; none at 0.
                float logoY, logoWidth;
            };
            // Steam's sizes; the logo's placing chosen by eye. The logo image is the
            // logo with a small margin.
            const Image images[] = {
                { "p", 600, 900, true, 0.24f, 540.0f },
                { "", 920, 430, true, 0.5f, 640.0f },
                { "_hero", 1920, 620, true, 0.0f, 0.0f },
                { "_logo", kLogoImageWidth, int(kLogoImageWidth * 0.95f * logoAspect + 0.5f) + 16, false, 0.5f,
                    kLogoImageWidth * 0.95f },
            };
            std::vector<steam::Artwork> out;
            for (const Image& image : images)
            {
                const ImVec2 size(float(image.width), float(image.height));
                ImDrawList list(ImGui::GetDrawListSharedData());
                list._ResetForNewFrame();
                list.PushClipRect(ImVec2(0, 0), size);
                list.PushTextureID(ImGui::GetIO().Fonts->TexID);
                if (image.scene)
                    DrawScene(&list, st, Layout(ImVec2(0, 0), size), 0.0f, kSteamArtTime);
                if (image.logoWidth > 0.0f)
                    DrawLogoArt(&list, *logo, ImVec2(size.x * 0.5f, size.y * image.logoY), image.logoWidth);
                steam::Artwork art = { image.suffix, image.width, image.height };
                if (!RenderToPixels(st.renderer, list, art))
                {
                    LOGW("Launcher: can't draw Steam's artwork: {}", SDL_GetError());
                    return {};
                }
                // The scene is opaque, but its multiplied layers leave the target's alpha
                // as it was (none); the logo, drawn over nothing, comes back with its
                // colours multiplied by its alpha.
                for (size_t i = 0; i < art.rgba.size(); i += 4)
                {
                    if (image.scene)
                        art.rgba[i + 3] = 255;
                    else if (const int alpha = art.rgba[i + 3]; alpha > 0 && alpha < 255)
                    {
                        for (size_t c = i; c < i + 3; c++)
                            art.rgba[c] = uint8_t(std::min(255, (art.rgba[c] * 255 + alpha / 2) / alpha));
                    }
                }
                out.push_back(std::move(art));
            }
            return out;
        }

        // Steam's close takes a while with games' cloud saves to sync, or an update.
        constexpr std::chrono::seconds kSteamCloseTimeout{ 180 };
        // After this long closing, the popup offers to end Steam (it can get stuck).
        constexpr std::chrono::seconds kSteamStuckAfter{ 15 };

        void StartSteamWork(State& st)
        {
            st.steamStep = SteamStep::Working;
            st.steamControl = std::make_shared<SteamControl>();
            st.steamWaitStart = std::chrono::steady_clock::now();
            st.steamTask = std::async(std::launch::async,
                [steamPath = st.steamPath, artwork = std::move(st.steamArt), exe = ProgramPath(),
                    control = st.steamControl] {
                    SteamResult result;
                    const bool wasOpen = steam::IsRunning(steamPath);
                    try
                    {
                        bool closed = !wasOpen;
                        if (wasOpen && !steam::RequestShutdown(steamPath))
                            result.error = fmt::format("can't ask Steam to close (error {})", GetLastError());
                        else if (wasOpen)
                        {
                            control->closing = true;
                            const auto end = std::chrono::steady_clock::now() + kSteamCloseTimeout;
                            closed = !steam::IsRunning(steamPath);
                            while (!closed && std::chrono::steady_clock::now() < end)
                            {
                                if (control->end.exchange(false))
                                    steam::End(steamPath);
                                Sleep(250);
                                closed = !steam::IsRunning(steamPath);
                            }
                            control->closing = false;
                            if (!closed)
                                result.error = "Steam didn't close in time (start it again once it has)";
                        }
                        if (closed)
                        {
                            // Started from Steam (Big Picture, a controller), straight into the game.
                            const steam::Shortcut shortcut = { "Dragon Age: Origins", exe, "--play" };
                            result.accounts = steam::AddShortcut(steamPath, shortcut, artwork, result.error);
                        }
                    }
                    catch (const std::exception& e)
                    {
                        LOGE("Launcher: adding to Steam failed: {}", e.what());
                        result.error = "something went wrong (see dao.log)";
                    }
                    // Open again if it was before, even when adding failed (and if it
                    // closed after all); not while Windows still holds the old one.
                    // (A few seconds for one that this closed or ended to go.)
                    result.steamStuck = steam::IsStuck(steamPath);
                    for (int tries = 0; wasOpen && result.steamStuck && tries < 20; tries++)
                    {
                        Sleep(250);
                        result.steamStuck = steam::IsStuck(steamPath);
                    }
                    if (result.steamStuck)
                        LOGW("Launcher: Steam has exited but Windows hasn't let go of it, so it can't start again");
                    else if (wasOpen && !steam::IsRunning(steamPath) && !steam::Start(steamPath))
                        LOGW("Launcher: can't start Steam again (error {})", GetLastError());
                    return result;
                });
            st.steamArt.clear();
        }
    }

    // Between frames: the artwork drawn (with the renderer).
    void UpdateSteam(State& st)
    {
        if (st.steamStep == SteamStep::Start && ImGui::GetFrameCount() > 0)
        {
            st.steamArt = RenderSteamArt(st);
            st.steamResult = {};
            if (steam::IsRunning(st.steamPath))
                st.steamStep = SteamStep::Confirm;
            else
                StartSteamWork(st);
        }
    }

    // Each frame, minimized too: the work's result taken.
    void FinishSteamWork(State& st)
    {
        if (st.steamStep == SteamStep::Working &&
            st.steamTask.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            st.steamResult = st.steamTask.get();
            st.steamStep = SteamStep::Done;
        }
    }

    void SteamPopup(State& st)
    {
        constexpr const char* kTitle = "Add to Steam";
        if (st.steamStep == SteamStep::None)
            return;
        if (!ImGui::IsPopupOpen(kTitle))
            ImGui::OpenPopup(kTitle);
        const float width = 560.0f * st.scale;
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0), ImVec2(width, FLT_MAX));
        if (!ImGui::BeginPopupModal(kTitle, nullptr,
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings))
            return;
        const bool cancel =
            ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false);
        bool close = false;
        ImGui::PushTextWrapPos(0.0f);
        if (st.steamStep == SteamStep::Confirm)
        {
            ImGui::TextUnformatted("Steam is open: it has to close to add the game, and is started again after.");
            ImGui::Spacing();
            if (ImGui::Button("Close Steam and add"))
                StartSteamWork(st);
            ImGui::SameLine();
            if (ImGui::Button("Cancel") || cancel)
            {
                close = true;
                st.steamArt.clear();
            }
            // Cancel (the last item) is the default.
            if (ImGui::IsWindowAppearing())
                ImGui::SetKeyboardFocusHere(-1);
        }
        else if (st.steamStep == SteamStep::Done)
        {
            const SteamResult& result = st.steamResult;
            if (result.accounts > 0)
                ImGui::TextUnformatted(result.accounts > 1
                        ? fmt::format("Added to Steam, for its {} accounts on this PC.", result.accounts).c_str()
                        : "Added to Steam.");
            else
            {
                ImGui::PushStyleColor(ImGuiCol_Text, kError);
                ImGui::Text("Can't add the game to Steam: %s.", result.error.c_str());
                ImGui::PopStyleColor();
            }
            if (result.accounts > 0)
                HelpText("It's in the library as Dragon Age: Origins, with artwork from the game's menus. Started "
                         "from Steam, it goes straight into the game; started from its own shortcut, it shows "
                         "this launcher as before.");
            // Added for some accounts, not all.
            if (result.accounts > 0 && !result.error.empty())
                HelpText(fmt::format("Not for every account: {}.", result.error).c_str());
            if (result.steamStuck)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, kError);
                ImGui::TextUnformatted("Steam is stuck: it has ended, but Windows is still holding on to it, "
                                       "so it won't start again until you restart the PC.");
                ImGui::PopStyleColor();
            }
            ImGui::Spacing();
            if (ImGui::Button("OK") || cancel)
                close = true;
            // (The only item.)
            if (ImGui::IsWindowAppearing())
                ImGui::SetKeyboardFocusHere(-1);
        }
        else if (st.steamControl && st.steamControl->closing)
        {
            const auto waited = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - st.steamWaitStart);
            ImGui::Text("Waiting for Steam to close (%d s)...", int(waited.count()));
            if (waited >= kSteamStuckAfter)
            {
                HelpText("Steam can take a while to close when it syncs cloud saves or updates. If it's stuck "
                         "(its windows are gone but it doesn't finish, and it won't start again), end it here, "
                         "as Task Manager's End task would. It's started again after.");
                ImGui::Spacing();
                ImGui::BeginDisabled(st.steamControl->end);
                if (ImGui::Button("End Steam"))
                    st.steamControl->end = true;
                ImGui::EndDisabled();
            }
        }
        else
            ImGui::TextUnformatted("Adding the game to Steam...");
        ImGui::PopTextWrapPos();
        if (close)
        {
            st.steamStep = SteamStep::None;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}
