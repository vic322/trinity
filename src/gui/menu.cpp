#include "menu.h"

#include <imgui.h>
#include <cstdio>

#include "framework.h"
#include "widgets.h"
#include "../core/state.h"
#include "../game/player.h"

// Focused personal menu built on Trinity's existing UI and player hooks.
// Original project attribution and license remain in the repository.
namespace trinity::gui
{
    bool WantsDraw()
    {
        return State::Get().menuOpen || ui::ToastsActive();
    }

    void Render()
    {
        State& st = State::Get();
        const char* const tabs[] = { "COMBAT" };
        ui::SetTabs(tabs, 1);
        ImGui::GetIO().MouseDrawCursor = false;
        ui::DrawToasts();
        if (!st.menuOpen)
            return;

        // This read also requests character tracking while every cheat is off,
        // so the menu can report readiness before a feature is enabled.
        const int tracked = game::Player::GetTrackedPlayerCount();
        const bool ready = game::Player::Ready();
        char status[96];
        if (ready)
            std::snprintf(status, sizeof(status), "Player ready | %d character%s tracked",
                          tracked, tracked == 1 ? "" : "s");
        else
            std::snprintf(status, sizeof(status), "Waiting for player - load into gameplay");

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
