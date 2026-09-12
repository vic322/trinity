#include "menu.h"

#include <imgui.h>
#include <cstdio>

#include "framework.h"
#include "widgets.h"
#include "../core/state.h"
#include "../core/compat.h"
#include "../core/version_detect.h"
#include "../game/player.h"

// Focused personal menu built on Trinity's existing UI and player hooks.
// Original project attribution and license remain in the repository.
namespace trinity::gui
{
    namespace
    {
        // A version mismatch is the one failure nobody can diagnose from inside
        // the game, so it gets an always-on panel instead of a line in a menu
        // there is no reason to open once nothing responds.
        void DrawVersionWarning()
        {
            const ImGuiViewport* vp = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f,
                                           vp->WorkPos.y + vp->WorkSize.y * 0.10f),
                                    ImGuiCond_Always, ImVec2(0.5f, 0.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(30.0f, 24.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.10f, 0.02f, 0.03f, 0.94f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.85f, 0.18f, 0.22f, 1.00f));

            ImGui::Begin("##combatmenu_version_warning", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);

            ImGui::SetWindowFontScale(2.1f);
            ImGui::TextColored(ImVec4(1.00f, 0.42f, 0.42f, 1.00f), "UNSUPPORTED GAME VERSION");

            ImGui::SetWindowFontScale(1.30f);
            ImGui::Dummy(ImVec2(0.0f, 12.0f));
            ImGui::Text("This mod needs   %s", core::kSupportedTitle);
            ImGui::Text("Executable       %s", core::kSupportedPE);
            ImGui::Dummy(ImVec2(0.0f, 8.0f));
            ImGui::TextColored(ImVec4(1.00f, 0.78f, 0.35f, 1.00f),
                               "You are running  %s", core::GetGameVersion().rawVersionStr);

            ImGui::SetWindowFontScale(1.0f);
            ImGui::Dummy(ImVec2(0.0f, 14.0f));
            ImGui::TextDisabled("The combat menu is disabled and no game memory has been touched.");
            ImGui::TextDisabled("Delete Trinity.asi from the game's bin64 folder, or install a build for your version.");

            ImGui::End();
            ImGui::PopStyleColor(2);
            ImGui::PopStyleVar(2);
        }
    }

    bool WantsDraw()
    {
        return !core::GameVersionSupported() || State::Get().menuOpen || ui::ToastsActive();
    }

    void Render()
    {
        State& st = State::Get();
        ImGui::GetIO().MouseDrawCursor = false;

        if (!core::GameVersionSupported())
        {
            DrawVersionWarning();
            return;
        }

        const char* const tabs[] = { "COMBAT" };
        ui::SetTabs(tabs, 1);
        ui::DrawToasts();
        if (!st.menuOpen)
            return;

        // This read also requests character tracking while every cheat is off,
        // so the menu can report readiness before a feature is enabled.
        const int tracked = game::Player::GetTrackedPlayerCount();
        const bool ready = game::Player::Ready();
        char status[192];
        if (ready)
            std::snprintf(status, sizeof(status), "%s (PE %s)  |  %d character%s tracked",
                          core::kSupportedTitle, core::kSupportedPE,
                          tracked, tracked == 1 ? "" : "s");
        else
            std::snprintf(status, sizeof(status), "%s (PE %s)  |  waiting for player - load into gameplay",
                          core::kSupportedTitle, core::kSupportedPE);

        ui::BeginFrame();
        ui::Begin(status);
        ui::Toggle("God Mode", &st.godMode,
                   "Keeps tracked player health full and blocks incoming health damage.");
        ui::Toggle("Infinite Stamina", &st.infStamina,
                   "Keeps player stamina full for sprinting, dodging, and climbing.");
        ui::FloatOption("Damage Multiplier", &st.dmgOutMult,
                        1.0f, 100.0f, 1.0f, 1.0f, "%.0fx",
                        "Scale damage from your attacks. 1x is normal. Extreme Damage overrides this setting while enabled.");
        ui::Toggle("Extreme Damage (10,000x)", &st.oneHitKill,
                   "Massively increases damage from your attacks. Scripted invulnerability and boss phase limits can still apply.");
        if (ui::Option("Disable All",
                       "Turn off God Mode, Infinite Stamina, and Extreme Damage, and restore normal damage."))
        {
            st.godMode = false;
            st.infStamina = false;
            st.infMountStamina = false;
            st.infSpirit = false;
            st.oneHitKill = false;
            st.noFallDamage = false;
            st.dmgOutMult = 1.0f;
            st.dmgInMult = 1.0f;
            ui::Toast("All combat cheats disabled");
        }
        ui::End();
    }
}
