#include "mod.h"
#include <MinHook.h>
#include "logger.h"
#include "state.h"
#include "localization.h"
#include "version_detect.h"
#include "compat.h"
#include "readiness.h"
#include "../hooks/dx12_hook.h"
#include "../game/player.h"
#include "../game/focused_tick.h"
#include "../game/offsets.h"
#include "../mem/scanner.h"
namespace trinity {
void Mod::Initialize(HMODULE module) {
    if (m_initialized) return;
    m_module=module;
    LOG("Focused Combat Menu based on Trinity - supports %s (PE %s).",
        core::kSupportedTitle, core::kSupportedPE);
    loc::Init();
    State::Get() = State{};
    State::Get().autoSave = false;
    State::Get().showConsole = false;
    if (MH_Initialize()!=MH_OK) {
        LOG_ERR("MinHook failed to initialize; the menu cannot load.");
        return;
    }
    // The overlay goes up before the version is judged. On an unsupported
    // build it is the only thing that can say so - without it a mismatch looks
    // exactly like a broken install: nothing on screen and nothing to read.
    if (!hooks::InstallDX12Hooks()) {
        LOG_ERR("DX12 hooks failed to install; the menu cannot draw.");
        MH_Uninitialize();
        return;
    }
    if (!core::GameVersionSupported()) {
        m_initialized=true;
        LOG_ERR("Unsupported game version %s [PE %s] - this build requires %s (PE %s). "
                "No gameplay hooks installed; the overlay will say so on screen.",
                core::GetGameVersionDisplay(), core::GetGameVersion().rawVersionStr,
                core::kSupportedTitle, core::kSupportedPE);
        return;
    }
    const bool ready=core::WaitForReadiness([] {
        return mem::FindPattern(game::kSig_DamageApply) != 0 &&
               mem::FindPattern(game::kSig_MoveUpdate) != 0;
    }, [] {return GetTickCount64();}, [](uint32_t ms){Sleep(ms);},180000,2000);
    bool combatReady=false;
    if (!ready) {
        LOG_ERR("Combat hooks unavailable: gameplay code never became scannable.");
    } else if (!game::Player::Install()) {
        LOG_ERR("Combat hooks unavailable: the player hooks failed to install.");
    } else if (!game::InstallFocusedTick()) {
        game::Player::Remove();
        LOG_ERR("Combat hooks unavailable: the per-frame driver failed to install.");
    } else {
        combatReady=true;
    }
    m_initialized=true;
    if (combatReady)
        LOG_OK("Ready. TILDE/BACKTICK (no Shift) or LB + D-pad Down opens the combat menu. All cheats start OFF.");
    else
        LOG_ERR("The menu will open but every combat option is inactive.");
}
void Mod::Shutdown() {
    if (!m_initialized) return;
    game::RemoveFocusedTick();
    game::Player::Remove();
    hooks::RemoveDX12Hooks();
    MH_Uninitialize();
    Logger::Shutdown();
    m_initialized=false;
}
}
